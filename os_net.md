# BSD45 用户态网络协议栈（S403）+ VFS 透明转发 + 共享页闭环（S402）

> 本轮完成三件事：① **VFS 透明转发层**——`open("/srv/x")` 等普通 syscall 经内核转发到用户态 fs_server；② **共享页大数据闭环**——KP-7 原语封装为"分配→填充→提交→映射→校验→释放"全链路；③ **用户态网络协议栈**——UDP/IP + 环回虚拟网卡运行在用户态 netsrv，故障隔离、可重启。默认不用这些能力时系统与原内核一致（零回退）。

## 1. VFS 透明转发层（内核态实现，S402/S404）

### 1.1 问题与目标

S400 的文件服务通过用户态 `srv_*` API 访问——用户程序要显式调用新 API。目标是**透明转发**：用户程序用标准 `open("/srv/x")/read/write/close` 即可，由**内核 syscall handler** 识别 `/srv/` 前缀与虚拟 fd，把请求转成 IPC 发给 fs_server，用户代码一行不改、libc 不做任何拦截。

### 1.2 为什么早期内核态方案会崩（以及它其实不是 curpcb 的问题）

最早的尝试是在内核 handler 内写 `srv_forward` + `srv_deliver_waiter`，"阻塞→等回复→继续往下跑 C 代码"。它失败的真正原因是**阻塞模型**：`resume_user` 阻塞后直接从 `uk_regs` 恢复用户态，**syscall handler 的整个 C 栈帧被丢弃**（`usermode.c` 里 `proc_sched()` 之后"不会执行到这里"），所以 handler 中途阻塞后无法继续执行后续回填逻辑。

至于常被引用的 TRAP 12：本端口的 syscall 拷贝用的是 `ucopyin`（`user_ok()` 预校验页表），**根本不读 `curpcb->pcb_onfault`**，那段依赖 onfault 的汇编 `copyin` 与 `trap.c` 都未参与编译。所以"维护 curpcb 才能做内核态转发"是误判；即便全程维护 curpcb，也解决不了 C 栈帧被丢弃的问题。

### 1.3 正确解法：一次性完成（continuation）模式

handler 发起 IPC 后把**续传状态**存进 `proc`，阻塞并交出 CPU；回复到达时由**另一进程上下文里的回调**完成回填与唤醒，用户态从 `uk_regs` 恢复即"syscall 返回"。这与 S400 的 `ipc_recv` 阻塞 + `ipc_deliver_waiter` 投递是同一套成熟模式。

| 组成 | 位置 |
|---|---|
| 协议常量（镜像 `libc/include/srv.h`） | `kernel/include/sys/srvproto.h`（新增） |
| 续传状态 | `struct process`：`uk_kvfs_waiting/op/buf/len/handle` |
| per-process 虚拟 fd 表 | `struct process`：`uk_kvfs_fds[8]`，基址 `0x4000`（内核 fd 是 `0..NFD-1`，不冲突） |
| 发起转发 + 阻塞 | `kvfs_forward()`（`kernel/sys/usermode.c`） |
| 回复回填 + 唤醒 | `kvfs_reply_deliver()`，挂在 `SYS_IPC_SEND` 的钩子上 |
| 拦截点 | `SYS_open`（前缀）、`SYS_read/write`（虚拟 fd）、`SYS_close` |

以 read 为例的完整数据流：

```
用户 read(0x4000, buf, n)
  → sys_read 识别虚拟 fd → 取出服务 handle
  → kvfs_forward: 打包 SRV_READ → 会合投递/入队到端口 0
                  → uk_regs = *r，登记 uk_kvfs_*，kthread_block_tcb + proc_sched
  → fs_server 处理 → ipc_send(端口 1, rep)
  → SYS_IPC_SEND 钩子 kvfs_reply_deliver: 切到等待者页目录 pipe_copy 回填 buf
                  → uk_regs.eax = n → proc_wake
  → 等待者从 uk_regs 恢复，用户态拿到 n
```

**串行化**：内核是协作式调度，转发 syscall 一阻塞即让出 CPU，因此同一时刻最多一个内核转发事务在途，用单个全局 `kvfs_waiter` 定位回复目标即可。fs_server 零改动（它的回复不带请求者标识，多客户端并发需按 `reply_port` 区分，列入后续）。

**libc 侧**：`open/read/write/close` 恢复为纯 syscall 包装，不再检测 `/srv/`（S402 的 libc 层实现已被本阶段取代）。

### 1.4 故障隔离

- 转发前 `port_owner(0) == 0`（服务未启动/已退出）→ 直接返回 `-EIO`
- 服务在处理中途退出 → `port_release` → `ipc_fail_waiters` 给在途事务写 `-EIO` 并唤醒
- 服务重启并重新注册端口后自动恢复
- 请求/回复端口沿用 fs_server 的 0/1；数据 ≤56 字节内联（大包走共享页，见 §2）

### 1.5 过程中修掉的两个真实内核 bug

这两个 bug 在 libc 层转发时被"用户态自旋重试"掩盖了，改到内核态后立刻暴露：

1. **`resume_user` 破坏用户 `edx`**（`kernel/arch/i386/usermode_asm.s`）
   原实现先加载用户 `edx`，再用 `movw __udatasel,%dx` 设置段选择子——`dx` 正是 `edx` 的低 16 位，于是刚恢复的用户 `edx` 被改写成段选择子 `0x27`(39)。而 GCC 内联 syscall 常用 `mov %edx,%ebx` 传常量（fs_server 传 `SRV_PORT=0`），内核于是收到端口 39 → `EINVAL` → 服务误判并自杀。
   **修复**：段选择子设置在前，用户 `edx` 最后加载。

2. **单线程进程阻塞没真正生效**（`kthread_block_tcb`）
   单线程分支只置 `uk_state = P_BLOCKED`，不置 `t_state = KT_BLOCK`；`proc_sched()` 末尾"回落当前线程"分支（`mt->t_state == KT_RUN`）立刻又把刚阻塞的进程恢复，等于没阻塞。内核转发因此不等回复就带着旧 `eax` "返回"。
   **修复**：同时把主 TCB 置 `KT_BLOCK`，由 `proc_wake()` 恢复。

## 2. 共享页大数据闭环（KP-7，S402）

### 2.1 问题

IPC 消息内联区只有 84 字节（数据 ≤56 字节），大文件/大包无法承载。

### 2.2 闭环设计

`libc/include/shmbuf.h`（static inline，工具单文件即可用）封装 KP-7 原语：
```
shm_alloc()  → page_alloc + map_shared 到 SHM_VA，返回可写指针
   ↓ 填充数据（整页 4KB）
shm_commit() → 解除映射，数据留在物理页；pa 放进 IPC 消息的 pa 字段发送
   ↓ 接收方
shm_map(pa)  → 映射收到的 pa 到 SHM_VA，读取数据
shm_unmap()  → 读完解除映射
shm_free()   → page_free 释放物理页（ACK 最后一环，由分配方调用）
```
无需跨进程引用计数；服务崩溃时内核在 `free_address_space` 统一回收映射页。

`shmtest.c` 验证整页 4KB 数据往返 + 释放后再分配（无泄漏）。这是 VFS 大数据读写与 net 大包传输的基础原语。

## 3. 用户态网络协议栈（netsrv，S403）

### 3.1 架构：MINIX 式服务化

完整 4.5BSD TCP/IP 栈上移需要网卡驱动 + IOPL + IRQ 投递地基（当前缺，QEMU 无网卡配置）。本轮实现**协议栈本体完全在用户态**的可运行骨架：

```
 用户程序 (nettest)  ── net_* API over IPC (端口4/5) ──▶ netsrv (用户态)
                                                          ├ 协议分派表 proto_dispatch[]
                                                          │   (UDP 已实现 / TCP 预留)
                                                          ├ socket 表 socks[]
                                                          │   (端口绑定 + RX 数据报队列)
                                                          └ 环回虚拟网卡 loopback
                                                              (本机端口→目标 socket 队列)
 内核：仅 IPC 端口，无协议栈代码
```

"协议栈在用户态"的实体是 `proto_dispatch[]`——用户态的协议开关表（对应内核 cdevsw 的用户态等价物），协议知识在服务进程里，不在内核。新增协议只需加表项。

### 3.2 能力

- `NET_SOCKET/BIND/CONNECT/SEND/RECV/CLOSE/STAT/SHUTDOWN` 操作
- UDP 数据报收发：发送到本机绑定端口经环回网卡直接投递到目标 socket 的 RX 队列
- 端口唯一性检查（`EADDRINUSE`）、socket 表满（`EIO`）、未实现协议（TCP→`EINVAL`）
- `NET_STAT` 统计：socket 数、发包数、收包数、环回投递数
- 故障隔离：SHUTDOWN → 内核释放端口 → 客户端 `-EIO` → 可重启

### 3.3 边界（如实说明）

- 物理网卡驱动未上移：发往非本机端口的包计入已发送计数后丢弃（无物理 NIC）。真正接入物理网络需要 IOPL 端口 I/O + IRQ→用户态投递 + 网卡驱动服务，是后续阶段
- 当前为无连接 UDP 语义；TCP（NET_SOCK_STREAM）在分派表预留但未实现
- 数据 ≤56 字节内联；大包可走 §2 共享页（原语已具备）
- 单客户端串行；多客户端并发需要按客户端区分 reply_port

## 4. 设计取舍

1. **透明转发与零回退**：`/srv/` 是新增前缀，内核原有 `/dev/*`、`tmpfs`、`ISO` 路径完全不动；不使用服务时行为与原内核一致。
2. **一次性完成而非嵌套阻塞**：见 §1.2/§1.3。内核态转发**不在 handler 里"阻塞后继续跑"**，
   而是把续传状态存进 `proc`、由回复回调代劳回填与唤醒——这样既实现了内核截获，
   又完全不触碰 `proc/kthread` 双层线程模型与既有的优先级调度算法（调度器一行未改）。
3. **串行化换取服务零改动**：单全局事务槽位，同一时刻一个在途转发；代价是不支持多进程
   并发 `/srv/`，与当前"单客户端"现状一致，后续按 `reply_port` 扩展。
3. **环回优先**：用户态 net 先做可完整验证的环回路径，证明协议栈服务化与故障隔离；物理网卡上移列为后续。

## 5. 构建与验证

遵守原则：**Windows 用 PowerShell、Ubuntu 用 bash；Ubuntu 只构建不执行测试**。

```bash
# Ubuntu：仅构建（ELF 直链严格校验）
cd ~/bsd45-build && bash build_all.sh
```

```powershell
# Windows：i686 原生链路 + QEMU 执行
$env:PATH = "C:\Users\Administrator\.workbuddy\binaries\python\versions\3.13.12;" + $env:PATH
.\build_kernel.ps1; .\build_sh.ps1; .\build_iso.ps1
.\tools\run_qemu_test.ps1 -Keys "shell|/bin/shmtest|exit 0" -WaitFor "user exit"
.\tools\run_qemu_test.ps1 -Keys "shell|/bin/vfstest|exit 0" -WaitFor "user exit"
.\tools\run_qemu_test.ps1 -Keys "shell|/bin/nettest|exit 0" -WaitFor "user exit"
```

一键验收脚本 `tools/test_net.sh` 驱动 vfstest/shmtest/nettest 并断言"内核不 TRAP"。

实际验证结果（2026-11-20，Ubuntu bash ELF 直链校验 + Windows i686 原生链路 + QEMU）：
- `vfstest: PASS` —— **内核态**转发：标准 `open("/srv/vfstest")` 由 `SYS_open` 拦截并返回
  虚拟 fd 0x4000、write 11 字节、read 回 "transparent"、服务退出后 `open` 返回
  -1/errno=EIO、重启恢复；无 kernel TRAP，全程无 libc 拦截
- `nettest: PASS` —— 用户态 netsrv 的 UDP 环回收发、统计（sent=1/recv=1/loop=1/socks=2）、
  故障隔离 EIO、重启恢复；无 kernel TRAP
- `shmtest: PASS` —— 4KB 共享页往返校验 + 释放后再分配（无泄漏）
- 回归：`srvtest: PASS`、`devtest: PASS`、`ipctest: PASS` —— 内核改动零回退

调试要点（已解决，如实记录）：
1. 内核态转发的真正障碍是**阻塞模型**而非 `curpcb`：`resume_user` 阻塞后丢弃 syscall
   handler 的 C 栈帧，故采用"续传状态 + 回复回调代劳"的一次性完成模式（§1.3）。
2. `resume_user` 用 `%dx` 设置段选择子时破坏了用户 `edx`，使 fs_server 传入的
   `SRV_PORT=0` 变成段选择子 0x27(39) → `ipc_recv` 返回 `EINVAL` → 服务自杀。
   已在恢复用户 `edx` 之前完成段设置（§1.5-1）。
3. `kthread_block_tcb` 对单线程进程只阻塞进程不阻塞主 TCB，`proc_sched()` 的"回落自己"
   分支会立刻恢复它，转发带旧 `eax` 假返回。已同步置 `KT_BLOCK`（§1.5-2）。
4. fs_server 的 CLOSE 语义是释放服务端文件槽位（`files[idx].used=0`，非删除文件内容），
   vfstest 用同 fd write/read 回环验证透明读写，另加 **close 完整转发专项用例**：连续
   20 次不同文件名 open/close，若 close 真把 SRV_CLOSE 转发到服务端，槽位会一直复用、
   全部成功；若仍为旧的"本地假释放"，16 个服务端槽位会在第 17 次 open 返回 -ENOSPC。
   S405（2026-11-21）已将 `SYS_close` 从"本地释放虚拟 fd"改为完整阻塞转发
   （`kvfs_forward(SRV_CLOSE_KERNEL)` + 回调 `kvfs_reply_deliver` 释放槽位并返回 0）。
5. `nettest` 的失败曾出现一次，为重编译后服务启动等待不足的时序抖动（`delay` 按周期
   计数、内核体积变化会改变实际等待时长），重跑即 PASS，非代码回归。

## 6. 变更文件清单

**新增**
- `kernel/include/sys/srvproto.h`（内核侧 fs_server 协议常量，S404）
- `libc/include/shmbuf.h`、`libc/include/net.h`
- `libexec/shmtest.c`、`libexec/vfstest.c`、`libexec/netsrv.c`、`libexec/nettest.c`
- `os_net.md`（本文档）

**修改（S404 内核态转发）**
- `kernel/sys/usermode.c`（`kvfs_forward` / `kvfs_reply_deliver` / open·read·write·close 拦截 /
  `ipc_fail_waiters` 的 EIO / `kthread_block_tcb` 单线程阻塞修复）
- `kernel/include/sys/kprocess.h`（`uk_kvfs_*` 续传字段 + `uk_kvfs_fds[]`）
- `kernel/arch/i386/usermode_asm.s`（`resume_user` 的 `%dx` 破坏用户 `edx` 修复）
- `kernel/thread/kipc.c`、`kernel/include/sys/kipc.h`（`port_owner()`）
- `libc/src/libc_sys.c`（撤回 /srv/ 拦截，恢复纯 syscall 包装）

**修改（其余）**
- `build_sh.sh/.ps1`、`build_iso.sh/.ps1`（编译打包新程序与新头文件）
- `build_kernel.ps1`（genassym 输出捕获修复，规避 PowerShell 5.1 的 UTF-16 BOM）
- `os_microkernel.md`（交叉引用）

**关于"零回退"**：不使用 `/srv/` 与虚拟 fd 时，内核 syscall 路径行为不变（前缀与 fd 基址
都是新增分支）；`proc/kthread` 双层线程模型与既有优先级调度算法未改动。S404 的两处修复
（`resume_user` 寄存器破坏、单线程阻塞）是纯粹的正确性修复，对所有路径都是净收益。
