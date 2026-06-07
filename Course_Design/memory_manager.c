#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HOLES 100
#define MAX_FRAMES 20
#define MAX_PAGES 200

/* =========================
   动态分区管理部分
   ========================= */
typedef struct {
    int id;         // 分区编号
    int start;      // 起始地址
    int size;       // 分区大小
    int free;       // 1空闲，0已分配
    int pid;        // 占用该分区的进程号，空闲时为-1
} Partition;

void init_partitions(Partition parts[], int* n) {
    *n = 1;
    parts[0].id = 1;
    parts[0].start = 0;
    parts[0].size = 1024;   // 默认给 1024KB 主存，可按需修改
    parts[0].free = 1;
    parts[0].pid = -1;
}

void print_partitions(Partition parts[], int n) {
    printf("\n");
    printf("%-6s %-10s %-10s %-10s %-10s\n",
        "ID", "Start", "Size(KB)", "State", "PID");

    for (int i = 0; i < n; i++) {
        if (parts[i].free) {
            printf("%-6d %-10d %-10d %-10s %-10s\n",
                parts[i].id,
                parts[i].start,
                parts[i].size,
                "Free",
                "-");
        }
        else {
            printf("%-6d %-10d %-10d %-10s %-10d\n",
                parts[i].id,
                parts[i].start,
                parts[i].size,
                "Used",
                parts[i].pid);
        }
    }
    printf("\n");
}

void merge_adjacent_free(Partition parts[], int* n) {
    for (int i = 0; i < *n - 1; i++) {
        if (parts[i].free && parts[i + 1].free) {
            parts[i].size += parts[i + 1].size;
            for (int j = i + 1; j < *n - 1; j++) {
                parts[j] = parts[j + 1];
            }
            (*n)--;
            i--;
        }
    }
}

int alloc_first_fit(Partition parts[], int* n, int pid, int req) {
    for (int i = 0; i < *n; i++) {
        if (parts[i].free && parts[i].size >= req) {
            if (parts[i].size == req) {
                parts[i].free = 0;
                parts[i].pid = pid;
            }
            else {
                for (int j = *n; j > i; j--) {
                    parts[j] = parts[j - 1];
                }
                (*n)++;
                parts[i + 1].id = parts[i].id + 1;
                parts[i + 1].start = parts[i].start + req;
                parts[i + 1].size = parts[i].size - req;
                parts[i + 1].free = 1;
                parts[i + 1].pid = -1;

                parts[i].size = req;
                parts[i].free = 0;
                parts[i].pid = pid;

                for (int k = i + 2; k < *n; k++) {
                    parts[k].id = k + 1;
                }
            }
            return 1;
        }
    }
    return 0;
}

int alloc_best_fit(Partition parts[], int* n, int pid, int req) {
    int idx = -1;
    int best = 1 << 30;
    for (int i = 0; i < *n; i++) {
        if (parts[i].free&& parts[i].size >= req && parts[i].size < best) {
            best = parts[i].size;
            idx = i;
        }
    }
    if (idx == -1) return 0;

    if (parts[idx].size == req) {
        parts[idx].free = 0;
        parts[idx].pid = pid;
    }
    else {
        for (int j = *n; j > idx; j--) {
            parts[j] = parts[j - 1];
        }
        (*n)++;
        parts[idx + 1].id = parts[idx].id + 1;
        parts[idx + 1].start = parts[idx].start + req;
        parts[idx + 1].size = parts[idx].size - req;
        parts[idx + 1].free = 1;
        parts[idx + 1].pid = -1;

        parts[idx].size = req;
        parts[idx].free = 0;
        parts[idx].pid = pid;

        for (int k = idx + 2; k < *n; k++) {
            parts[k].id = k + 1;
        }
    }
    return 1;
}

int free_partition(Partition parts[], int* n, int pid) {
    for (int i = 0; i < *n; i++) {
        if (!parts[i].free && parts[i].pid == pid) {
            parts[i].free = 1;
            parts[i].pid = -1;
            merge_adjacent_free(parts, n);
            return 1;
        }
    }
    return 0;
}

void dynamic_partition_menu() {
    Partition parts[MAX_HOLES];
    int n;
    init_partitions(parts, &n);

    while (1) {
        int choice;
        printf("\n========== 动态分区管理 ==========\n");
        printf("1. 查看内存分区\n");
        printf("2. 申请内存（首次适应 FF）\n");
        printf("3. 申请内存（最佳适应 BF）\n");
        printf("4. 释放内存\n");
        printf("0. 返回主菜单\n");
        printf("请选择：");
        scanf("%d", &choice);

        if (choice == 0) break;

        if (choice == 1) {
            print_partitions(parts, n);
        }
        else if (choice == 2 || choice == 3) {
            int pid, req;
            printf("输入进程号 PID：");
            scanf("%d", &pid);
            printf("输入申请内存大小(KB)：");
            scanf("%d", &req);

            int ok = (choice == 2)
                ? alloc_first_fit(parts, &n, pid, req)
                : alloc_best_fit(parts, &n, pid, req);

            if (ok) {
                printf("内存分配成功。\n");
            }
            else {
                printf("内存分配失败，空闲分区不足。\n");
            }
            print_partitions(parts, n);
        }
        else if (choice == 4) {
            int pid;
            printf("输入要释放的 PID：");
            scanf("%d", &pid);
            if (free_partition(parts, &n, pid)) {
                printf("释放成功。\n");
            }
            else {
                printf("未找到该进程占用的分区。\n");
            }
            print_partitions(parts, n);
        }
        else {
            printf("无效选择。\n");
        }
    }
}

/* =========================
   页面置换部分
   ========================= */
void print_frames(int frames[], int frameCount) {
    printf("Frames: ");
    for (int i = 0; i < frameCount; i++) {
        if (frames[i] == -1) printf("[ ] ");
        else printf("[%d] ", frames[i]);
    }
    printf("\n");
}

void fifo_replacement() {
    int frameCount, pageCount;
    int pages[MAX_PAGES];
    int frames[MAX_FRAMES];
    int next = 0;
    int faults = 0;

    printf("\n========== FIFO 页面置换 ==========\n");
    printf("请输入物理块数：");
    scanf("%d", &frameCount);
    printf("请输入页面访问序列长度：");
    scanf("%d", &pageCount);
    printf("请输入页面访问序列：\n");
    for (int i = 0; i < pageCount; i++) scanf("%d", &pages[i]);

    for (int i = 0; i < frameCount; i++) frames[i] = -1;

    for (int i = 0; i < pageCount; i++) {
        int hit = 0;
        for (int j = 0; j < frameCount; j++) {
            if (frames[j] == pages[i]) {
                hit = 1;
                break;
            }
        }

        if (!hit) {
            faults++;
            frames[next] = pages[i];
            next = (next + 1) % frameCount;
        }

        printf("访问页面 %d -> %s\n", pages[i], hit ? "Hit" : "Fault");
        print_frames(frames, frameCount);
    }

    printf("\n缺页次数：%d\n", faults);
    printf("缺页率：%.2f%%\n", 100.0 * faults / pageCount);
}

void lru_replacement() {
    int frameCount, pageCount;
    int pages[MAX_PAGES];
    int frames[MAX_FRAMES];
    int lastUsed[MAX_FRAMES];
    int faults = 0;

    printf("\n========== LRU 页面置换 ==========\n");
    printf("请输入物理块数：");
    scanf("%d", &frameCount);
    printf("请输入页面访问序列长度：");
    scanf("%d", &pageCount);
    printf("请输入页面访问序列：\n");
    for (int i = 0; i < pageCount; i++) scanf("%d", &pages[i]);

    for (int i = 0; i < frameCount; i++) {
        frames[i] = -1;
        lastUsed[i] = -1;
    }

    for (int t = 0; t < pageCount; t++) {
        int page = pages[t];
        int hitIndex = -1;
        int emptyIndex = -1;

        for (int i = 0; i < frameCount; i++) {
            if (frames[i] == page) {
                hitIndex = i;
                break;
            }
            if (frames[i] == -1 && emptyIndex == -1) {
                emptyIndex = i;
            }
        }

        if (hitIndex != -1) {
            lastUsed[hitIndex] = t;
            printf("访问页面 %d -> Hit\n", page);
        }
        else {
            faults++;
            if (emptyIndex != -1) {
                frames[emptyIndex] = page;
                lastUsed[emptyIndex] = t;
            }
            else {
                int lruIndex = 0;
                for (int i = 1; i < frameCount; i++) {
                    if (lastUsed[i] < lastUsed[lruIndex]) {
                        lruIndex = i;
                    }
                }
                frames[lruIndex] = page;
                lastUsed[lruIndex] = t;
            }
            printf("访问页面 %d -> Fault\n", page);
        }

        print_frames(frames, frameCount);
    }

    printf("\n缺页次数：%d\n", faults);
    printf("缺页率：%.2f%%\n", 100.0 * faults / pageCount);
}

void page_replacement_menu() {
    while (1) {
        int choice;
        printf("\n========== 页面置换 ==========\n");
        printf("1. FIFO\n");
        printf("2. LRU\n");
        printf("0. 返回主菜单\n");
        printf("请选择：");
        scanf("%d", &choice);

        if (choice == 0) break;
        else if (choice == 1) fifo_replacement();
        else if (choice == 2) lru_replacement();
        else printf("无效选择。\n");
    }
}

/* =========================
   主菜单
   ========================= */
int main() {
    while (1) {
        int choice;
        printf("\n====================================\n");
        printf("        内存管理模拟系统\n");
        printf("====================================\n");
        printf("1. 动态分区管理（FF / BF）\n");
        printf("2. 页面置换（FIFO / LRU）\n");
        printf("0. 退出\n");
        printf("请选择：");
        scanf("%d", &choice);

        if (choice == 0) break;
        else if (choice == 1) dynamic_partition_menu();
        else if (choice == 2) page_replacement_menu();
        else printf("无效选择。\n");
    }

    return 0;
}