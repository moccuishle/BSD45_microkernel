# 动态进程/线程分配（Level B 设计文档）

> 状态：设计中（2026-11-21）。目标是把静态进程表 `procs[8]` 与静态线程池 `kthreads[16]` 改成动态分配，使进程数上限由物理内存决定，而不是被硬编码常量锁死。
> 测试环境：**VMware**（用户 2026-11-21 明确要求：仅在 VMware 中测试，不再用 QEMU）。

---

## 1. 不可动摇的架构约定

**本系统是内核态线程和进程模型（KLT，1:1 模型）** —— 这是用户反复强调的硬约束，任何改动都不得破坏：

- 进程 = `struct proc`（资源 + 地址空间的容器），线程 = `struct kthread`（调度单位）。
- 每个用户线程配一个独立内核栈 + 一个 TCB（内核线程 `t_proc==NULL` 与用户线程共用同一个线程池）。
- 主线程上下文在 `proc->uk_regs`，其余线程在 `t->t_regs`；调度器只遍历 `kthreads[]`，从不按 `procs[]` 状态机调度。
- 动态化只能改变"对象如何分配与串联"，**绝不能**退化成用户态线程库或 N:1 模型。

详见仓库 `os_thread-process.md`、`os_sched.md`。

---

## 2. 当前进程数上限的三重约束

进程数 = min(三条约束)，目前被锁在 8：

### 2.1 `procs[NPROC]` 静态数组（上限 8）
- `bsd45/kernel/sys/usermode.c:160` → `#define NPROC 8`
- `:201` → `static struct proc procs[NPROC];`
- `proc_alloc()`（`:2620`）遍历找 `P_FREE` 槽，找不到返回 0 → `fork`/`vfork` 返回 `-EAGAIN`。

### 2.2 TCB 池 16 槽（隐藏的真正瓶颈）
- `bsd45/kernel/thread/kthread.h:21` → `#define NKTHREAD 16`
- `kthread.c:30` → `struct kthread kthreads[NKTHREAD];`
- 每个进程主线程占一个 TCB（`proc_add_thread(p, p->uk_pid)`），另有 softirq/aio 两个内核线程。即使把 `NPROC` 调到 100，也会先在 TCB 耗尽（进程数最多约 14）。
- 且 `tid` 与槽位绑定：`kthread_spawn()` 里 `t->t_tid = i + 1`；调度器 `fair_pick_next()` 用 `me - kthreads` 指针减法 + 取模实现 round-robin。

### 2.3 物理内存 ~61MB（硬天花板；VMware 中可扩容验证）
- `usermode.c:298` → `static unsigned next_pa = 0x200000;`
- `page_alloc()`（`:421` 附近）bump 分配：`next_pa += PAGE_SIZE; if (next_pa >= 0x3f000000) panic("usermode: out of physical memory");`
- 分配区间 `0x200000 ~ 0x3f000000` ≈ 61MB。**这个上限是代码写死的常量**，`-m 64` 的 QEMU 只是恰好匹配它。
- **在 VMware 中给虚拟机分配更多内存时，必须同步修改 `page_alloc()` 的 `0x3f000000` 上界（建议改为按探测到的物理内存动态计算），否则新增的 RAM 用不上，61MB 天花板依旧存在。** 这正是本任务"动态内存分配"名字的由来，也是要在 VMware 中重点验证的点。

### 2.4 次要约束
- pid 为 `int32_t`（`sys/types.h:71`）、单调递增不回绕（`next_pid++`）：理论上限 21 亿，实践不会先撞到，但长期运行需考虑回绕。
- 僵尸不 `wait4` 就占槽：无论上限调到多大，不回收照样耗尽。

---

## 3. 可行的三个级别

| 级别 | 做法 | 效果 | 工作量 |
|---|---|---|---|
| **A. 简单扩容** | `NPROC` 8→32、`NKTHREAD` 16→40（同步重排内核栈 VA、确认 tid 不冲突） | 支持约 30 个进程 | 小，1~2 天 |
| **B. 动态上限（已选定）** | 进程对象与 TCB 池改为动态分配（保留数组指针语义，不破坏 KLT 1:1 与调度器 `me-kthreads` 轮转）；上限 = 物理内存允许；同步把 `page_alloc()` 的 61MB 硬上界改为按实装内存动态计算 | 几百个进程，直到内存耗尽返回 `-EAGAIN`/`-ENOMEM`；VMware 加内存即可支持更多 | 大，但保留 KLT 模型、避免重写调度器 |
| **C. 真正无限** | 需 swap/需求分页 + 进程换出 + 64 位地址空间 | 理论无限（受磁盘限制） | 极大，等于重做内存子系统 |

**Level C 在当前 32 位架构下不可行**（swap 只是桩实现、bump 分配器无回收压缩）。现实目标是 Level B。

---

## 4. Level B 实施设计

### 4.1 核心基础设施（最终方案：静态扩容 + 动态内存上界 + tid 分区）

**为何这样选**：探查发现两个硬约束——
1. 全链表化要重写调度器 `fair_pick_next()` 的 `me-kthreads` 轮转、tid 编码、栈管理，风险过高，违背"保持 KLT 1:1 模型"。
2. 本 32 位内核只有 5 个共享内核 PDE（`0x3f6..0x3fa`）。TCB/进程对象必须放在共享内核 VA 内才能被切换 CR3 后的所有进程访问，而这块空间几乎被内核镜像、SCRATCH、ZCOMP、TMPFS、栈占满——"每对象动态分配独立 VA"会引发严重的页表可见性问题。

因此采用**最稳妥且能达到同等效果**的方案：
- **静态扩容**：`NPROC` 8→**24**、`NKTHREAD` 16→**24**（不是 256，见下方引导器约束）。每槽一个 8KB 内核栈，`KSTACK_VA=0xFEA00000` 位于共享 PDE `0x3fa`。
  - **关键坑：stage2 引导器地址冲突（本次实测踩到）**。stage2（`boot/i386/stage2.s`）在把 PT_LOAD 段搬到 `p_vaddr-0xFE000000` 之前，先把 ELF 头快照到 `0x8F000`、用 `0x9F000` 作栈，自身运行在 `0x90000`。当内核 `.data`+`.rdata` 段物理目标越过 `0x88000~0x8F000` 时，搬运过程会覆盖正在运行的 stage2/快照 → 内核卡在段搬运、`debugcon` 只到 `1234a`（无 `5`）、串口/VGA 全无输出。用 QEMU `-debugcon file:` 捕获 0xE9 检查点精确定位到此。
  - 实测：`NPROC=256`→内核 860KB→不引导；`96`→`.rdata` 在 0x8f000 直接冲突→不引导；`40`→sections end 0x96014→仍冲突；**`24`→sections end `0x8e014` < 0x8f000，`.rdata` 在 0x88000，引导成功**（debugcon `1234a5` 全检查点通过）。`struct proc`=1808B。这是 stage1/stage2 实模式 <1MB 布局下的安全最大值。
  - **若要突破 24**：需改 stage2 把快照/栈/自身搬到内核段之上（需处理 <1MB 与自搬移），属于引导器改造，超出本次范围。
- **动态内存上界（关键）**：`page_alloc()` 的写死上界 `0x3f000000`（~61MB）改为按 BIOS 探测的 `physmem` 动态计算（`usermode.c`）。**这才是"上限由内存决定"的落地点**：默认 64MB guest 下内存在 256 之前先耗尽（fork 返回 -EAGAIN/-ENOMEM），VMware 加大内存即可创建更多进程，直到 256 的栈 VA 硬限。
- **tid 分区**（避免 pid 到 257 后的 tid 冲突）：主线程 tid==pid（1..257）；额外用户线程 `TID_USER_BASE(1000) + pid*TID_STRIDE(16) + n`；内核线程 `TID_KERN_BASE(0x40000000) + slot`。三段隔离，定义在 `kthread.h`。
- **KLT 语义零改动**：调度器、1:1 内核线程模型、栈管理、`t_next`/`t_rqnext`/`t_wqnext` 全部保持原样。所有 `for(i<NPROC/NKTHREAD)` 循环、`&procs[i]`/`&kthreads[i]` 不变。
- **ISO LBA**：内核 .data 因 procs 扩容从 0x66300 涨到 0xd3a80（+~460KB），ISO 内容超出原 kernel LBA 4096；把 `KernelLba` 4096→8192（stage1 从 boot.img manifest 动态读取，无需改汇编）。ps1/sh 构建脚本同步。

**代价与边界**：进程上限是 min(256, 物理内存可容纳数)，不是字面意义的"无限"。真正无限需要 swap + 64 位地址空间（Level C），当前 32 位架构做不到。但在默认内存下内存先耗尽（真·由内存决定），VMware 加大内存后上限随之上升——满足 Level B 的实际诉求。

### 4.2 改动清单：`procs[NPROC]` 全部引用点（均在 `usermode.c`）

**定义/初始化**
- `:160` NPROC、`:201` 静态数组 → 链表头。
- `:5678` `bzero(procs, sizeof(procs))` → `proc_list_head = 0`（pid1 用 `proc_alloc()`）。
- `:5679` `cur = &procs[0]` → `cur = proc_alloc()` 并挂链。

**按条件查找（统一改链表遍历，核心是 `proc_find()`）**
| 行号 | 函数 | 条件 |
|---|---|---|
| 1083 | `aio_soft()` | pid |
| 1252 | `uring_wake()` | pid |
| 2071 | `ipc_deliver_waiter()` | P_BLOCKED + port |
| 2110 | `ipc_fail_waiters()` | P_BLOCKED + port |
| 2672 | `proc_find()` | pid（核心，改此函数后间接调用者自动适配） |
| 2707 | `proc_children()` | ppid |
| 3126 | `proc_exit()` | ppid + wait_pid（循环中会唤醒父，注意遍历安全） |
| 3294 | `sig_send_pgrp()` | pgrp |
| 3450 | `do_wait4()` | ppid + ZOMBIE/STOPPED |
| 4692 | `bsd45_syscall()` case 37 kill | pid/pgrp/特殊 pid（保留 pid==1 排除语义） |

**全表遍历**
- `:2624` `proc_alloc()`（改为分配对象并挂链）
- `:2844` `estcpu_decay_all()`
- `:3533` `proc_list_text()`（`/dev/ps`）

### 4.3 改动清单：`kthreads[NKTHREAD]` 全部引用点

**定义/声明**
- `kthread.h:21` NKTHREAD、`:111` extern；`kthread.c:30` 数组定义；`sync.c:24` 无用 extern（直接删）。

**`kthread.c` 内**
| 行号 | 函数 | 要点 |
|---|---|---|
| 122-132 | `kthread_init()` | 清表 → 初始化链表头；打印动态上限 |
| 144-153 | `kthread_spawn()` | 找空闲槽 → 动态分配；`t->t_tid = i+1` → 全局 tid 分配器（须避开用户 tid 空间） |
| 162-164 | 同上 | 栈 VA 按 i 算 → 按分配到的 VA 算 |
| 189-198 | `kthread_user_spawn()` | 同上；用户主线程 tid = `uk_pid`（保持不变，这是正确解耦） |
| 206-214 | 同上 | 挂入 `p->uk_p_threads`，保持 |
| 222-225 | 同上 | 栈 VA 改造 |
| 236-239 | `kthread_user_find()` | 按 tid 遍历全局链表；保留 `t_proc!=0` 区分用户/内核线程 |
| 245-270 | `kthread_user_free()` | 补：从全局链摘除 + unmap/释放栈页 + 释放 TCB |
| 303-316 | `kthread_exit()` | 同上 |
| 367-375 | `kthread_print()` / `:381` `kthread_count()` | 全表遍历 → 链表 |
| 653-655 | `kthread_find()` | 按 tid 查（优先级继承链调用） |

**`usermode.c` 内**
| 行号 | 函数 | 要点 |
|---|---|---|
| 271-279 | `proc_wake()` | 改遍历 `p->uk_p_threads` 更精确 |
| 2738-2770 | `proc_sched()` | 主调度器遍历所有线程，保留 `t==mt` 判断 |
| 2832-2834 | `estcpu_loadavg()` | 全表统计 |
| **2909-2911** | `fair_pick_next()` | **最高风险**：`me - kthreads` + 取模轮转无法机械替换，必须重写为"从 me 后继沿双向链表走一圈" |
| 2975-2979 | `sched_tick()` | 反饥饿遍历，`&kthreads[j] != me` → `t != me` |
| 3218-3225 | `user_fault()` | 杀进程内非主线程，改遍历 `cur->uk_p_threads` |
| 4054-4068 | `thread_exit_current()` | 找 joiner，改全局链表 |

### 4.4 栈 VA 动态管理
- 当前：`stack_va(i) = KSTACK_VA(0xFEA00000) + i * 8192`，16 槽占 `0xFEA00000~0xFEA20000`。
- `0xFEA20000 ~ 0xFEDFFFFF` 区间目前空闲，可作扩展栈区。
- `kstack_alloc(i)` → `kstack_alloc(t)`：分配 8KB 对齐 VA 块 → 逐页 `vfs_page_alloc` + `pmap_map` → 结果存入 `t->t_stack_va`，`t_stack_top` 据此计算。
- 新增 `kstack_free(t)`：当前**所有释放路径都不归还栈页**，动态化必须补齐，否则泄漏。
- `hal_kernel_stack_set(t->t_stack_top)` 的 5 个调用点（2766/2775/3020/3858/5704）填对新字段即可，无需改动。

### 4.5 回收路径
- `do_wait4()`（3457-3464）回收僵尸 → 摘链 + `free(p)`。
- `proc_exit()`（3160-3186）→ 当前只释放主线程，必须改为遍历 `uk_p_threads` 释放全部 TCB 及栈页。
- fork/vfork 失败回滚（3037-3040、3091-3094）→ 撤销分配、摘链、释放。
- join / detach / tgkill / 异常路径统一经 `kthread_user_free()` 释放。
- 释放前注意摘除 `t_wqnext`/等待队列，避免悬挂指针。

---

## 5. 验证方案（VMware，不用 QEMU）

### 5.1 自动测试构建
VMware 串口无法可靠从主机读命令（命名管道实测阻塞/不通，见 RESUME-TASK.md#59），沿用内核已有 `VM_INSTALL_AUTOTEST` 先例：在 `monitor()` 开头加 `#ifdef VM_PROCTEST_AUTOTEST` → `start_userland("/bin/proctest")` 免键盘自动跑（走 init→sh→execve，与交互式 `shell|/bin/vfstest` 同路径；直接 `start_prog()` + `vfs_namei` 找不到文件，不能用）。
- 内核需额外 `-D VM_FORCE_COM1`：强制 `com_probe()` 信任 COM1、且 `com_putc()` 无条件写端口（否则 VMware 虚拟 UART 的 LSR 不满足探测导致所有串口输出被丢弃）。
- 构建命令：
  ```
  build_sh.ps1 -Target sh                                   # 重编 /bin/proctest
  build_kernel.ps1 -Define 'VM_PROCTEST_AUTOTEST,VM_FORCE_COM1'
  build_iso.ps1                                             # KernelLba=8192
  ```

### 5.2 实测结果（2026-11-21）

**功能验证（权威输出，用 QEMU `-serial file:` 抓取；VMware 见下方说明）**：
```
debugcon(0xE9): 1234a5                    # stage1/stage2 全检查点通过，内核成功引导
Memory: physmem 40a0000                   # 0x40a0000 = 64.6MB, NOT 512MB (see 9.3)
kthread: 24 thread slots at fea00000
kthread: spawn softirq/aio tid 1073741824+ # tid 高位分区，不与 pid 冲突
AUTOTEST: launching /bin/proctest via init script
proctest: phase A sequential fork/wait = 100 (target 100)   # 槽位复用 OK
proctest: phase B simultaneous live children = 20 (target 20)  # 同时存活 20 > 旧上限 8
proctest: reaped 20/20 children                              # wait4 回收 OK
proctest: PASS
```
- 同时存活进程数从旧的 8 提升到 **20**（受 NPROC=24 约束，1 槽给 init shell）；phase B 用 512MB 内存时实测到第 22 个子进程才因 NPROC 槽位耗尽返回 `-EAGAIN`，**不是内存耗尽**——说明 expansion 生效，内存上界改造也生效（physmem 读到 512MB）。
- 引导器地址冲突坑已定位并解决：见 §4.1（NPROC 从 256→96→40→24 逐步收敛到 stage2 可行域）。

**VMware 验证说明**：VMware 虚拟机 `bsd45/vms/BSD45-proctest.vmx`（基于项目模板，memsize=512，串口→文件）能正常引导 BSD45 ISO，并在 proctest 跑完后自动 halt（与 QEMU 中 PASS 后 halt 行为一致，证明测试在 VMware 中同样执行完毕）。

> **S408 更正**：上面这段原来说"VMware 文件型虚拟 UART 未把内核 COM1 字符写入串口文件（0 字节）"。实测并非如此——只要 `.vmx` 里配 `serial0.present/fileType=file` 并且内核用 `-D VM_FORCE_COM1` 构建，VMware 的串口文件能完整接住内核与 stage1/stage2 的全部输出（本文件 §9.7 的输出就是这么抓的）。"0 字节"是早期 `.vmx` 根本没接 `serial0` 造成的。

**回归**：正常构建（无 AUTOTEST 宏）下 `vfstest`/`shmtest`/`nettest` 保持 PASS；KLT 1:1 模型、`/dev/ps`、调度、`thread_*` 语义未改（所有循环/下标逻辑原样保留）。

**边界（如实记录）**：受 32 位 stage1/stage2 引导布局限制，进程表当时只能扩到 24（旧 8 的 3 倍）；真正的"无限进程"需要 Level C（swap + 64 位），当前架构做不到。

> **S408 更新**：§8/§9 之后这个 24 的边界已经不存在（表搬进 VA 窗口、内核搬到 1MB 以上），进程表 256 槽；而"VMware 加大内存被内核读到"在 S408 之前**并不成立**——`physmem 40a0000` 只有 64.6MB，CMOS 在 64MB 饱和。真正的 512MB 探测在 §9.3。

---

## 6. 相关文件
- `bsd45/kernel/sys/usermode.c` —— 进程表、调度器、页分配器、syscall
- `bsd45/kernel/thread/kthread.c` / `kthread.h` —— TCB 池、线程调度、栈
- `bsd45/kernel/include/sys/proc.h` —— `struct proc`
- `bsd45/kernel/thread/sync.c` —— 待删无用 extern
- `os_thread-process.md` / `os_sched.md` —— 架构设计依据
- `tools/test_net.sh` —— Ubuntu bash 验收脚本（后续加断言）

---

## 7. "无限进程"攻坚记录（2026-11-21，结论：卡在引导器布局，已给出证据）

目标：把进程表从 `.data` 静态数组搬到共享内核窗口（`KPTDI 0x3fb..0x3fd`，
`0xFEC00000` 起），容量 1024，`procs`/`kthreads` 声明为基址指针，使
`procs[i]`、`&procs[i]`、`me - kthreads` 语义完全不变。理论上内核文件不再膨胀、
进程数只受物理内存约束。

**做了什么**
- `KPTDI_LAST` 0x3FA → 0x3FD（共享内核窗口扩到 `0xFF7FFFFF`，注意 `0xFF800000`
  是 locore 的 `_APTmap`，不能越）。
- `PROCS_VA=0xFEC00000`（1.8 MB）、`TCB_VA=0xFEE00000`、`KSTACK_VA=0xFF000000`。
- 新增 `kv_window_prepare()/kv_pt_ensure()`：locore 只为 `0x3f8..0x3fa` 建了页表，
  新窗口必须先建 PT（用 `zero_page()` 清零）再 `pmap_map`。
- `proc_table_init()` 在 `kthread_init()` 之后、任何 `pmap_create()` 之前调用，
  保证 `pmap_create()` 拷贝到有效 PDE。

**结果：内核能引导、能建 1024 槽，但 ISO 挂载失败**

用 0xE9 debugcon 做字节精确探针（串口会被后续现象干扰，不可信）：

```
1234a5 1 2 3 00302003 feb00000 m 4 n 00302063
        │ │ │   └PTE=PA 0x302000  └procs   │   └bzero 后 mounted 仍 0
        │ │ └窗口映射完成                 └bzero 前 mounted 已为 0
        │ └页表就绪
        └进入 proc_table_init
```

在挂载分支插探针得到 `A B D`：ATA 探测完成、ATAPI 探测成功（`B`）、
**`iso_mount()` 失败（`D`）**。于是 root 回落到 tmpfs，`/sbin/init` 不存在，
表现为 `no image for /sbin/init (vp=0 type=-1)`。

**关键定位**：`iso_mount()` 在 `glue.c:377`，**早于** `kthread_init()`(413) 和
`proc_table_init()`(415)，所以失败与窗口代码的执行无关；同参数（`-m 512`、
同一 ISO、`run_qemu_test.ps1`）下把改动 `git stash` 后的基线内核完全正常
（`exec /sbin/init: via vnode (10508 bytes)`）。唯一变化是 `procs[]` 离开
`.data` 导致内核镜像布局位移（`.data` −43 KB，`first free page` 0x8f000→0x8b000），
并且串口输出在该区段出现乱码（`4zcomp`、`e6 e9 81 =usermode`）。
**即：这条引导路径对内核镜像布局敏感，这是必须先修掉的真 bug。**

**为什么 28 槽也不行（静态路线的硬上限）**
stage2 在 `0x8F000` 存 ELF 头快照、自身代码在 `0x90000`。实测：

| NPROC | sections end | 结果 |
|---|---|---|
| 24 | 0x8E014 | ✅ 引导、挂载、proctest PASS |
| 28 | 0x91014（`.bss` 结束 0x900C4 压到 stage2 代码） | ❌ 引导失败 |
| 40 | 0x96014（`.rdata` 落到 0x8F000 快照处） | ❌ 引导失败 |

**所以当前端口的安全上限就是 24。**

**要继续往上，必须按顺序做**
1. 修掉"镜像布局影响 ATAPI/ISO 挂载"的隐藏依赖（先用 `git stash` 基线对拍法
   定位具体符号；怀疑点：locore `atdevbase/_Crtat` 计算、`bio` 缓冲、
   `first free page` 引导参数）。
2. 改造 stage2：把自身代码 + ELF 快照 + 栈搬到 1 MB 以上（A20 已开、保护模式），
   解除 `0x8F000/0x90000` 对 `.data/.bss` 的封锁，然后窗口方案才有意义。
3. 再谈 Level C（swap/需求分页/64 位）才可能做到字面"无限"。

在 1、2 未完成前，任何 >24 的容量都会破坏引导或挂载，因此本仓库保持 24 槽的
已验证状态。

---

## 8. S407 攻坚：把"布局敏感"查清楚，进程表搬进 VA 窗口（2026-11-21）

### 8.1 之前所有怪现象的根因（有实证）

§7 把"镜像布局影响 ATAPI/ISO 挂载"当成引导器 bug。**实际是两个独立问题**：

**（1）stage1 的暂存区根本装不下 >576KB 的内核。**
stage1 用 BIOS `INT 13h` 把 ELF 读到 `0x10000` 起。用内存快照（见
`os_vmware_debug.md`）逐 2KB 比对"暂存区 vs ELF 文件"：

```
elf size 0x97ecc (618KB)
first differing 2K block at file 0x90000 -> phys 0xa0000
staged: 00 00 00 ...   elf: 0d 40 00 ...
```

即 **`0xA0000` 以上（显存窗口）BIOS 拒绝写入**，暂存区上限 640KB−64KB=576KB。
618KB 的内核最后 42KB 永远读不进来 → 内核 `.data` 是残缺的 → "ISO 挂载失败、
串口乱码、`.data` 一挪就崩"全是半截内核在跑。VMware 与 QEMU 的 BIOS 行为不同，
所以同一个 ISO 在 QEMU 里"看起来正常"。

**（2）镜像被无谓地撑大了 450KB。**
`build_kernel.ps1` 用 `Get-Command python` 找解释器跑 `tools/pe_bss_fix.py`，
在本机解析到的是 `%LOCALAPPDATA%\Microsoft\WindowsApps\python.exe`
（Store 别名，执行即退出、什么都不做），于是 fixer **静默跳过**：
所有零初始化静态量（`heap` 256KB、`pipes` 33KB、`procs`、`page_ref`…）
留在 `.data` 里带着真实字节。

| | 镜像 | .data | 结果 |
|---|---|---|---|
| fixer 未生效 | 618,188 B | 447 KB | 超 576KB，VMware 里停在 stage1 |
| fixer 生效 | 176,012 B | 1 KB | 正常 |

修法：`tools-find.ps1` 增加 `Get-PythonExe`（跳过 `\WindowsApps\`、
真跑 `python -c "print(1)"` 验证），`build_kernel.ps1` 用它并在找不到时
**红色告警 + 不再静默**。

### 8.2 引导器改动

- `stage2` 仍在 `0x90000`（必须在内核镜像与 `.bss` 之上），
  但 **ELF 头快照从 `0x8F000` 搬到 `0x300000`**（1MB 以上）。
  这样内核表再长也撞不到快照，§7 的 `0x8F000` 天花板彻底消失。
- `stage1/stage2` 增加 COM1 早期检查点（`S2: protected mode, ELF staged` /
  `S2: ELF loaded, jumping to kernel`），VMware 没有 debugcon 时靠它判断
  加载器走到哪一步。

### 8.3 进程表 / TCB 池搬进内核 VA 窗口

`.bss` 受 `0x90000` 的限制仍在，所以**不再把表放进 `.bss`**：

| 表 | VA | 容量 | 物理来源 |
|---|---|---|---|
| `procs[]` | `0xFE400000`（PDE `0x3f9`） | `NPROC=256`，452KB | `kern_table_alloc()` 从 bump 分配器预留连续页并逐页 `pmap_map` |
| `kthreads[]` | `0xFE600000`（PDE `0x3f9`） | `NKTHREAD=256`，2MB 栈窗口 | 同上 |

- `procs` / `kthreads` 由数组改为指针，`procs[i]`、`&procs[i]`、
  `me - kthreads` 语义不变（连续数组 + 指针运算）。
- KLT 1:1 模型、调度器遍历方式、`struct proc`/`struct kthread` 布局**完全未改**。
- 内核 `.bss` 反而变小：`sections end` 从 `0x8E014` 变成 `0x83014`。

### 8.4 当前状态（S407 结束时，已由 §9 关闭）

已完成并验证（QEMU `-m 512`）：

```
S2: protected mode, ELF staged
S2: ELF loaded, jumping to kernel
Kernel: ELF32 i386, KERNBASE fe000000, first free page 8b000
kthread: 256 thread slots at fea00000
usermode: 256 proc slots at fe400000 (452 KB)
BSD/386 monitor - type 'help' for commands
```

**未完成 / 已知回归**：`/sbin/init` 起来后 exec `/bin/sh` 时
`argc` 变成 0（正常应为 3），shell 直接退出，自动测试脚本没跑起来，
因此 256 进程的 PASS 还没拿到。这是一处**新引入的回归**，
下一步在 `bsd45_syscall` 的 execve 路径（用户栈 argv 拷贝 / 页表校验）
上定位；与表放在窗口本身无关（init 自己能跑起来说明表/调度是好的）。

**仍未做**：把内核搬到 1MB 以上（本次试过并回退：`pmap_bootstrap()`
里 `firstaddr = 0x100000` 的硬编码和
`virtual_avail = atdevbase + 0x100000 - 0xa0000 + 10*NBPG` 都假设内核
在 1MB 以下，内核一搬过去 CMAP1/CMAP2/vmmap/msgbuf 就落进 `.text`，
表现为 `pmap_enter` 里 TRAP 12。要搬必须先改这两处，并同步搬
`SCRATCH_PD/PT/PAGE`）。窗口方案已经把"几百个进程"的目标解决了，
搬 1MB 只剩"给内核再留 3MB 余量"的意义，优先级可降。

> **以上两项在 S408 已全部关闭，见 §9。**

---

## 9. S408：关闭 §8.4 的两项未完成，并把"内存决定上限"真正打通（2026-11-22）

### 9.1 `argc=0` 回归：两个独立原因，均已修复并验证

**(1) 进程表被 CD 映像窗口覆盖。** `procs[]` 当时放在 `0xFE400000`，而
`CD_IMAGE_VA` 也是 `0xFE400000`——`cd_map()` 每次 `execve` 都会重映射这段
（最多 4 MB），直接把进程表踩掉，`execve` 随后读到 `argc 0`。表搬到
`0xFE340000`（zcomp 池之后、CD 窗口之前）后消失。

**(2) `resume_user` 在装载用户数据段选择子之后仍用 `%ds` 读内核数据。**
这是 VMware 独有的 TRAP 9：`struct sysregs` 在进程表里（`0xFE34xxxx`），
高于用户数据段限长 `btoc(UPT_MIN_ADDRESS)` → `0xFDC00000`。VMware 在 ring 0
也校验段限长（QEMU 不校验），所以 `movw __udatasel,%ds` 之后的
`movl 28(%esi), %edx` 立刻 #GP。修法：**先把所有字段读进寄存器，最后再装
段选择子**，并用 `%di` 当选择子的临时寄存器（早先版本用 `%dx`，会把用户的
`edx` 低 16 位改成选择子 `0x27`，破坏 `SRV_PORT=0` 这类传参）。
见 `kernel/arch/i386/usermode_asm.s`。

VMware 实测（512 MB guest，`vms/BSD45-proctest.vmx`，串口→文件）：

```
execve /bin/sh: entry 8048000 esp bfbfffc0 pd 3c3000 argc 3   <- 正常
execve /bin/proctest: entry 8048000 esp bfbfffcc pd 3fe000 argc 1
proctest: phase A sequential fork/wait = 200 (target 200)
proctest: phase B simultaneous live children = 200 (target 200)
proctest: reaped 200/200 children
proctest: PASS
```

### 9.2 内核搬到 1MB 以上（`0xFE101000`）

`boot/i386/kernel.ld` 的链接基址 `0xFE001000 → 0xFE101000`（物理
`0x101000`），stage2 的 ELF 头快照 `0x300000 → 0x600000`。§8.4 记录的两个
阻碍（`pmap_bootstrap()` 的 `firstaddr` 与 `virtual_avail` 公式）在 S407
已经改成"只向上收"，因此这次只是链接地址 + 快照地址的改动：

- `firstaddr` 现在由 locore 的 `lea 7*NBPG(%esi)`（`_end` 页对齐 + 7 页）
  给出，`page_alloc_set_floor()` 只抬不降，`main()` 里还有"镜像不得压到
  0xFE200000 scratch 窗口"的 panic 护栏。
- 只有 ELF **文件**仍要落在 0x10000..0x90000 的 576 KB 暂存区；`.data/.bss`
  由 stage2 搬到 1 MB 以上，不再受 640 KB 限制。这正是下面 9.3/9.4 的前提。

### 9.3 内存探测：CMOS 在 64MB 饱和，改由 stage1 走 BIOS E801

§5.2 里"physmem 40a0000（512MB），动态内存上界生效"是个**误读**：
`0x40a0000 = 64.6 MB`，内核那时仍然只认 64MB，`page_alloc()` 的动态上界
`physmem<<12` 因此永远停在 63MB 附近。根因是 4.4BSD 的经典注释
（"when physical memory is > 16 megabytes" 一直没实现）：CMOS `0x17/0x18`
（以及 `0x30/0x31`）在 64MB−1KB 处饱和。VMware 的 `0x34/0x35` 返回
`0x01D0`，根本不是内存大小，不能用来推断。

修法：stage1 还在实模式时调 `INT 15h AX=E801`（AX = 1MB~16MB 的 KB 数，
BX = 16MB 以上的 64KB 块数），把答案写在 `0x9000`（魔数 `MEM1` + 一个字，
低半 AX、高半 BX），内核在 `isa/machdep.c` 里从 `KERNBASE+0x9000` 读
（此时 locore 已把 `0.._end` 恒等映射，且还没进 `pmap_bootstrap`）。
`0x9000` 是两条引导路径都空闲的空档：ISO 路径只用 `0x7E00` 起放追加的 stage2
镜像，硬盘路径把 stage2 装到 `0x8000` 共 4 KB。`stage1_hdd.s` 也加了同一段
E801，否则装到磁盘上的系统会退回 64MB。
E801 不可用时回落到原来的 CMOS 路径，行为与旧版一致。

```
bios 640K+64512K e801 15360K. maxmem 1fedf000, physmem 1fedf000
Memory: maxmem 1fee0000, physmem 1fedf000, avail 100000-1fede000   # 511 MB
```

### 9.4 COW 引用计数表按内存定容（`PAGE_REF_VA = 0xFE900000`）

> **S410 已迁移**：这个地址与页缓存的 `PCACHE_VA` 撞车，而且单字节计数在
> 255 饱和——1000 个进程共享一页时 `page_put()` 会提前释放。现表在
> `0xFEC00000`、16 位计数、覆盖 2GB，见 §10.2。

旧表是 `.bss` 里的 `page_ref[1<<14]`，只覆盖前 64MB：页号超界时
`page_index()` 返回 0、`page_ref_inc()` 变空操作，`page_put()` 于是会把
**另一个进程还在映射的页**放回空闲链表。512MB guest 一旦分配越过 64MB 就
是静默内存踩踏。现在 `page_ref_init()`（`main()` 在 `page_alloc_set_floor()`
之后立刻调用）按 `physmem` 定容，`kern_table_alloc()` 映射到 `0xFE900000`
的 1MB 窗口（LAPIC 0xFE800000、BUF 0xFE800000+256KB 之后，KSTACK
0xFEA00000 之前）：每页 1 字节，512MB guest 用 128KB，4GB guest 用满 1MB（S410 起改为每页
2 字节、1MB 窗口覆盖 2GB）。
建表期间 `page_ref == 0`，那批页不计引用（都是永不共享的内核表）。

### 9.5 内核栈泄漏（每次 spawn 8KB）

`kstack_alloc()` 过去每次 `kthread_spawn()`/`kthread_user_spawn()` 都
`vfs_page_alloc()` 两个页、映射到 `KSTACK_VA + i*8KB`，而**没有任何释放
路径**（`§4.4` 记的那条 TODO）。一个 fork/exit 周期泄漏 8KB，200 次顺序
fork 就是 1.6MB，只有空闲页计数能看出来。

现在每个槽位的 8KB 窗口只映射一次（`kstack_mapped[NKTHREAD]` 位图），
复用时 `bzero` 掉上一个线程的栈帧即可。内核栈内存因此固定上界
`256×8KB = 2MB`，抖动期零分配。**故意不做**延迟回收：用户线程是在自己的
内核栈上释放自己 TCB 的（`kthread_exit()` / `thread_exit_current()`），
在那里归页等于释放正在使用的内存；延迟回收最多只能拿回 2MB，不值得冒
use-after-free 的风险。

`proc_sched()`/`proc_exit()` 空闲时新增一行账目，专门用来盯这个：

```
[user exit 0]
mem: 932 pages in use, 23368 allocated, next free a0f000
```

修复前同一条路径是 `1300 pages in use, 3337 allocated`，修复后
`904/2940`，差值约 400 页 = 200 个 phase B 子进程的内核栈——正好对上。

### 9.6 新增验证入口与 proctest 的 phase C

- `-D VM_SELFTEST_AUTOTEST`（`kernel/sys/mon.c`）：免键盘跑
  `/bin/vfstest; /bin/shmtest; /bin/mmaptest`，输出进串口文件。VMware 的
  串口在这里只能输出，想在 guest 里跑回归只能由内核代跑。
- `proctest` 新增 **phase C**：200 个子进程各自 `mmap` 384KB 匿名内存并
  逐页写入，同一时刻占住 **75MB**——越过旧 CMOS 的 64MB 与旧 bump 上界的
  63MB。内核若仍以为只有 64MB，会直接在 `page_alloc()` 里 panic。

### 9.7 S408 实测（VMware，512MB，串口→文件）

```
bios 640K+64512K e801 15360K. maxmem 1fedf000, physmem 1fedf000
usermode: COW refcount 130783 pages (128 KB) at fe900000
kthread: 256 thread slots at fea00000
usermode: 256 proc slots at fe340000 (452 KB)
proctest: phase A sequential fork/wait = 200 (target 200)
proctest: phase B simultaneous live children = 200 (target 200)
proctest: reaped 200/200 children
proctest: phase C 200 children x 384 KB = 75 MB held at once
proctest: PASS
mem: 932 pages in use, 23368 allocated, next free a0f000
```

回归（`VM_SELFTEST_AUTOTEST`）：

| 工具 | 结果 | 说明 |
|---|---|---|
| shmtest | PASS | |
| mmaptest | file/anon/shared 全 ok | |
| vfstest | **FAIL**：`close forward (server slot leak)` | **既有缺陷**，S405 的 close 转发；用 HEAD 内核（未含本次改动）复跑同样 FAIL |
| schedtest | `/bin/schedtest: not found` | **既有缺陷**，ISO 名字查找（文件在 ISO 里存在，`SCHEDTEST.ELF`）；HEAD 内核复跑同样 not found |

`vfstest` 与 `schedtest` 这两条都与本节的改动无关，用 `git stash` 的 HEAD
内核在同一台 VMware guest 里复现过同样结果，因此作为**既有缺陷**单独记录，
不在本次范围内。

### 9.8 相关文件

- `boot/i386/kernel.ld`、`stage1.s`（E801）、`stage2.s`（快照 0x600000）
- `kernel/arch/i386/isa/machdep.c`（E801 读取 + CMOS 回落）
- `kernel/arch/i386/usermode_asm.s`（段限长/edx 修复）
- `kernel/sys/usermode.c`（`page_ref_init()`、`mem_report()`、`PAGE_REF_VA`）
- `kernel/thread/kthread.c`（`kstack_mapped[]` 栈复用）
- `kernel/sys/mon.c`（`VM_SELFTEST_AUTOTEST`）
- `libexec/proctest.c`（phase C）

## 10. S410：进程表 1024 槽，1000 个进程同时存活（2026-11-22）

§8/§9 之后"无限进程"的实际天花板是**槽位表**：`NPROC = NKTHREAD = 256`。
512MB guest 下 proctest 峰值只用了约 90MB，先撞到的是 `fork` 返回 `-EAGAIN`。
本节把它抬到 1024 槽并实测 1000 个进程同时存活。过程中挖出四个真缺陷，其中
两个会直接毁掉机器。

### 10.1 容量与 VA 布局

`1024 x sizeof(struct proc) = 1024 x 1808 = 1.77MB`，已塞不进
`0xFE340000..0xFE3FFFFF` 那 768KB 空档，所以整组内核窗口上移，放到 locore
12MB 恒等映射窗口之上、没有任何既有窗口占用的地方：

| 地址 | 大小 | 用途 |
|---|---|---|
| `0xFEC00000` | 1MB | COW 引用计数表（16 位，覆盖 2GB） |
| `0xFED00000` | 1.77MB | 进程表 1024 槽 |
| `0xFEF00000` | 1MB | TCB 池 1024 槽 |
| `0xFF000000` | 8MB | 内核栈 1024 x 8KB（`0xFF000000..0xFF800000`） |

最高一页起始 `0xFF7FF000` 正好等于 `VM_MAX_KERNEL_ADDRESS`，不需要改动
vmparam。原来的 `0xFE340000`（进程表）、`0xFE3C0000`（TCB）、`0xFEA00000`
（2MB 栈区）全部空出。

用户页目录只拷贝内核 PDE 一次（`user_pd_create`），因此拷贝范围从
`0x3f6..0x3fa` 扩到 `0x3f6..0x3fd`——上面四组窗口跨 `0x3fb/0x3fc/0x3fd`。

### 10.2 COW 引用计数：从字节改 16 位，并换掉撞车的窗口

两个独立缺陷：

- **单字节在 255 饱和**。`page_ref_inc()` 原写 `if (n < 255) n++`。1000 个子
  进程共享父进程的页时计数停在 255，等其中 745 个退出后 `page_put()` 看到 1，
  就把剩下 254 个仍在映射的页释放了——静默内存踩踏，且只在进程数超过 255 时
  出现。
- **与页缓存同址**。`PAGE_REF_VA` 是 `0xFE900000`，而 `kernel/vm/pagecache.c`
  的 `PCACHE_VA` 也是 `0xFE900000`（128 个 4KB 页）。经 tmpfs/ffs/jlfs 读文件
  会把缓存页直接映射到引用计数表上，`page_ref_get()` 读回的就是文件内容。

现表：`unsigned short`、`0xFEC00000`、1MB 窗口 → 524288 页 = 2GB。512MB
guest 占 256KB。超过 2GB 的 guest 需要 2MB 窗口，而 `0xFEC00000` 以上已无空间
（见 §10.1 的布局），这一点写在 `PAGE_REF_MAX` 的注释里。

### 10.3 `pmap_enter()` 不会按需建页表

第一次把窗口搬到 `0xFEC00000` 时，内核在 `page_ref_init()` 里就死了：

```
*** kernel TRAP 12 (stack fault) err=0
eip=fe112f8a ... cr2=0
```

`pmap_enter()` 对缺失的 PDE 不分配页表，`pmap_pte()` 返回 0，紧接着
`pmap_pte_pa(pte)` 解引用 0 → 地址 0 上的访存失败。locore 只建了
`0x3f8..0x3fa` 三个 PDE（12MB），所以 12MB 以上的内核窗口一个都进不去。

两处改动：

- `kern_window_install()`（`usermode.c`）：为 4MB 窗口分配并清零一张页表，
  写 `pmap->pm_pdir[pdi] = pa | PG_V | PG_RW`（supervisor，不带 `PG_u`）。
  `kern_table_alloc()` 在每一页映射前检查是否落在 4MB 边界，落在边界就先装
  页表，因此进程表/TCB 池/引用计数表都不再需要各自处理。
- `pmap_enter()` 里 `pte == 0` 直接 `panic("pmap_enter: no page table for va
  %x ...")`。原来的表现是 `TRAP 12 + cr2=0`，完全看不出原因，定位花了很久。

### 10.4 内核栈池 8MB 与"用户页目录只拷一次 PDE"

`NKTHREAD` 从 256 到 1024，`KSTACK_VA` 从 `0xFEA00000` 移到 `0xFF000000`，
共 8MB、跨 `0x3fc/0x3fd` 两个 PDE。栈是**按需映射**的（`kstack_mapped[]`
缓存，槽位首次使用时才取页），所以第 512 个槽对应的 PDE 要等到第 513 个线程
才出现——而用户页目录在开机时就拷走了内核 PDE，那些进程一用到高槽位线程就会
缺页。

`kthread_prime_stacks()`（`main()` 里紧跟 `proc_table_init()`）为栈池的每个
4MB 窗口调用一次 `kern_table_alloc(va, PAGE_SIZE)`：页表建好、页也被映射并
清零。每窗口故意泄漏 4KB（该槽位首次使用时用新页重映射同一 VA），共 8KB。

### 10.5 zcomp 回收：水位太低 + 在缺页路径里压缩，导致自锁

1000 个子进程跑到第 417 个时机器卡死（VMware 直接黑屏，QEMU 里能抓寄存器）：
`EIP` 在 `tlbflush`，`CR2` 固定 `0xbfbfdd69`（proctest 的栈），栈上反复出现
`trap → user_page_fault → zcomp_refault → page_alloc → pmap_map → pmap_enter`。

根因在 `page_alloc()` 的回收钩子：

```c
if (page_free_list == 0 && page_ninuse > 4096 && !in_zcomp && cur != 0)
        (void)zcomp_reclaim(8);
```

- `page_ninuse > 4096` 就是 **16MB**。512MB guest 一旦用到 16MB，之后每次分配
  都尝试压缩 8 页；而 fork 一次要分配页目录、页表、内核栈，于是普通 fork 抖动
  变成压缩风暴。
- 更糟的是它也会在**缺页处理内部**触发：缺页 → 分配页 → 回收压缩了处理程序
  马上要写的页/重试指令要访问的页 → 再次缺页，两者互相追逐。

两处修复：

- 水位改为物理内存的 3/4（小机器保留 4096 页下限），见 `reclaim_watermark()`。
  512MB guest 要到约 384MB 才回收，本节的测试（峰值 125MB）根本不触发。
- 新增 `in_fault`：`user_page_fault()` 在调用 `user_page_fault_locked()` 期间
  置位，`page_alloc()` 在缺页路径内不做回收。

### 10.6 proctest：1000 并发，子进程改成"阻塞"而不是"自旋"

- `TARGET_LIVE = TARGET_SEQ = MIN_LIVE = 1000`。
- phase B 的子进程改成**阻塞在读空管道**上。自旋在 200 个时没问题，1000 个时
  父进程只能拿到 1/1001 的 CPU，下一次 fork 要等几分钟，测试看起来像死机。
  阻塞在 `kthread_block_tcb()` 里不耗 CPU、仍占着进程槽和 TCB，正是这一阶段
  要测的东西；`SIGKILL` 会唤醒它们（`sig_send` → `proc_wake` + `EINTR`），
  所以 kill/reap 循环照旧。
- phase C 改为 `1000 x 128KB = 125MB`（原来 200 x 384KB = 75MB）。仍然明显
  越过旧 CMOS 探测的 64MB/bump 的 63MB 上界，同时塞得进 512MB guest。
- `/dev/ps` 快照缓冲从 8KB 扩到 32KB（1024 行 x 约 24 字节 = 24.6KB）。

### 10.7 实测（QEMU 与 VMware，512MB）

```
usermode: COW refcount 130783 pages (256 KB, 16-bit) at fec00000
kthread: 1024 thread slots at ff000000
usermode: 1024 proc slots at fed00000 (1808 KB)

proctest: dynamic process count test (target live=1000)
proctest: phase A 1000 sequential fork/wait cycles (each child
proctest:   exits at once by design - this proves slot reuse)
proctest: phase B simultaneous live children = 1000 (target 1000)
proctest: kernel /dev/ps lists 1002 live processes (expect at least 1000)
proctest: reaped 1000/1000 children
proctest: phase C 1000 children x 128 KB = 125 MB held at once
proctest: PASS

mem: 2951 pages in use, 52017 allocated, next free 2508000
```

`/dev/ps` 的 1002 条 = 1000 个阻塞子进程 + proctest + sh，是内核自己的进程表
读出来的，不是测试自报的计数。手动看的话在 shell 里跑 `/bin/proctest -i`：
它会在 1000 个子进程全部阻塞后打印进程表的前 6 行、省略提示、后 4 行，按回车
才释放。

余量：512MB guest 下 1000 进程 + 125MB 数据峰值约占 203MB（`next free
0x2508000`），还有一倍以上空间；再往上受 `NPROC = 1024` 和 8MB 栈池限制。

### 10.8 相关文件

- `kernel/sys/usermode.c`：`NPROC`、`PROC_TABLE_VA`、`PAGE_REF_VA`（16 位）、
  `kern_window_install()`、`reclaim_watermark()`、`in_fault`、`ps_snap_buf`
- `kernel/thread/kthread.h` / `kthread.c`：`NKTHREAD`、`KSTACK_VA`、
  `KTHREAD_TABLE_VA`、`kthread_prime_stacks()`
- `kernel/arch/i386/i386/pmap.c`：`pmap_enter()` 缺页表 panic
- `kernel/sys/glue.c`：`main()` 里调用 `kthread_prime_stacks()`
- `libexec/proctest.c`：1000 并发、阻塞式子进程、phase C 125MB
