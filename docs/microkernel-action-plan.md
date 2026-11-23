# BSD45 微内核化执行方案（S100.3 落地版）

> 本文不是又一份设计文档。`docs/kernel-boundary.md` / `docs/microkernel-design.md`
> 已经把「留什么、搬什么、IPC 长什么样」讲清楚了。本文要做的是：**核对设计与代码
> 现状的偏差，给出可以马上动手的第一步，并标出每个阶段的可验证判定点。**
>
> 基线：工作区 `main`，`30a5620`（JLFS 镜像）。

---

## 0. 先纠正三个过时前提（重要）

微内核设计文档写于协作式基线（`c89e4fd`），但当前代码已经前进了两个大版本。
照旧文档动手会做无用功。核对结果：

| 设计文档的假设 | 当前代码真实状态 | 对方案的影响 |
|---|---|---|
| 「协作式调度、无时钟中断」 | **已有 100Hz PIT + `sched_tick()` 抢占**（`arch/i386/timer.c` 已编进构建，IDT 挂 IRQ0，`sched_tick` 在 `usermode.c:2594`） | 看门狗超时、服务抢占、卡死检测都有了硬件基础；文档里「无时钟无法检测卡死」的限制**已不成立** |
| KP-4 阻塞原语「完全缺失」 | 线程层已有 `kthread_block()` / `kthread_wake()`（`kthread.c:275/290`，含 run queue 出/入队 + `sched_on_block/wake` + 栈切换） | 第一步不是「从零写 KP-4」，而是**给现有机制加一个 wait channel 包装** |
| 调度单位是 `struct uproc` / `struct process` | 已是 **KLT**：`struct process`（资源容器）+ `struct kthread`（唯一调度单位），20 级优先级 + 继承 + estcpu | IPC 阻塞/唤醒应挂在 `kthread` 上，不是 `proc`；端口等待队列存 TCB |

结论：**真正缺的只有 KP-6（IPC send/recv）和 KP-7（共享页）**。KP-4 是 80% 现成的。
这是好消息——S100.3 的门槛比文档写的低。

---

## 1. 目标布局（和已有设计一致，不重复论证）

```
用户态 ring3
  /bin/sh · init      fs_server (tmpfs+ISO)      tty_server (行规程)
         └──────────────────┬───────────────────────┘
                    IPC: send/recv + 共享页
内核 ring0
  调度/KLT ─ ipc.c(新) ─ pmap/trap/IDT ─ block_on/wakeup(包装)
  syscall/fd/管道(不动) ─ bdev 内核代理(ATAPI 端口 I/O)
```

留内核：调度、页表、trap、时钟、端口 I/O、IPC 内核侧、syscall/fd/管道。
上移用户态：tmpfs、ISO9660、tty 行规程。

---

## 2. 落地顺序（每步独立可回退、独立可验收）

### 步骤 1 — KP-4 补齐：wait channel 包装（低风险，纯增量）

在 `kthread.c` 加两个函数，复用现成阻塞机制：

```c
struct waitq { struct kthread *head; };   /* 等待某通道的 TCB 链表 */

void block_on(struct waitq *wq) {
    kthread_enqueue_waiter(wq, curk);      /* 挂入等待队列 */
    kthread_block();                       /* 已有：出队 + 切栈 */
}
void wakeup(struct waitq *wq) {
    for (t = wq->head; t; t = next)
        kthread_wake(t);                   /* 已有：入队 + 提优先级 */
}
```

- 不改任何现有调用点；先把 4 处手写 `P_BLOCKED` 状态机留着。
- **判定点**：`kthread_selftest()` 加一个 block/wake 用例；17 套全绿；`check-boundary.sh` 硬规则仍 0。
- 交付：独立 commit，`-Variant mono` 行为零变化。

### 步骤 2 — KP-6 IPC：内核消息层（核心，一次只做这个）

新增 `kernel/sys/ipc.c`（约 200-300 行），按文档 §4 的契约：

- 端口表固定 16 项：`struct port { pid owner; struct waitq recvq; struct msg q[...]; }`
- `struct msg`：`from / type / len / pa / inline[64]`（≤64 字节内联，大块走 pa）
- **会合语义**：`send` 时接收方在等 → 直接交付 + `wakeup`；否则发送方 `block_on`；
  `recv` 时队列空 → `block_on`。回复不用单独原语，请求里带 `reply_port`。
- 新增 syscall：`port_register / port_wait / msg_send / msg_recv`（编号接续现有最大号）。
- 服务退出时端口关闭，内核 `wakeup` 所有等待者并返回 `-EIO`（故障隔离的基石）。

**判定点**：写一个用户态 `ipc_ping` 测试——两个进程互发消息 + 阻塞唤醒 +
服务退出后等待方收到 EIO。17 套全绿。这一步**先不接 VFS**，纯验证 IPC 本身。

### 步骤 3 — KP-7 共享页（配合步骤 2 的大数据路径）

- `map_shared(pa, len, target_pid)`：把服务分配的物理页映射进目标进程页表。
- 生命周期「谁分配谁释放 + ACK」：内核 copyout 完立即回 ACK，服务收到 ACK 才 `vfs_page_free`。
- 复用现有 `vfs_page_alloc/free` + `pmap_map`（KP-2/3 已有），不重新造页管理。

**判定点**：用户态 `shmem_test`——服务填一页数据发给内核，内核 copyout 到调用者
缓冲并校验内容，ACK 后页归零回池。进程退出不泄漏（复用 `free_address_space`）。

### 步骤 4 — 第一个服务：tmpfs → fs_server（S100.3 的正题）

`tmpfs.c` 已经是独立文件（S100.2a），这是它被调用的原因。做法：

- 内核侧新增 `ipc_vfs.c`：tmpfs 的 11 个 vnodeops 改成打包 IPC 消息（**不缓存文件内容**）。
- 用户态 `servers/fs_server.c`：把 `tmpfs.c` 的节点表/数据页逻辑原样搬过去，跑 recv 循环。
- 启动：`init` 挂占位 root vnode → fork/exec `fs_server` → 等端口注册（有 100Hz 时钟了，
  可以做真超时，10s 没注册回退内核内 ramfs）→ exec `/bin/sh`。
- **`-Variant mono|micro` 开关**：`mono` 走现有 tmpfs.c，`micro` 走 IPC 转发。文件级差异。

**判定点**：`cd /tmp; echo x > a; cat a` 全走 IPC 且 17 套全绿；
故障注入——`kill fs_server` 后 `ls /tmp` 内核不 TRAP、返回错误、init 能重启服务。
这是整个微内核化的**决策点**：如果 IPC 往返已经让交互明显变卡，就停在这里，
写结论留在「FS 用户态 + tty 内核」混合态，不必硬做完。

### 步骤 5 — ISO9660 + bdev（S100.4）

先落 `bdev_read(lba, nsec, buf)` 包装 `atapi_read`（消掉 6 处层次债），
再把 `isofs.c` 搬进 fs_server。判定：从 ISO 跑 `/bin/sh` 全绿。

### 步骤 6 — tty_server + 交付分叉（S100.5/6）

行规程（`console_read` 等）上移到 tty_server，端口字节 I/O 留内核。
此时才把两套发布快照放进 `microkernel/bsd45/` 和 `monolithickernel/bsd45/`。

---

## 3. 三条硬约束（来自 S100，不能破）

1. **17 套验收全绿**（S17 后从 15 套涨到 17 套：`test_cache` + `test_mmap`），每步都要过。
2. **`-Variant mono` 永远可回退**；主线只合入不影响 mono 的改动。
3. **先让 IPC 自身跑通（步骤 2/3）再接 VFS**，不要一上来就改 vnodeops——那样 IPC bug
   和 VFS bug 混在一起，无法定位。

---

## 4. 需要额外做的（文档没要求，但专家会做）

- **故障注入要自动化**：不要只手工 kill。加 `tools/test_fault.sh`——在 QEMU 里 kill
  fs_server，断言内核不 TRAP、返回 EIO、服务被重启。这是「微内核化到底有没有意义」
  的唯一硬证据。
- **IPC 开销要量化**：内核加 `ipc_sends/ipc_recvs` 计数器（文档 §9 已列），用同一组
  `all_tests.sh` 对比 mono/micro 的上下文切换数和耗时。**没有数字就没法决定停在哪一侧。**
- **更新过时文档**：动手前先把 `microkernel-design.md` 的基线从「协作式、无时钟」
  改成当前 S28+/KLT/100Hz 现实，否则后续每个决策都会被旧前提带偏。
- **端口 I/O 不上移**：没有 IOPL 权限模型，ATAPI 的 `inb/outb` 搬用户态代价大于收益，
  按文档留在内核 bdev 代理——这条判断是对的，别推翻。

---

## 5. 立刻可以做的第一件事

步骤 1（wait channel 包装）风险最低、又是后面所有步骤的地基。建议从这里开工：
在 `kthread.c` 加 `struct waitq` + `block_on/wakeup`，给 `kthread_selftest()` 加一个
block/wake 用例，跑 17 套确认全绿，提交。然后再进步骤 2。
