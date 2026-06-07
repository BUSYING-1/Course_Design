#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>

#define BUFFER_SIZE 5
#define PRODUCER_COUNT 2
#define CONSUMER_COUNT 2
#define ITEMS_PER_PRODUCER 8
#define TOTAL_ITEMS (PRODUCER_COUNT * ITEMS_PER_PRODUCER)

#define READER_COUNT 3
#define WRITER_COUNT 2
#define PHILOSOPHER_COUNT 5
#define EAT_TIMES 3

/* =========================
 * 通用输出互斥锁
 * ========================= */
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

void safe_print(const char* fmt, ...) {
    va_list args;
    pthread_mutex_lock(&print_mutex);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&print_mutex);
}

/* =========================
 * 一、生产者-消费者
 * ========================= */
int buffer[BUFFER_SIZE];
int in_pos = 0;
int out_pos = 0;

sem_t empty_slots;
sem_t full_slots;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

void* producer_thread(void* arg) {
    int id = *(int*)arg;

    for (int i = 0; i < ITEMS_PER_PRODUCER; i++) {
        int item = id * 100 + i;

        sem_wait(&empty_slots);
        pthread_mutex_lock(&buffer_mutex);

        buffer[in_pos] = item;
        safe_print("[Producer %d] produce %d -> buffer[%d]\n", id, item, in_pos);
        in_pos = (in_pos + 1) % BUFFER_SIZE;

        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&full_slots);

        sleep(1);
    }

    safe_print("[Producer %d] finished.\n", id);
    return NULL;
}

void* consumer_thread(void* arg) {
    int id = *(int*)arg;

    for (int i = 0; i < TOTAL_ITEMS / CONSUMER_COUNT; i++) {
        sem_wait(&full_slots);
        pthread_mutex_lock(&buffer_mutex);

        int item = buffer[out_pos];
        safe_print("                 [Consumer %d] consume %d <- buffer[%d]\n", id, item, out_pos);
        out_pos = (out_pos + 1) % BUFFER_SIZE;

        pthread_mutex_unlock(&buffer_mutex);
        sem_post(&empty_slots);

        sleep(1);
    }

    safe_print("[Consumer %d] finished.\n", id);
    return NULL;
}

void demo_producer_consumer() {
    pthread_t producers[PRODUCER_COUNT];
    pthread_t consumers[CONSUMER_COUNT];
    int pids[PRODUCER_COUNT];
    int cids[CONSUMER_COUNT];

    in_pos = 0;
    out_pos = 0;
    for (int i = 0; i < BUFFER_SIZE; i++) buffer[i] = -1;

    sem_init(&empty_slots, 0, BUFFER_SIZE);
    sem_init(&full_slots, 0, 0);

    safe_print("\n========== 生产者-消费者 ==========\n");

    for (int i = 0; i < PRODUCER_COUNT; i++) {
        pids[i] = i + 1;
        pthread_create(&producers[i], NULL, producer_thread, &pids[i]);
    }

    for (int i = 0; i < CONSUMER_COUNT; i++) {
        cids[i] = i + 1;
        pthread_create(&consumers[i], NULL, consumer_thread, &cids[i]);
    }

    for (int i = 0; i < PRODUCER_COUNT; i++) pthread_join(producers[i], NULL);
    for (int i = 0; i < CONSUMER_COUNT; i++) pthread_join(consumers[i], NULL);

    sem_destroy(&empty_slots);
    sem_destroy(&full_slots);

    safe_print("Producer-Consumer demo finished.\n");
}

/* =========================
 * 二、读者-写者
 * 选择模式：
 * 1. 读者优先
 * 2. 写者优先
 * ========================= */

 /* -------- 读者优先 -------- */
int rp_read_count = 0;
sem_t rp_resource;
sem_t rp_rmutex;

void* rp_reader_thread(void* arg) {
    int id = *(int*)arg;

    for (int i = 0; i < 3; i++) {
        sem_wait(&rp_rmutex);
        rp_read_count++;
        if (rp_read_count == 1) {
            sem_wait(&rp_resource);
        }
        sem_post(&rp_rmutex);

        safe_print("[Reader %d] is reading (round %d)\n", id, i + 1);
        sleep(1);

        sem_wait(&rp_rmutex);
        rp_read_count--;
        if (rp_read_count == 0) {
            sem_post(&rp_resource);
        }
        sem_post(&rp_rmutex);

        sleep(1);
    }

    safe_print("[Reader %d] finished.\n", id);
    return NULL;
}

void* rp_writer_thread(void* arg) {
    int id = *(int*)arg;

    for (int i = 0; i < 3; i++) {
        sem_wait(&rp_resource);
        safe_print("                    [Writer %d] is writing (round %d)\n", id, i + 1);
        sleep(2);
        sem_post(&rp_resource);

        sleep(1);
    }

    safe_print("[Writer %d] finished.\n", id);
    return NULL;
}

void demo_readers_writers_reader_priority() {
    pthread_t readers[READER_COUNT];
    pthread_t writers[WRITER_COUNT];
    int rids[READER_COUNT];
    int wids[WRITER_COUNT];

    rp_read_count = 0;
    sem_init(&rp_resource, 0, 1);
    sem_init(&rp_rmutex, 0, 1);

    safe_print("\n========== 读者-写者（读者优先） ==========\n");

    for (int i = 0; i < READER_COUNT; i++) {
        rids[i] = i + 1;
        pthread_create(&readers[i], NULL, rp_reader_thread, &rids[i]);
    }

    for (int i = 0; i < WRITER_COUNT; i++) {
        wids[i] = i + 1;
        pthread_create(&writers[i], NULL, rp_writer_thread, &wids[i]);
    }

    for (int i = 0; i < READER_COUNT; i++) pthread_join(readers[i], NULL);
    for (int i = 0; i < WRITER_COUNT; i++) pthread_join(writers[i], NULL);

    sem_destroy(&rp_resource);
    sem_destroy(&rp_rmutex);

    safe_print("Readers-Writers (Reader Priority) demo finished.\n");
}

/* -------- 写者优先 -------- */
int wp_read_count = 0;
int wp_write_count = 0;

sem_t wp_resource;
sem_t wp_read_try;
sem_t wp_rmutex;
sem_t wp_wmutex;

void* wp_reader_thread(void* arg) {
    int id = *(int*)arg;

    for (int i = 0; i < 3; i++) {
        sem_wait(&wp_read_try);
        sem_wait(&wp_rmutex);
        wp_read_count++;
        if (wp_read_count == 1) {
            sem_wait(&wp_resource);
        }
        sem_post(&wp_rmutex);
        sem_post(&wp_read_try);

        safe_print("[Reader %d] is reading (round %d)\n", id, i + 1);
        sleep(1);

        sem_wait(&wp_rmutex);
        wp_read_count--;
        if (wp_read_count == 0) {
            sem_post(&wp_resource);
        }
        sem_post(&wp_rmutex);

        sleep(1);
    }

    safe_print("[Reader %d] finished.\n", id);
    return NULL;
}

void* wp_writer_thread(void* arg) {
    int id = *(int*)arg;

    for (int i = 0; i < 3; i++) {
        sem_wait(&wp_wmutex);
        wp_write_count++;
        if (wp_write_count == 1) {
            sem_wait(&wp_read_try);
        }
        sem_post(&wp_wmutex);

        sem_wait(&wp_resource);
        safe_print("                    [Writer %d] is writing (round %d)\n", id, i + 1);
        sleep(2);
        sem_post(&wp_resource);

        sem_wait(&wp_wmutex);
        wp_write_count--;
        if (wp_write_count == 0) {
            sem_post(&wp_read_try);
        }
        sem_post(&wp_wmutex);

        sleep(1);
    }

    safe_print("[Writer %d] finished.\n", id);
    return NULL;
}

void demo_readers_writers_writer_priority() {
    pthread_t readers[READER_COUNT];
    pthread_t writers[WRITER_COUNT];
    int rids[READER_COUNT];
    int wids[WRITER_COUNT];

    wp_read_count = 0;
    wp_write_count = 0;

    sem_init(&wp_resource, 0, 1);
    sem_init(&wp_read_try, 0, 1);
    sem_init(&wp_rmutex, 0, 1);
    sem_init(&wp_wmutex, 0, 1);

    safe_print("\n========== 读者-写者（写者优先） ==========\n");

    for (int i = 0; i < READER_COUNT; i++) {
        rids[i] = i + 1;
        pthread_create(&readers[i], NULL, wp_reader_thread, &rids[i]);
    }

    for (int i = 0; i < WRITER_COUNT; i++) {
        wids[i] = i + 1;
        pthread_create(&writers[i], NULL, wp_writer_thread, &wids[i]);
    }

    for (int i = 0; i < READER_COUNT; i++) pthread_join(readers[i], NULL);
    for (int i = 0; i < WRITER_COUNT; i++) pthread_join(writers[i], NULL);

    sem_destroy(&wp_resource);
    sem_destroy(&wp_read_try);
    sem_destroy(&wp_rmutex);
    sem_destroy(&wp_wmutex);

    safe_print("Readers-Writers (Writer Priority) demo finished.\n");
}

void demo_readers_writers() {
    int policy;
    printf("\n========== 读者-写者 ==========\n");
    printf("1. 读者优先\n");
    printf("2. 写者优先\n");
    printf("请选择优先策略：");

    if (scanf("%d", &policy) != 1) {
        printf("输入错误。\n");
        return;
    }

    if (policy == 1) {
        demo_readers_writers_reader_priority();
    }
    else if (policy == 2) {
        demo_readers_writers_writer_priority();
    }
    else {
        printf("无效选择。\n");
    }
}

/* =========================
 * 三、哲学家进餐
 * 采用 room = N-1 避免死锁
 * ========================= */
sem_t forks[PHILOSOPHER_COUNT];
sem_t room;

void* philosopher_thread(void* arg) {
    int id = *(int*)arg;
    int left = id;
    int right = (id + 1) % PHILOSOPHER_COUNT;

    for (int i = 0; i < EAT_TIMES; i++) {
        safe_print("[Philosopher %d] thinking (round %d)\n", id + 1, i + 1);
        sleep(1);

        sem_wait(&room);

        sem_wait(&forks[left]);
        sem_wait(&forks[right]);

        safe_print("      [Philosopher %d] eating with forks %d and %d (round %d)\n",
            id + 1, left + 1, right + 1, i + 1);
        sleep(2);

        sem_post(&forks[right]);
        sem_post(&forks[left]);
        sem_post(&room);

        sleep(1);
    }

    safe_print("[Philosopher %d] finished.\n", id + 1);
    return NULL;
}

void demo_philosophers() {
    pthread_t philosophers[PHILOSOPHER_COUNT];
    int pids[PHILOSOPHER_COUNT];

    sem_init(&room, 0, PHILOSOPHER_COUNT - 1);
    for (int i = 0; i < PHILOSOPHER_COUNT; i++) {
        sem_init(&forks[i], 0, 1);
    }

    safe_print("\n========== 哲学家进餐 ==========\n");

    for (int i = 0; i < PHILOSOPHER_COUNT; i++) {
        pids[i] = i;
        pthread_create(&philosophers[i], NULL, philosopher_thread, &pids[i]);
    }

    for (int i = 0; i < PHILOSOPHER_COUNT; i++) {
        pthread_join(philosophers[i], NULL);
    }

    sem_destroy(&room);
    for (int i = 0; i < PHILOSOPHER_COUNT; i++) {
        sem_destroy(&forks[i]);
    }

    safe_print("Dining Philosophers demo finished.\n");
}

/* =========================
 * 主菜单
 * ========================= */
int main() {
    srand((unsigned int)time(NULL));

    while (1) {
        int choice;
        printf("\n====================================\n");
        printf("     进程同步与并发控制模拟系统\n");
        printf("====================================\n");
        printf("1. 生产者-消费者\n");
        printf("2. 读者-写者\n");
        printf("3. 哲学家进餐\n");
        printf("0. 退出\n");
        printf("请选择：");

        if (scanf("%d", &choice) != 1) {
            printf("输入错误。\n");
            return 1;
        }

        if (choice == 0) {
            break;
        }
        else if (choice == 1) {
            demo_producer_consumer();
        }
        else if (choice == 2) {
            demo_readers_writers();
        }
        else if (choice == 3) {
            demo_philosophers();
        }
        else {
            printf("无效选择。\n");
        }
    }

    return 0;
}