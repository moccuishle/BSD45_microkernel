# 为什么 bsd45 内核固定只有 16 个 kthread

## 问题

内核里 `struct kthread kthreads[NKTHREAD];` 为何把 `NKTHREAD` 定死为 16?

## 结论速览

`NKTHREAD = 16` 是一个**编译期定死的静态数组**(`kernel/thread/kthread.h:21`、`kernel/thread/kthread.c:30`)。它不是"暂时设小了",而是这个教学内核的设计选择——TCB 池用固定槽位 + 固定地址布局换取实现简单。

槽位预算:

| 用途 | 槽数 |
|------|------|
| 8 个进程的主线程 TCB(`NPROC=8`) | 8 |
| softirq + aio 两个内核线程 | 2 |
| 空闲,给额外用户线程 | 6 |
| **合计** | **16** |

## 原因一:槽号被编码进虚拟地址,数量改了地址布局就崩

这是最硬的约束。`kstack_alloc()`(`kthread.c:100-113`)给第 `i` 个槽的内核栈地址是**算出来**的,不是动态申请的:

```c
unsigned char *va = (unsigned char *)(KSTACK_VA + i * KSTACK_SIZE);
// 0xFEA00000 + i × 8192
```

- `KSTACK_VA = 0xFEA00000`,`KSTACK_SIZE = 8192`(`kthread.h:48-49`)
- 每槽 8KB 内核栈,地址由下标唯一确定,启动时用 `pmap_map` 逐页映射
- 用户栈同理,`thread_create`(`usermode.c:4040`)用:

```c
sva = 0xBFA00000u + ((unsigned)(tid & 0xf) << 16);
```

`tid & 0xf` **只取低 4 位,天然就是 16 个槽**。

所以想把 16 改成 32,光改宏没用,还得改地址编码、映射范围和 tid 分配规则。

## 原因二:固定槽让调度器退化成线性扫描,没有动态内存管理

- `kthread_spawn()` / `kthread_user_spawn()` 就是 `for (i = 0; i < NKTHREAD; i++)` 找第一个 `!t_used` 的槽(`kthread.c:144,189`)
- `proc_sched()` 也是每轮 `for (i = 0; i < NKTHREAD; i++)` 遍历全部槽找可运行 TCB(`usermode.c:2535`)
- TCB 池是全局静态数组 `kthreads[16]`,`curk` 一个全局指针就够用
- 没有链表、没有 slab 分配器、没有锁竞争

对教学内核来说,可预测、好调试比可伸缩更有价值。

## 原因三:容量预算刚好够,用户态还有更小的上限

- 8 个进程槽(`NPROC=8`)各占 1 个主线程 TCB = 8 个
- softirq、aio 两个内核线程固定占 2 个
- 剩 6 个槽给额外用户线程
- libc 的 `pthread_create` 自带 `NTHREADS = 4` 上限(`libc/src/pthread.c`)

全系统约束写成:

```
Σ(1 + n_i) + 2(内核线程) ≤ 16
```

文档 `os_thread-process.md:384` 的原话:"TCB 16 个是硬上限……libc 的 `pthread_create` 又限制每进程 4 个,所以实际很难打满。"

## 代价(改代码时会踩到)

因为 TCB 是全局共享的稀缺池:

1. 槽位满时 `kthread_user_spawn()` 返回 0,`thread_create()` 报 `-EAGAIN`(`usermode.c:4045-4047`)
2. `proc_exit()` 只回收 `cur->uk_p_thread` 一个 TCB(`usermode.c:2824`),多线程进程被 `exit()` 或信号终止时兄弟 TCB 会泄漏,槽位留在 `kthreads[]` 里而地址空间已释放——`os_thread-process.md:324` 明确记下的已知缺陷
3. 内核线程与用户线程抢同一个池,内核线程有优先权(`proc_sched()` 每轮先 `kthread_run()` 把内核线程队列跑空)

## 一句话总结

16 不是性能调优的结果,而是"槽号 = 地址"这种设计能成立的最大值;要扩容得动地址布局(内核栈 VA 公式、用户栈 `tid & 0xf` 编码、pmap 映射范围、tid 分配),不是改一个宏那么简单。

## 相关代码位置

| 内容 | 位置 |
|------|------|
| `NKTHREAD = 16`、`KSTACK_VA`、`KSTACK_SIZE` | `kernel/thread/kthread.h:21,48-49` |
| `struct kthread kthreads[NKTHREAD]` | `kernel/thread/kthread.c:30` |
| `kstack_alloc()` 内核栈地址编码 | `kernel/thread/kthread.c:100-113` |
| 槽位线性分配 | `kernel/thread/kthread.c:144,189` |
| 用户栈 VA 的 `tid & 0xf` | `kernel/sys/usermode.c:4040` |
| 调度器线性扫描 TCB | `kernel/sys/usermode.c:2535` |
| 设计说明与已知缺陷 | `os_thread-process.md:322,324,384` |
