/*
 * os_scheduler_core.c
 *
 * 核心主线：动态权重 + 分段 Aging + 公平性调度
 * 扩展 1：实时任务调度（deadline / urgency，支持 EDF 对比）
 * 扩展 2：系统性能测试与分析（多场景、多算法、多指标对比）
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#define PID_LEN 32
#define MAX_N 128
#define MAX_SEGMENTS 1024
#define MAX_LOG 1024
#define MAX_SCENARIO 64
#define W2_MIN 0.2
#define W2_MAX 0.8

typedef enum {
    ALG_FCFS = 0,
    ALG_SJF,
    ALG_RR,
    ALG_PRIORITY,
    ALG_EDF,
    ALG_DAFS,
    ALG_COUNT
} Algorithm;

typedef struct {
    char pid[PID_LEN];
    int arrival;
    int burst;
    double base_priority;
    int deadline;          /* < 0 表示无 deadline */
    int is_realtime;       /* 1 表示实时任务 */

    int remaining;
    int executed;
    int completion;
    int first_start;
    int started;
} Process;

typedef struct {
    char pid[PID_LEN];
    int start;
    int end;
} Segment;

typedef struct {
    int n;
    Process p[MAX_N];
    Segment timeline[MAX_SEGMENTS];
    int tl_count;
    char log[MAX_LOG][256];
    int log_count;
    int now;
    int context_switches;

    /* DAFS 参数 */
    int threshold_t;
    double alpha;
    double beta;
    double pmax;
    int quantum;
} Scheduler;

typedef struct {
    double avg_wait;
    double avg_tat;
    double avg_wtat;
    double avg_response;
    int max_wait;
    int context_switches;
    int deadline_miss;
    double avg_tardiness;
    int makespan;
} Metrics;

typedef struct {
    const char* name;
    Algorithm alg;
} AlgItem;

static void safe_copy(char* dst, size_t dstsz, const char* src) {
    if (dstsz == 0) return;
    snprintf(dst, dstsz, "%s", src);
}

static double clip_double(double x, double low, double high) {
    if (x < low) return low;
    if (x > high) return high;
    return x;
}

static double normalize_value(double x, double min_v, double max_v) {
    if (fabs(max_v - min_v) < 1e-12) return 0.5;
    return (x - min_v) / (max_v - min_v);
}

static void add_segment(Scheduler* s, const char* pid, int start, int end) {
    if (end <= start) return;
    if (s->tl_count > 0) {
        Segment* last = &s->timeline[s->tl_count - 1];
        if (strcmp(last->pid, pid) == 0 && last->end == start) {
            last->end = end;
            return;
        }
    }
    if (s->tl_count >= MAX_SEGMENTS) return;
    safe_copy(s->timeline[s->tl_count].pid, PID_LEN, pid);
    s->timeline[s->tl_count].start = start;
    s->timeline[s->tl_count].end = end;
    s->tl_count++;
}

static void add_log(Scheduler* s, const char* msg) {
    if (s->log_count >= MAX_LOG) return;
    safe_copy(s->log[s->log_count], sizeof(s->log[s->log_count]), msg);
    s->log_count++;
}

static void reset_scheduler(Scheduler* s) {
    s->tl_count = 0;
    s->log_count = 0;
    s->now = 0;
    s->context_switches = 0;
    for (int i = 0; i < s->n; ++i) {
        s->p[i].remaining = s->p[i].burst;
        s->p[i].executed = 0;
        s->p[i].completion = -1;
        s->p[i].first_start = -1;
        s->p[i].started = 0;
    }
}

static void init_process(Process* p, const char* pid, int at, int bt, double bp, int deadline, int rt) {
    safe_copy(p->pid, PID_LEN, pid);
    p->arrival = at;
    p->burst = bt;
    p->base_priority = bp;
    p->deadline = deadline;
    p->is_realtime = rt;
    p->remaining = bt;
    p->executed = 0;
    p->completion = -1;
    p->first_start = -1;
    p->started = 0;
}

static void init_default_set(Scheduler* s) {
    s->n = 5;
    init_process(&s->p[0], "P1", 0, 7, 3, 18, 0);
    init_process(&s->p[1], "P2", 2, 4, 5, 15, 1);
    init_process(&s->p[2], "P3", 4, 9, 1, 30, 0);
    init_process(&s->p[3], "P4", 6, 5, 4, 20, 1);
    init_process(&s->p[4], "P5", 8, 3, 2, 16, 1);
}

static int read_processes(Scheduler* s) {
    char buf[256];
    int n;

    printf("请输入进程数 n（直接回车使用默认样例）：");
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    if (buf[0] == '\n' || buf[0] == '\0') return 0;
    if (sscanf(buf, "%d", &n) != 1 || n <= 0 || n > MAX_N) {
        printf("输入无效，改用默认样例。\n");
        return 0;
    }

    s->n = n;
    printf("输入格式：到达时间 运行时间 初始优先级 deadline realtime(0/1)\n");
    printf("例如：0 7 3 18 0\n");
    for (int i = 0; i < n; ++i) {
        printf("第 %d 个进程：", i + 1);
        if (!fgets(buf, sizeof(buf), stdin)) return 0;
        int at, bt, dl, rt;
        double bp;
        if (sscanf(buf, "%d %d %lf %d %d", &at, &bt, &bp, &dl, &rt) != 5) {
            printf("格式错误，改用默认样例。\n");
            return 0;
        }
        char pid[PID_LEN];
        snprintf(pid, sizeof(pid), "P%d", i + 1);
        init_process(&s->p[i], pid, at, bt, bp, dl, rt);
    }
    return 1;
}

static int count_ready(const Scheduler* s) {
    int c = 0;
    for (int i = 0; i < s->n; ++i) {
        if (s->p[i].arrival <= s->now && s->p[i].remaining > 0) c++;
    }
    return c;
}

static void ready_indices(const Scheduler* s, int idxs[], int* cnt) {
    int k = 0;
    for (int i = 0; i < s->n; ++i) {
        if (s->p[i].arrival <= s->now && s->p[i].remaining > 0) idxs[k++] = i;
    }
    *cnt = k;
}

static double dynamic_priority(const Scheduler* s, const Process* p) {
    int waiting = s->now - p->arrival - p->executed;
    if (waiting < 0) waiting = 0;

    double v;
    if (waiting <= s->threshold_t) {
        v = p->base_priority + s->alpha * waiting;
    }
    else {
        double x = (double)(waiting - s->threshold_t);
        v = p->base_priority + s->alpha * s->threshold_t + s->beta * x * x;
    }
    if (v > s->pmax) v = s->pmax;
    return v;
}

static double fairness_score(const Process* p, int now) {
    int waiting = now - p->arrival - p->executed;
    if (waiting < 0) waiting = 0;
    int remaining = p->remaining < 0 ? 0 : p->remaining;
    return (waiting + 1.0) / (remaining + 1.0);
}

static double urgency_score(const Process* p, int now) {
    if (!p->is_realtime || p->deadline < 0) return 0.0;
    int remain_to_deadline = p->deadline - now;
    if (remain_to_deadline < 0) remain_to_deadline = 0;
    return 1.0 / (remain_to_deadline + 1.0);
}

static int cmp_fcfs(const void* a, const void* b, void* arg) {
    const Scheduler* s = (const Scheduler*)arg;
    int ia = *(const int*)a;
    int ib = *(const int*)b;
    const Process* pa = &s->p[ia];
    const Process* pb = &s->p[ib];
    if (pa->arrival != pb->arrival) return pa->arrival - pb->arrival;
    if (pa->executed != pb->executed) return pa->executed - pb->executed;
    return strcmp(pa->pid, pb->pid);
}

static int choose_fcfs(const Scheduler* s, const int idxs[], int cnt) {
    int best = idxs[0];
    for (int i = 1; i < cnt; ++i) {
        int j = idxs[i];
        const Process* a = &s->p[j];
        const Process* b = &s->p[best];
        if (a->arrival < b->arrival ||
            (a->arrival == b->arrival && a->executed < b->executed) ||
            (a->arrival == b->arrival && a->executed == b->executed && strcmp(a->pid, b->pid) < 0)) {
            best = j;
        }
    }
    return best;
}

static int choose_sjf(const Scheduler* s, const int idxs[], int cnt) {
    int best = idxs[0];
    for (int i = 1; i < cnt; ++i) {
        int j = idxs[i];
        const Process* a = &s->p[j];
        const Process* b = &s->p[best];
        if (a->burst < b->burst ||
            (a->burst == b->burst && a->arrival < b->arrival) ||
            (a->burst == b->burst && a->arrival == b->arrival && strcmp(a->pid, b->pid) < 0)) {
            best = j;
        }
    }
    return best;
}

static int choose_priority(const Scheduler* s, const int idxs[], int cnt) {
    int best = idxs[0];
    for (int i = 1; i < cnt; ++i) {
        int j = idxs[i];
        const Process* a = &s->p[j];
        const Process* b = &s->p[best];
        if (a->base_priority > b->base_priority ||
            (fabs(a->base_priority - b->base_priority) < 1e-12 && a->arrival < b->arrival) ||
            (fabs(a->base_priority - b->base_priority) < 1e-12 && a->arrival == b->arrival && strcmp(a->pid, b->pid) < 0)) {
            best = j;
        }
    }
    return best;
}

static int choose_edf(const Scheduler* s, const int idxs[], int cnt) {
    int best_rt = -1;
    int best_nonrt = -1;
    for (int i = 0; i < cnt; ++i) {
        int j = idxs[i];
        if (s->p[j].is_realtime && s->p[j].deadline >= 0) {
            if (best_rt < 0 || s->p[j].deadline < s->p[best_rt].deadline ||
                (s->p[j].deadline == s->p[best_rt].deadline && strcmp(s->p[j].pid, s->p[best_rt].pid) < 0)) {
                best_rt = j;
            }
        }
        else {
            if (best_nonrt < 0 || s->p[j].arrival < s->p[best_nonrt].arrival ||
                (s->p[j].arrival == s->p[best_nonrt].arrival && strcmp(s->p[j].pid, s->p[best_nonrt].pid) < 0)) {
                best_nonrt = j;
            }
        }
    }
    return best_rt >= 0 ? best_rt : idxs[0];
}

static int choose_dafs(const Scheduler* s, const int idxs[], int cnt, double* out_w1, double* out_w2,
    double* out_score, double* out_wait_pressure, double* out_urgency) {
    double pri[MAX_N], fair[MAX_N], urg[MAX_N];
    double npri[MAX_N], nfair[MAX_N], nurg[MAX_N];
    double min_pri = 1e100, max_pri = -1e100;
    double min_fair = 1e100, max_fair = -1e100;
    double min_urg = 1e100, max_urg = -1e100;

    for (int i = 0; i < cnt; ++i) {
        const Process* p = &s->p[idxs[i]];
        pri[i] = dynamic_priority(s, p);
        fair[i] = fairness_score(p, s->now);
        urg[i] = urgency_score(p, s->now);

        if (pri[i] < min_pri) min_pri = pri[i];
        if (pri[i] > max_pri) max_pri = pri[i];
        if (fair[i] < min_fair) min_fair = fair[i];
        if (fair[i] > max_fair) max_fair = fair[i];
        if (urg[i] < min_urg) min_urg = urg[i];
        if (urg[i] > max_urg) max_urg = urg[i];
    }

    int rt_count = 0;
    double avg_wait = 0.0;
    for (int i = 0; i < cnt; ++i) {
        const Process* p = &s->p[idxs[i]];
        int w = s->now - p->arrival - p->executed;
        if (w < 0) w = 0;
        avg_wait += w;
        if (p->is_realtime) rt_count++;
    }
    avg_wait /= cnt;

    /* 扩展 1：实时任务加入紧迫度；扩展 2：压力越大，公平性权重越高 */
    double wait_pressure = avg_wait / (avg_wait + s->threshold_t + 1.0);
    double rt_pressure = (double)rt_count / (double)cnt;
    double urgency_pressure = 0.0;
    for (int i = 0; i < cnt; ++i) urgency_pressure = fmax(urgency_pressure, urg[i]);

    double w2 = 0.20 + 0.35 * wait_pressure + 0.15 * rt_pressure + 0.15 * urgency_pressure;
    w2 = clip_double(w2, W2_MIN, W2_MAX);
    double w1 = 1.0 - w2;

    int best = 0;
    double best_score = -1e100;
    double best_npri = -1e100;
    double best_nfair = -1e100;
    double best_nurg = -1e100;

    for (int i = 0; i < cnt; ++i) {
        npri[i] = normalize_value(pri[i], min_pri, max_pri);
        nfair[i] = normalize_value(fair[i], min_fair, max_fair);
        nurg[i] = normalize_value(urg[i], min_urg, max_urg);

        double score = w1 * npri[i] + w2 * nfair[i] + 0.15 * nurg[i];
        const Process* p = &s->p[idxs[i]];

        if (score > best_score ||
            (fabs(score - best_score) < 1e-12 && nurg[i] > best_nurg) ||
            (fabs(score - best_score) < 1e-12 && fabs(nurg[i] - best_nurg) < 1e-12 && npri[i] > best_npri) ||
            (fabs(score - best_score) < 1e-12 && fabs(nurg[i] - best_nurg) < 1e-12 && fabs(npri[i] - best_npri) < 1e-12 && strcmp(p->pid, s->p[idxs[best]].pid) < 0)) {
            best = i;
            best_score = score;
            best_npri = npri[i];
            best_nfair = nfair[i];
            best_nurg = nurg[i];
        }
    }

    if (out_w1) *out_w1 = w1;
    if (out_w2) *out_w2 = w2;
    if (out_score) *out_score = best_score;
    if (out_wait_pressure) *out_wait_pressure = wait_pressure;
    if (out_urgency) *out_urgency = urgency_pressure;
    (void)best_nfair;
    return idxs[best];
}

static int simulate(Scheduler* base, Algorithm alg, int quantum, Metrics* m, bool verbose) {
    Scheduler s = *base;
    reset_scheduler(&s);
    s.quantum = quantum > 0 ? quantum : 1;

    int finished = 0;
    while (finished < s.n) {
        int idxs[MAX_N];
        int cnt = 0;
        ready_indices(&s, idxs, &cnt);

        if (cnt == 0) {
            add_segment(&s, "IDLE", s.now, s.now + 1);
            s.now += 1;
            continue;
        }

        int chosen = -1;
        int run_for = 0;
        double w1 = 0.0, w2 = 0.0, score = 0.0, wait_pressure = 0.0, urg_p = 0.0;

        switch (alg) {
        case ALG_FCFS:
            chosen = choose_fcfs(&s, idxs, cnt);
            run_for = s.p[chosen].remaining;
            break;
        case ALG_SJF:
            chosen = choose_sjf(&s, idxs, cnt);
            run_for = s.p[chosen].remaining;
            break;
        case ALG_PRIORITY:
            chosen = choose_priority(&s, idxs, cnt);
            run_for = s.p[chosen].remaining;
            break;
        case ALG_EDF:
            chosen = choose_edf(&s, idxs, cnt);
            run_for = s.p[chosen].remaining;
            break;
        case ALG_RR:
            chosen = idxs[0];
            for (int i = 1; i < cnt; ++i) {
                if (s.p[idxs[i]].arrival < s.p[chosen].arrival ||
                    (s.p[idxs[i]].arrival == s.p[chosen].arrival && strcmp(s.p[idxs[i]].pid, s.p[chosen].pid) < 0)) {
                    chosen = idxs[i];
                }
            }
            run_for = s.quantum < s.p[chosen].remaining ? s.quantum : s.p[chosen].remaining;
            break;
        case ALG_DAFS:
            chosen = choose_dafs(&s, idxs, cnt, &w1, &w2, &score, &wait_pressure, &urg_p);
            run_for = s.quantum < s.p[chosen].remaining ? s.quantum : s.p[chosen].remaining;
            break;
        default:
            chosen = choose_fcfs(&s, idxs, cnt);
            run_for = s.p[chosen].remaining;
            break;
        }
        if (run_for <= 0) run_for = 1;

        Process* p = &s.p[chosen];
        if (!p->started) {
            p->started = 1;
            p->first_start = s.now;
        }

        if (verbose) {
            char msg[256];
            if (alg == ALG_DAFS) {
                snprintf(msg, sizeof(msg), "t=%3d  选择 %-4s  run=%d  (w1=%.2f, w2=%.2f, waitP=%.3f, urgP=%.3f)",
                    s.now, p->pid, run_for, w1, w2, wait_pressure, urg_p);
            }
            else if (alg == ALG_EDF) {
                snprintf(msg, sizeof(msg), "t=%3d  选择 %-4s  run=%d  (EDF)", s.now, p->pid, run_for);
            }
            else {
                snprintf(msg, sizeof(msg), "t=%3d  选择 %-4s  run=%d", s.now, p->pid, run_for);
            }
            add_log(&s, msg);
        }

        add_segment(&s, p->pid, s.now, s.now + run_for);
        p->executed += run_for;
        p->remaining -= run_for;
        s.now += run_for;

        if (p->remaining == 0) {
            p->completion = s.now;
            finished++;
        }
        else {
            s.context_switches++;
        }
    }

    double sum_wait = 0.0, sum_tat = 0.0, sum_wtat = 0.0, sum_resp = 0.0, sum_tard = 0.0;
    int max_wait = 0;
    int deadline_miss = 0;
    int makespan = 0;

    for (int i = 0; i < s.n; ++i) {
        Process* p = &s.p[i];
        int tat = p->completion - p->arrival;
        int wt = tat - p->burst;
        int resp = p->first_start - p->arrival;
        double wtat = (double)tat / (double)p->burst;
        int tardiness = 0;
        if (p->deadline >= 0) {
            tardiness = p->completion - p->deadline;
            if (tardiness > 0) deadline_miss++;
            if (tardiness < 0) tardiness = 0;
        }
        if (wt > max_wait) max_wait = wt;
        sum_wait += wt;
        sum_tat += tat;
        sum_wtat += wtat;
        sum_resp += resp;
        sum_tard += tardiness;
        if (p->completion > makespan) makespan = p->completion;
    }

    m->avg_wait = sum_wait / s.n;
    m->avg_tat = sum_tat / s.n;
    m->avg_wtat = sum_wtat / s.n;
    m->avg_response = sum_resp / s.n;
    m->max_wait = max_wait;
    m->context_switches = s.context_switches;
    m->deadline_miss = deadline_miss;
    m->avg_tardiness = sum_tard / s.n;
    m->makespan = makespan;

    if (verbose) {
        printf("\n=== 调度过程日志 [%d] ===\n", alg);
        for (int i = 0; i < s.log_count; ++i) printf("%s\n", s.log[i]);
    }

    *base = s;
    return 0;
}

static const char* alg_name(Algorithm alg) {
    switch (alg) {
    case ALG_FCFS: return "FCFS";
    case ALG_SJF: return "SJF";
    case ALG_RR: return "RR";
    case ALG_PRIORITY: return "Priority";
    case ALG_EDF: return "EDF";
    case ALG_DAFS: return "DAFS";
    default: return "Unknown";
    }
}

static void print_process_table(const Scheduler* s) {
    printf("\n=== 进程数据 ===\n");
    printf("-----------------------------------------------------------------------------------------------\n");
    printf("%-8s%-6s%-6s%-8s%-8s%-10s%-10s\n", "PID", "AT", "BT", "BP", "DL", "RT", "Exec");
    printf("-----------------------------------------------------------------------------------------------\n");
    for (int i = 0; i < s->n; ++i) {
        const Process* p = &s->p[i];
        printf("%-8s%-6d%-6d%-8.1f%-8d%-10d%-10d\n", p->pid, p->arrival, p->burst, p->base_priority, p->deadline, p->is_realtime, p->executed);
    }
    printf("-----------------------------------------------------------------------------------------------\n");
}

static void print_summary(const Scheduler* s, const Metrics* m, Algorithm alg) {
    printf("\n[%s] 结果：\n", alg_name(alg));
    printf("平均等待时间      : %.2f\n", m->avg_wait);
    printf("平均周转时间      : %.2f\n", m->avg_tat);
    printf("平均带权周转时间  : %.2f\n", m->avg_wtat);
    printf("平均响应时间      : %.2f\n", m->avg_response);
    printf("最大等待时间      : %d\n", m->max_wait);
    printf("上下文切换次数    : %d\n", m->context_switches);
    printf("deadline miss     : %d\n", m->deadline_miss);
    printf("平均超期时间      : %.2f\n", m->avg_tardiness);
    printf("完工时间 makespan  : %d\n", m->makespan);

    printf("\n文本甘特图：\n");
    for (int i = 0; i < s->tl_count; ++i) {
        printf("[%3d, %3d)  %s\n", s->timeline[i].start, s->timeline[i].end, s->timeline[i].pid);
    }
}

static void run_single_mode(void) {
    Scheduler s;
    memset(&s, 0, sizeof(s));
    s.threshold_t = 5;
    s.alpha = 1.0;
    s.beta = 0.5;
    s.pmax = 1000.0;
    s.quantum = 2;

    printf("=== 动态权重 + 分段 Aging + 公平性调度（核心 + 实时扩展 + 对比分析）===\n");
    printf("提示：回车直接使用默认样例。\n");

    if (!read_processes(&s)) {
        printf("使用默认样例。\n");
        init_default_set(&s);
    }

    char buf[256];
    printf("\n参数设置（直接回车使用默认值）：\n");
    printf("临界时间 T（默认 5）：");
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n' && buf[0] != '\0') {
        int T;
        if (sscanf(buf, "%d", &T) == 1 && T >= 0) s.threshold_t = T;
    }
    printf("线性系数 α（默认 1.0）：");
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n' && buf[0] != '\0') {
        double a;
        if (sscanf(buf, "%lf", &a) == 1) s.alpha = a;
    }
    printf("平方系数 β（默认 0.5）：");
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n' && buf[0] != '\0') {
        double b;
        if (sscanf(buf, "%lf", &b) == 1) s.beta = b;
    }
    printf("优先级上限 Pmax（默认 1000）：");
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n' && buf[0] != '\0') {
        double pm;
        if (sscanf(buf, "%lf", &pm) == 1) s.pmax = pm;
    }
    printf("时间片 quantum（默认 2）：");
    if (fgets(buf, sizeof(buf), stdin) && buf[0] != '\n' && buf[0] != '\0') {
        int q;
        if (sscanf(buf, "%d", &q) == 1 && q > 0) s.quantum = q;
    }

    print_process_table(&s);

    Metrics m;
    Algorithm algs[] = { ALG_FCFS, ALG_SJF, ALG_RR, ALG_PRIORITY, ALG_EDF, ALG_DAFS };
    int alg_cnt = (int)(sizeof(algs) / sizeof(algs[0]));

    for (int i = 0; i < alg_cnt; ++i) {
        Scheduler cur = s;
        Metrics curm;
        int q = (algs[i] == ALG_RR || algs[i] == ALG_DAFS) ? s.quantum : s.quantum;
        simulate(&cur, algs[i], q, &curm, algs[i] == ALG_DAFS);
        print_summary(&cur, &curm, algs[i]);
        if (algs[i] == ALG_DAFS) m = curm;
    }

    (void)m;
}

/* 扩展 2：多场景批量测试 */
static void fill_scenario_short_jobs(Scheduler* s) {
    s->n = 6;
    init_process(&s->p[0], "P1", 0, 2, 3, 8, 0);
    init_process(&s->p[1], "P2", 1, 3, 4, 9, 0);
    init_process(&s->p[2], "P3", 2, 2, 5, 10, 1);
    init_process(&s->p[3], "P4", 3, 4, 1, 12, 0);
    init_process(&s->p[4], "P5", 4, 3, 2, 11, 1);
    init_process(&s->p[5], "P6", 5, 2, 3, 13, 0);
}

static void fill_scenario_long_jobs(Scheduler* s) {
    s->n = 6;
    init_process(&s->p[0], "P1", 0, 10, 3, 18, 0);
    init_process(&s->p[1], "P2", 1, 9, 4, 17, 1);
    init_process(&s->p[2], "P3", 2, 8, 1, 20, 0);
    init_process(&s->p[3], "P4", 3, 7, 2, 19, 1);
    init_process(&s->p[4], "P5", 4, 9, 5, 22, 1);
    init_process(&s->p[5], "P6", 5, 8, 3, 21, 0);
}

static void fill_scenario_clustered_arrival(Scheduler* s) {
    s->n = 6;
    init_process(&s->p[0], "P1", 0, 5, 2, 12, 0);
    init_process(&s->p[1], "P2", 0, 4, 4, 11, 1);
    init_process(&s->p[2], "P3", 0, 6, 1, 15, 0);
    init_process(&s->p[3], "P4", 1, 3, 5, 10, 1);
    init_process(&s->p[4], "P5", 1, 7, 3, 18, 0);
    init_process(&s->p[5], "P6", 1, 2, 4, 9, 1);
}

static void fill_scenario_sparse_arrival(Scheduler* s) {
    s->n = 6;
    init_process(&s->p[0], "P1", 0, 4, 3, 10, 0);
    init_process(&s->p[1], "P2", 3, 5, 4, 14, 1);
    init_process(&s->p[2], "P3", 6, 3, 2, 13, 1);
    init_process(&s->p[3], "P4", 9, 6, 5, 20, 0);
    init_process(&s->p[4], "P5", 12, 2, 1, 18, 0);
    init_process(&s->p[5], "P6", 15, 4, 3, 24, 1);
}

static void benchmark_mode(void) {
    Scheduler base;
    memset(&base, 0, sizeof(base));
    base.threshold_t = 5;
    base.alpha = 1.0;
    base.beta = 0.5;
    base.pmax = 1000.0;
    base.quantum = 2;

    AlgItem algs[] = {
        {"FCFS", ALG_FCFS},
        {"SJF", ALG_SJF},
        {"RR", ALG_RR},
        {"Priority", ALG_PRIORITY},
        {"EDF", ALG_EDF},
        {"DAFS", ALG_DAFS}
    };
    const int alg_cnt = (int)(sizeof(algs) / sizeof(algs[0]));

    struct {
        const char* name;
        void (*fill)(Scheduler*);
    } scenarios[] = {
        {"短作业密集", fill_scenario_short_jobs},
        {"长作业密集", fill_scenario_long_jobs},
        {"集中到达", fill_scenario_clustered_arrival},
        {"稀疏到达", fill_scenario_sparse_arrival}
    };
    const int sc_cnt = (int)(sizeof(scenarios) / sizeof(scenarios[0]));

    printf("=== 系统性能测试与分析（多场景、多算法、多指标对比）===\n");
    printf("说明：本模式用于课程设计中的扩展 2，自动输出对比结果。\n");

    for (int sidx = 0; sidx < sc_cnt; ++sidx) {
        printf("\n================ 场景：%s ================\n", scenarios[sidx].name);
        scenarios[sidx].fill(&base);

        printf("%-10s%-10s%-10s%-10s%-10s%-10s%-10s%-10s\n",
            "ALG", "WTime", "TAT", "WTAT", "Resp", "MaxW", "CSW", "Miss");
        printf("--------------------------------------------------------------------------------\n");

        for (int a = 0; a < alg_cnt; ++a) {
            Scheduler cur = base;
            Metrics m;
            simulate(&cur, algs[a].alg, base.quantum, &m, false);
            printf("%-10s%-10.2f%-10.2f%-10.2f%-10.2f%-10d%-10d%-10d\n",
                algs[a].name, m.avg_wait, m.avg_tat, m.avg_wtat,
                m.avg_response, m.max_wait, m.context_switches, m.deadline_miss);
        }
    }

    printf("\n提示：如果需要更漂亮的图表，可将这些结果导出到 CSV，再用 Excel / Python 作图。\n");
}

int main(void) {
    printf("=== 操作系统课程设计：调度算法扩展实验 ===\n");
    printf("1. 单组数据运行（含 FCFS/SJF/RR/Priority/EDF/DAFS 对比）\n");
    printf("2. 系统性能测试与分析（多场景批量对比）\n");
    printf("请选择模式：");

    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    int choice = atoi(buf);

    if (choice == 2) {
        benchmark_mode();
    }
    else {
        run_single_mode();
    }

    printf("\n扩展 3（pthread 并发性能优化实验）单独新建一个源文件实现，不与本程序耦合。\n");
    return 0;
}
