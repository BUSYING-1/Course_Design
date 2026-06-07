/*
 * extension3_pthread_optimizer.c
 *
 * 扩展 3：并发性能优化实验（高竞争版，独立 pthread 版本）
 *
 * 设计目标：
 * 1) 多生产者线程模拟任务高并发到达
 * 2) 单消费者线程模拟调度器从共享队列取任务
 * 3) 对比两种锁策略：
 *    - 粗粒度锁：每次只取 1 个任务，临界区更长
 *    - 批量取任务：一次取多个任务，缩短临界区
 * 4) 在高竞争场景下对比吞吐量、平均等待时间、锁等待时间、阻塞次数等指标
 *
 * 编译：
 *   gcc -std=c11 -O2 -Wall -Wextra extension3_pthread_optimizer.c -lpthread -lm -o ext3
 *
 * 运行：
 *   ./ext3
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_TASKS 4096
#define MAX_PRODUCERS 16
#define NAME_LEN 32

typedef enum {
    LOCK_COARSE = 0,
    LOCK_BATCH = 1
} LockMode;

typedef struct {
    int id;
    char name[NAME_LEN];
    int arrival_ms;
    int burst_ms;
    struct timespec enq_ts;
    struct timespec start_ts;
    struct timespec end_ts;
} Task;

typedef struct {
    Task data[MAX_TASKS];
    int head;
    int tail;
    int size;
} TaskQueue;

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    pthread_mutex_t stats_mtx;

    TaskQueue q;
    Task tasks[MAX_TASKS];
    int task_count;
    int next_task_idx;

    int producers_done;
    int producer_count;
    int stop;

    LockMode mode;

    long total_wait_ms;
    long total_exec_ms;
    long total_lock_wait_us;
    long total_block_count;
    long total_dequeued;
    long total_context_switch_like;
} SharedState;

static void now_ts(struct timespec* ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static long diff_ms(const struct timespec* a, const struct timespec* b) {
    long sec = (long)(b->tv_sec - a->tv_sec);
    long nsec = (long)(b->tv_nsec - a->tv_nsec);
    return sec * 1000L + nsec / 1000000L;
}

static long diff_us(const struct timespec* a, const struct timespec* b) {
    long sec = (long)(b->tv_sec - a->tv_sec);
    long nsec = (long)(b->tv_nsec - a->tv_nsec);
    return sec * 1000000L + nsec / 1000L;
}

static void ms_sleep(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* 轻微忙等，用来模拟队列管理开销，放大临界区影响 */
static void busy_spin_us(int us) {
    if (us <= 0) return;
    struct timespec start, cur;
    now_ts(&start);
    do {
        now_ts(&cur);
    } while (diff_us(&start, &cur) < us);
}

static void queue_init(TaskQueue* q) {
    q->head = q->tail = q->size = 0;
}

static int queue_push(TaskQueue* q, const Task* t) {
    if (q->size >= MAX_TASKS) return -1;
    q->data[q->tail] = *t;
    q->tail = (q->tail + 1) % MAX_TASKS;
    q->size++;
    return 0;
}

static int queue_pop(TaskQueue* q, Task* out) {
    if (q->size <= 0) return -1;
    *out = q->data[q->head];
    q->head = (q->head + 1) % MAX_TASKS;
    q->size--;
    return 0;
}

static int queue_pop_batch(TaskQueue* q, Task* out, int max_batch) {
    int cnt = 0;
    while (cnt < max_batch && q->size > 0) {
        out[cnt++] = q->data[q->head];
        q->head = (q->head + 1) % MAX_TASKS;
        q->size--;
    }
    return cnt;
}

static void init_demo_tasks(SharedState* st) {
    /*
     * 高竞争测试数据：
     * 1) 任务数量增大到 120
     * 2) 到达时间高度集中（0~6ms 之间循环）
     * 3) 执行时间缩短（1~3ms）
     * 4) 让生产者/消费者更容易发生锁竞争
     */
    st->task_count = 120;
    st->producer_count = 8;

    for (int i = 0; i < st->task_count; ++i) {
        Task* t = &st->tasks[i];
        memset(t, 0, sizeof(*t));
        t->id = i + 1;
        snprintf(t->name, sizeof(t->name), "T%d", t->id);

        /* 高度集中到达：大量任务在短时间窗口内涌入 */
        t->arrival_ms = (i % 7);              /* 0~6ms */
        /* 更短执行时间，放大锁竞争在总耗时中的占比 */
        t->burst_ms = 1 + (i % 3);             /* 1~3ms */
    }
}

static int fetch_next_task(SharedState* st, Task* out) {
    if (st->next_task_idx >= st->task_count) return 0;
    *out = st->tasks[st->next_task_idx++];
    return 1;
}

static void* producer_thread(void* arg) {
    SharedState* st = (SharedState*)arg;

    while (1) {
        Task t;
        pthread_mutex_lock(&st->stats_mtx);
        int has_task = fetch_next_task(st, &t);
        pthread_mutex_unlock(&st->stats_mtx);

        if (!has_task) break;

        /* 让任务在极短时间内集中到达 */
        ms_sleep(t.arrival_ms);

        struct timespec lock_begin, lock_end, enq_time;
        now_ts(&lock_begin);
        pthread_mutex_lock(&st->mtx);
        now_ts(&lock_end);

        /* 模拟入队管理开销：粗粒度锁下更明显 */
        if (st->mode == LOCK_COARSE) {
            busy_spin_us(120);
        }

        if (queue_push(&st->q, &t) == 0) {
            now_ts(&enq_time);
            st->q.data[(st->q.tail - 1 + MAX_TASKS) % MAX_TASKS].enq_ts = enq_time;
            pthread_cond_signal(&st->cv);
        }

        pthread_mutex_unlock(&st->mtx);

        pthread_mutex_lock(&st->stats_mtx);
        st->total_lock_wait_us += diff_us(&lock_begin, &lock_end);
        pthread_mutex_unlock(&st->stats_mtx);
    }

    pthread_mutex_lock(&st->stats_mtx);
    st->producers_done++;
    pthread_mutex_unlock(&st->stats_mtx);

    pthread_cond_broadcast(&st->cv);
    return NULL;
}

static void record_task_done(SharedState* st, const Task* t,
    const struct timespec* start, const struct timespec* end) {
    long wait_ms = diff_ms(&t->enq_ts, start);
    long exec_ms = diff_ms(start, end);

    pthread_mutex_lock(&st->stats_mtx);
    st->total_wait_ms += wait_ms;
    st->total_exec_ms += exec_ms;
    st->total_dequeued++;
    pthread_mutex_unlock(&st->stats_mtx);
}

static void execute_task_sim(const Task* t) {
    ms_sleep(t->burst_ms);
}

static void* consumer_thread(void* arg) {
    SharedState* st = (SharedState*)arg;

    while (1) {
        Task local_batch[16];
        int batch_cnt = 0;

        struct timespec lock_begin, lock_end;
        now_ts(&lock_begin);
        pthread_mutex_lock(&st->mtx);
        now_ts(&lock_end);

        while (st->q.size == 0 && st->producers_done < st->producer_count && !st->stop) {
            pthread_mutex_lock(&st->stats_mtx);
            st->total_block_count++;
            pthread_mutex_unlock(&st->stats_mtx);
            pthread_cond_wait(&st->cv, &st->mtx);
        }

        if (st->q.size == 0 && st->producers_done >= st->producer_count) {
            pthread_mutex_unlock(&st->mtx);
            break;
        }

        if (st->mode == LOCK_COARSE) {
            /* 粗粒度锁：一次只拿一个任务，且在锁内做少量管理工作 */
            batch_cnt = queue_pop(&st->q, &local_batch[0]) == 0 ? 1 : 0;
            busy_spin_us(80);
        }
        else {
            /* 优化思路：一次批量取多个任务，减少锁竞争 */
            batch_cnt = queue_pop_batch(&st->q, local_batch, 8);
        }

        pthread_mutex_unlock(&st->mtx);

        pthread_mutex_lock(&st->stats_mtx);
        st->total_lock_wait_us += diff_us(&lock_begin, &lock_end);
        if (batch_cnt > 0) st->total_context_switch_like += batch_cnt;
        pthread_mutex_unlock(&st->stats_mtx);

        for (int i = 0; i < batch_cnt; ++i) {
            Task* t = &local_batch[i];
            struct timespec start_ts, end_ts;
            now_ts(&start_ts);
            t->start_ts = start_ts;
            execute_task_sim(t);
            now_ts(&end_ts);
            t->end_ts = end_ts;
            record_task_done(st, t, &start_ts, &end_ts);
        }
    }

    return NULL;
}

static void print_config(const SharedState* st) {
    printf("=== 扩展 3：pthread 并发性能优化实验（高竞争版）===\n");
    printf("任务数：%d\n", st->task_count);
    printf("生产者线程数：%d\n", st->producer_count);
    printf("锁策略：%s\n", st->mode == LOCK_COARSE ? "粗粒度锁" : "批量取任务/缩短临界区");
    printf("----------------------------------------\n");
}

static void print_tasks(const SharedState* st) {
    printf("任务列表：\n");
    printf("%-6s %-8s %-8s\n", "Task", "Arr(ms)", "Burst(ms)");
    for (int i = 0; i < st->task_count; ++i) {
        printf("%-6s %-8d %-8d\n", st->tasks[i].name, st->tasks[i].arrival_ms, st->tasks[i].burst_ms);
    }
    printf("----------------------------------------\n");
}

static void print_results(const SharedState* st, const struct timespec* t0, const struct timespec* t1) {
    long total_time_ms = diff_ms(t0, t1);
    double avg_wait = st->total_dequeued ? (double)st->total_wait_ms / (double)st->total_dequeued : 0.0;
    double avg_exec = st->total_dequeued ? (double)st->total_exec_ms / (double)st->total_dequeued : 0.0;
    double throughput = total_time_ms > 0 ? (double)st->total_dequeued / ((double)total_time_ms / 1000.0) : 0.0;

    printf("=== 实验结果 ===\n");
    printf("总耗时(ms)            : %ld\n", total_time_ms);
    printf("完成任务数            : %ld\n", st->total_dequeued);
    printf("平均排队等待时间(ms)  : %.2f\n", avg_wait);
    printf("平均执行时间(ms)      : %.2f\n", avg_exec);
    printf("锁等待总时间(us)      : %ld\n", st->total_lock_wait_us);
    printf("线程阻塞次数          : %ld\n", st->total_block_count);
    printf("近似切换次数          : %ld\n", st->total_context_switch_like);
    printf("吞吐量(任务/秒)       : %.2f\n", throughput);
    printf("----------------------------------------\n");
}

static void run_once(LockMode mode) {
    SharedState st;
    memset(&st, 0, sizeof(st));

    pthread_mutex_init(&st.mtx, NULL);
    pthread_mutex_init(&st.stats_mtx, NULL);
    pthread_cond_init(&st.cv, NULL);
    queue_init(&st.q);

    init_demo_tasks(&st);
    st.mode = mode;

    print_config(&st);
    print_tasks(&st);

    struct timespec t0, t1;
    now_ts(&t0);

    pthread_t producers[MAX_PRODUCERS];
    pthread_t consumer;

    for (int i = 0; i < st.producer_count; ++i) {
        if (pthread_create(&producers[i], NULL, producer_thread, &st) != 0) {
            perror("pthread_create producer");
            exit(1);
        }
    }

    if (pthread_create(&consumer, NULL, consumer_thread, &st) != 0) {
        perror("pthread_create consumer");
        exit(1);
    }

    for (int i = 0; i < st.producer_count; ++i) {
        pthread_join(producers[i], NULL);
    }
    pthread_join(consumer, NULL);

    now_ts(&t1);
    print_results(&st, &t0, &t1);

    pthread_cond_destroy(&st.cv);
    pthread_mutex_destroy(&st.stats_mtx);
    pthread_mutex_destroy(&st.mtx);
}

int main(void) {
    printf("=== 并发性能优化实验（高竞争版）===\n");
    printf("1. 粗粒度锁\n");
    printf("2. 批量取任务/缩短临界区\n");
    printf("请选择模式：");

    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) return 0;
    int choice = atoi(buf);

    if (choice == 2) {
        run_once(LOCK_BATCH);
    }
    else {
        run_once(LOCK_COARSE);
    }

    return 0;
}
