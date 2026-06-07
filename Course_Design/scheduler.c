#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define MAXN 100

typedef struct {
    int pid;            // 进程编号
    int arrival;        // 到达时间
    int burst;          // 服务时间/运行时间
    int priority;       // 优先级（数值越小优先级越高）
    int remaining;      // RR用剩余时间
    int completion;     // 完成时间
    int turnaround;     // 周转时间
    double weighted;    // 带权周转时间
    int started;        // 是否已开始（RR中可用）
} Process;

typedef struct {
    int pid;
    int start;
    int end;
} Slice;

/* ---------- 工具函数 ---------- */
void print_process_table(Process p[], int n) {
    printf("\n%-6s%-8s%-8s%-10s%-10s%-10s%-12s\n",
        "PID", "AT", "BT", "PRIOR", "CT", "TAT", "WTAT");
    for (int i = 0; i < n; i++) {
        printf("P%-5d%-8d%-8d%-10d%-10d%-10d%-12.2f\n",
            p[i].pid, p[i].arrival, p[i].burst, p[i].priority,
            p[i].completion, p[i].turnaround, p[i].weighted);
    }
}

void print_avg(Process p[], int n) {
    double sum_tat = 0, sum_wtat = 0;
    for (int i = 0; i < n; i++) {
        sum_tat += p[i].turnaround;
        sum_wtat += p[i].weighted;
    }
    printf("\n平均周转时间 = %.2f\n", sum_tat / n);
    printf("平均带权周转时间 = %.2f\n", sum_wtat / n);
}

void sort_by_arrival_then_pid(Process p[], int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (p[j].arrival > p[j + 1].arrival ||
                (p[j].arrival == p[j + 1].arrival && p[j].pid > p[j + 1].pid)) {
                Process tmp = p[j];
                p[j] = p[j + 1];
                p[j + 1] = tmp;
            }
        }
    }
}

void reset_processes(Process src[], Process dst[], int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
        dst[i].remaining = dst[i].burst;
        dst[i].completion = 0;
        dst[i].turnaround = 0;
        dst[i].weighted = 0;
        dst[i].started = 0;
    }
}

void record_slice(Slice slices[], int* cnt, int pid, int start, int end) {
    slices[*cnt].pid = pid;
    slices[*cnt].start = start;
    slices[*cnt].end = end;
    (*cnt)++;
}

void print_gantt(Slice slices[], int cnt) {
    printf("\n甘特图（执行片段）:\n");
    for (int i = 0; i < cnt; i++) {
        if (slices[i].pid == 0) {
            printf("[idle:%d-%d] ", slices[i].start, slices[i].end);
        }
        else {
            printf("[P%d:%d-%d] ", slices[i].pid, slices[i].start, slices[i].end);
        }
    }
    printf("\n");
}

/* ---------- FCFS ---------- */
void fcfs(Process p[], int n) {
    sort_by_arrival_then_pid(p, n);
    int time = 0;
    Slice slices[MAXN * 2];
    int scnt = 0;

    for (int i = 0; i < n; i++) {
        if (time < p[i].arrival) {
            record_slice(slices, &scnt, 0, time, p[i].arrival);
            time = p[i].arrival;
        }
        record_slice(slices, &scnt, p[i].pid, time, time + p[i].burst);
        time += p[i].burst;
        p[i].completion = time;
        p[i].turnaround = p[i].completion - p[i].arrival;
        p[i].weighted = (double)p[i].turnaround / p[i].burst;
    }

    printf("\n========== FCFS 先来先服务 ==========\n");
    print_gantt(slices, scnt);
    print_process_table(p, n);
    print_avg(p, n);
}

/* ---------- SJF（非抢占） ---------- */
void sjf(Process p[], int n) {
    int done = 0, time = 0;
    int visited[MAXN] = { 0 };
    Slice slices[MAXN * 2];
    int scnt = 0;

    while (done < n) {
        int idx = -1;
        int minBurst = INT_MAX;

        for (int i = 0; i < n; i++) {
            if (!visited[i] && p[i].arrival <= time) {
                if (p[i].burst < minBurst ||
                    (p[i].burst == minBurst && p[i].arrival < p[idx].arrival)) {
                    minBurst = p[i].burst;
                    idx = i;
                }
            }
        }

        if (idx == -1) {
            int nextArrival = INT_MAX;
            for (int i = 0; i < n; i++) {
                if (!visited[i] && p[i].arrival < nextArrival) {
                    nextArrival = p[i].arrival;
                }
            }
            record_slice(slices, &scnt, 0, time, nextArrival);
            time = nextArrival;
            continue;
        }

        record_slice(slices, &scnt, p[idx].pid, time, time + p[idx].burst);
        time += p[idx].burst;
        p[idx].completion = time;
        p[idx].turnaround = p[idx].completion - p[idx].arrival;
        p[idx].weighted = (double)p[idx].turnaround / p[idx].burst;
        visited[idx] = 1;
        done++;
    }

    printf("\n========== SJF 短作业优先（非抢占） ==========\n");
    print_gantt(slices, scnt);
    print_process_table(p, n);
    print_avg(p, n);
}

/* ---------- 优先级调度（非抢占，数值越小优先级越高） ---------- */
void priority_scheduling(Process p[], int n) {
    int done = 0, time = 0;
    int visited[MAXN] = { 0 };
    Slice slices[MAXN * 2];
    int scnt = 0;

    while (done < n) {
        int idx = -1;
        int bestPr = INT_MAX;

        for (int i = 0; i < n; i++) {
            if (!visited[i] && p[i].arrival <= time) {
                if (p[i].priority < bestPr ||
                    (p[i].priority == bestPr && p[i].arrival < p[idx].arrival)) {
                    bestPr = p[i].priority;
                    idx = i;
                }
            }
        }

        if (idx == -1) {
            int nextArrival = INT_MAX;
            for (int i = 0; i < n; i++) {
                if (!visited[i] && p[i].arrival < nextArrival) {
                    nextArrival = p[i].arrival;
                }
            }
            record_slice(slices, &scnt, 0, time, nextArrival);
            time = nextArrival;
            continue;
        }

        record_slice(slices, &scnt, p[idx].pid, time, time + p[idx].burst);
        time += p[idx].burst;
        p[idx].completion = time;
        p[idx].turnaround = p[idx].completion - p[idx].arrival;
        p[idx].weighted = (double)p[idx].turnaround / p[idx].burst;
        visited[idx] = 1;
        done++;
    }

    printf("\n========== 优先级调度（非抢占，数值越小优先级越高） ==========\n");
    print_gantt(slices, scnt);
    print_process_table(p, n);
    print_avg(p, n);
}

/* ---------- RR 时间片轮转 ---------- */
void rr(Process p[], int n, int quantum) {
    int time = 0, done = 0;
    int q[MAXN * 10];
    int front = 0, rear = 0;
    int inqueue[MAXN] = { 0 };
    Slice slices[MAXN * 20];
    int scnt = 0;

    // 初始按到达时间排序，便于把进程按时间加入队列
    sort_by_arrival_then_pid(p, n);

    while (done < n) {
        // 加入当前时刻已到达的进程
        for (int i = 0; i < n; i++) {
            if (!inqueue[i] && p[i].arrival <= time && p[i].remaining > 0) {
                q[rear++] = i;
                inqueue[i] = 1;
            }
        }

        if (front == rear) {
            // 队列为空，CPU空闲，跳到下一个到达时间
            int nextArrival = INT_MAX;
            for (int i = 0; i < n; i++) {
                if (p[i].remaining > 0 && p[i].arrival > time && p[i].arrival < nextArrival) {
                    nextArrival = p[i].arrival;
                }
            }
            if (nextArrival == INT_MAX) break;
            record_slice(slices, &scnt, 0, time, nextArrival);
            time = nextArrival;
            continue;
        }

        int idx = q[front++];
        int run = (p[idx].remaining < quantum) ? p[idx].remaining : quantum;

        record_slice(slices, &scnt, p[idx].pid, time, time + run);
        time += run;
        p[idx].remaining -= run;

        // 在这个时间段里新到达的进程进入队列
        for (int i = 0; i < n; i++) {
            if (!inqueue[i] && p[i].arrival <= time && p[i].remaining > 0) {
                q[rear++] = i;
                inqueue[i] = 1;
            }
        }

        if (p[idx].remaining > 0) {
            q[rear++] = idx;  // 重新排队
        }
        else {
            p[idx].completion = time;
            p[idx].turnaround = p[idx].completion - p[idx].arrival;
            p[idx].weighted = (double)p[idx].turnaround / p[idx].burst;
            done++;
        }
    }

    printf("\n========== RR 时间片轮转 ==========\n");
    printf("时间片大小 q = %d\n", quantum);
    print_gantt(slices, scnt);
    print_process_table(p, n);
    print_avg(p, n);
}

/* ---------- 主函数 ---------- */
int main() {
    int n;
    Process origin[MAXN], work[MAXN];

    printf("请输入进程数量 n：");
    if (scanf("%d", &n) != 1 || n <= 0 || n > MAXN) {
        printf("输入错误。\n");
        return 1;
    }

    printf("\n请输入每个进程的信息：到达时间 执行时间 优先级\n");
    printf("说明：优先级数值越小，优先级越高\n\n");

    for (int i = 0; i < n; i++) {
        origin[i].pid = i + 1;
        printf("P%d: ", i + 1);
        if (scanf("%d %d %d", &origin[i].arrival, &origin[i].burst, &origin[i].priority) != 3) {
            printf("输入错误。\n");
            return 1;
        }
        origin[i].remaining = origin[i].burst;
        origin[i].completion = 0;
        origin[i].turnaround = 0;
        origin[i].weighted = 0;
        origin[i].started = 0;
    }

    int quantum;
    printf("\n请输入时间片大小 q（RR算法使用）：");
    if (scanf("%d", &quantum) != 1 || quantum <= 0) {
        printf("输入错误。\n");
        return 1;
    }

    while (1) {
        int choice;
        printf("\n=============================\n");
        printf("1. FCFS 先来先服务\n");
        printf("2. SJF 短作业优先（非抢占）\n");
        printf("3. RR 时间片轮转\n");
        printf("4. 优先级调度（非抢占）\n");
        printf("0. 退出\n");
        printf("请选择算法：");
        if (scanf("%d", &choice) != 1) break;

        if (choice == 0) break;

        reset_processes(origin, work, n);

        switch (choice) {
        case 1:
            fcfs(work, n);
            break;
        case 2:
            sjf(work, n);
            break;
        case 3:
            rr(work, n, quantum);
            break;
        case 4:
            priority_scheduling(work, n);
            break;
        default:
            printf("无效选择。\n");
        }
    }

    return 0;
}