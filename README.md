# Course Design of Operating System

本仓库为《操作系统》课程设计代码仓库，包含基础实验代码与扩展实验代码，主要用于进程调度、内存管理、进程同步、文件系统以及并发性能优化等内容的实现与验证。

---

## 一、项目内容

### 1. 基础实验

#### `scheduler.c`
进程调度基础实验代码，包含常见调度算法的实现与测试。

#### `memory_manager.c`
内存管理基础实验代码，包含内存分配与页面置换相关功能。

#### `sync_control.c`
进程同步与并发控制实验代码，包含线程、互斥锁、信号量等内容。

#### `real_fs_manager.c`
文件系统基础实验代码，包含文件创建、读写、删除与目录管理等功能。

---

### 2. 扩展实验

#### `os_scheduler_core.c`
扩展实验一：  
基于动态权重、分段 Aging 与公平性调度的 CPU 调度算法。  
同时支持实时任务调度（deadline / urgency）以及多场景性能测试与分析。

#### `extension3_pthread_optimizer.c`
扩展实验二：  
基于 pthread 的并发性能优化实验。  
通过多生产者、单消费者模型，对比粗粒度锁与批量取任务/缩短临界区两种策略的性能差异。

---

## 二、运行环境

- 操作系统：Windows 11 / WSL2 Ubuntu
- 开发语言：C
- 编译器：GCC
- 线程库：pthread
- 标准：C11

---

## 三、编译方式

### 1. 进程调度扩展实验
```bash
gcc -std=c11 -O2 -Wall -Wextra os_scheduler_core.c -lm -o scheduler_core
