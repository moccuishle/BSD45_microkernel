# BSD45 SMP / 多核适配（S412）

> 状态：**第一阶段完成（2026-11-24）**——CPUID 动态拓扑、AP 实机上线、
> Local APIC + IPI 通路、每 CPU 统计已跑通；**用户进程/线程跨核调度**
> 尚未接管，BSP 仍是唯一调度器（见第 6 节）。
> 测试环境：VMware Workstation，`vms/BSD45-SMP.vmx`。

---

## 1. 目标

1. 不写死 CPU 数量：启动时动态识别逻辑 CPU、物理核、超线程（SMT）。
2. 在 x86 32 位非 PAE 端口上把 AP（Application Processor）真正拉起来。
3. 为后续「多核 × 多执行流 × 多线程/多进程」调度打地基：
   - 每个 CPU 有独立的 Local APIC、IDT/GDT/页目录可见性；
   - IPI（TLB shootdown / reschedule / stop）能送达并执行；
   - 保持内核态线程/进程模型（KLT 1:1），不退化成用户态线程库。

---

## 2. 动态拓扑识别（CPUID）

代码：`kernel/arch/i386/smp.c` 的 `cpu_topology_detect()` / `cpuinfo_print()`。

| 信息 | 来源 |
|---|---|
| 厂商字符串 | CPUID leaf 0（EBX/EDX/ECX） |
| 逻辑 CPU 数 | CPUID leaf 1 EBX[23:16] |
| HTT 特性位 | CPUID leaf 1 EDX[28] |
| SMT / 核数 | CPUID leaf 0xB（或 0x1F）：subleaf 0 = SMT 层（type 1），subleaf 1 = Core 层（type 2），EBX[15:0] 为该层逻辑数 |
| 无 0xB 时回退 | CPUID leaf 4 EAX[31:26] = cores-1 |

```
cpu: vendor AuthenticAMD family 25 model 1 step 2
cpu: logical 2, cores 2, smt 1, htt 1
cpu: apic id 0, maxcpu 8
```

- `MAXCPU = 8`，超过时只启用前 8 个逻辑 CPU，并打印截断提示。
- 不再使用 CMOS 0x5F 猜 CPU 数（旧实现）。
- `cpuinfo` monitor 命令可随时查看；`smpon` 时也会打印检测结果。

**超线程模型**：逻辑 CPU 是调度单位，SMT 兄弟核共享物理核的执行资源。
内核调度器看到的是 `logical` 个可运行槽位；`smt` 字段用于后续做
「同进程线程优先放同一物理核 / 分散到不同核」的亲和性策略。

实测：VMware 4 vCPU 配置（`numvcpus=4`, `cpuid.coresPerSocket=2`）下
CPUID 报告 `logical 2, cores 2, smt 1, htt 1`——VMware 对该 32 位 guest
只暴露 2 个逻辑处理器；代码本身按 CPUID 结果走，不依赖 VMX 配置。

---

## 3. AP 启动（真正的 multi-core bring-up）

### 3.1 旧的坑

原 `ap_tramp.s` 是**跟内核一起链接在 0xFE000000+** 的：实模式里
`lgdt _ap_gdtr`、`ljmp _ap_prot32` 都会编码成 `0xFE00xxxx`，AP 一执行就
跑飞。另有三个独立问题：

1. `ap_tramp_installed` 永远是 0，`smp_start_aps()` 直接 return。
2. 低端恒等映射的 PTE 只有 `PG_V`，`pmap_pte()` 又解析不了用户区 VA，
   向 0x7000 拷贝 trampoline 会 `TRAP 12`（`cr2=0x7000`）。
3. `lapic_send_ipi()` 写成 `vector | 0x500`；0x500 是 **INIT 投递模式**，
   所谓 reschedule IPI 实际在反复复位 AP。

### 3.2 现在的方式

- `ap_tramp.s` 改为**独立链接在物理 0x7000 的 raw blob**（不参与内核链接），
  `build_kernel.ps1` 用 `Build-RawStage` 生成 `ap_tramp_bin[]` 嵌入内核。
- trampoline 自带一个**页内临时 GDT**（0x7200，limit 23），因为 AP 实模式
  只能访问 1MB 以内，原内核 GDT 在 0x120b00 读不到。
- 数据块固定在 blob 偏移 0x300（物理 0x7300）：
  - 0x7300 GDTR（limit=23，base=0x7200）
  - 0x7306 AP 页目录物理地址
  - 0x730a AP 内核栈顶（VA）
  - 0x730e CPU id
  - 0x7312 `ap_start()` 虚拟地址
- **AP 专用页目录**：`ap_pd_setup()` 用 `vfs_page_alloc()` 取两页，建
  `0..4MB` 可写恒等映射 + 拷贝内核窗口 PDE（0x3EC..0x3FF），并把自己的
  递归 PTD（PDE 0x3F7）指向自己。AP 不再依赖 IdlePTD 的低端只读映射。
- BSP 通过高地址窗口 `0xFEB00000` 映射物理 0x7000 来写 blob（低端页只读）。
- INIT-SIPI-SIPI 后等待 `ci_active`；AP 在 `ap_start()` 里：
  `lgdt(r_gdt)` + `lidt(r_idt)` → `lapic_init()` → `lapic_timer_init()` →
  `sti` → `hlt` idle。

### 3.3 实测

```
bsd45> smpon
smp: MP table absent, CPUID logical 2 cores 2 smt 1 htt 1
smp: enabling, 2 CPU(s) detected, lapic mapped
smp: AP page dir 5f7000 (low PT 5f8000), tramp win feb00000
smp: AP trampoline 792 bytes at 7000, pd 5f7000
smp: starting 1 AP(s) via INIT/SIPI
smpon: 2 CPU(s) online
```

---

## 4. IPI 通路

- IDT 增加 0x40/0x41/0x42 三个门，指向 `kernel/arch/i386/smp_ipi.s` 的
  轻量 stub（pushal → `smp_handle_ipi(vector)` → popal → iret）。
- `smp_handle_ipi()` 先给 `cpus[me].ci_ticks++`，再按向量处理：
  - 0x40 TLB shootdown：重载 CR3 全刷 TLB；
  - 0x41 reschedule：记录（用户态调度接管后在这里置 need_resched）；
  - 0x42 stop：把该 CPU 标 inactive。
- `lapic_send_ipi()` 修正为固定投递：`ICR_LO = vector`（不再 `|0x500`）。

实测：

```
bsd45> smpping
smpping: IPI sent to active APs
bsd45> smpinfo
smp: enabled=1 active=2 ncpu=2 lapic=mapped
smp: ipi_tlb=0 ipi_resched=1 tlb_flushes=0
smp: cpu0 apic 0 active ticks 0
smp: cpu1 apic 1 active ticks 1
```

`cpu1 ticks 1` 证明 IPI 真的在 AP 上进了中断处理程序，而不是只发出去。

---

## 5. 当前调度模型（S413 已接管）

S413 把「每核执行上下文 / 每核 TSS / TCB 归属 / AP 进调度 / 锁与 IPI」
做完了，S412 列的 1–5 步已经落地，第 6 步（SMT 亲和性）留到 S414。

| 能力 | 状态 |
|---|---|
| 动态拓扑（CPUID，含 SMT/HTT），上限 `MAXCPU=16` | ✅ |
| AP 上线：每 CPU 独立 LAPIC / 页目录 / 页表 / IDT / **TSS** | ✅ |
| 每 CPU 调度状态（`cur` / `cur_thread` / `cur_pd` / `need_resched`） | ✅ |
| TCB 归属 `t_oncpu`（原子 claim/release），一 TCB 只在一个核上跑 | ✅ |
| AP 调 `proc_sched()`，用户进程/线程可跑在任意核 | ✅（默认关闭，见 §5.2） |
| 大内核锁（BKL）+ 进程表/TCB 池/控制台锁、IPC 唤醒 IPI | ✅ |
| SMT 亲和性（同进程线程分散到不同物理核） | ⬜ S414 |

### 5.1 现在的执行模型

```
用户态：所有核并行，互不加锁
   │  系统调用 / 缺页
   ▼
内核态：smp_bkl_enter()  ── 一把大内核锁串行化内核
   │  阻塞 / 返回用户态前 smp_bkl_drop()
   ▼
调度器：每个核独立扫描 kthreads[]，用 cmpxchg 认领没人占的 TCB
```

- **A/B 两把开关**：`smpon` 把 AP 拉起来（TSS、调度状态、IPI 全就绪）；
  `smpuser` 再决定 AP 是否参与用户线程调度。默认 `smpuser` 关，系统行为
  与单核版完全一致，只是多出 3 个在 `hlt` 里待命的核。
- **BKL 契约**：`smp_bkl_enter()` 在系统调用和用户缺页入口；
  `smp_bkl_exit()` 回用户态时；`smp_bkl_drop()` 在**任何要切走的路径**上——
  包括「没活干去 idle」那条，否则持锁 `hlt` 会把其它核全部卡死（S413 第一版
  就死在这里）。
- **归属规则**：一个 TCB 只能被它的 owner 核 `resume_user()`。这样它的
  16 KB 内核栈永远不会被两个核同时使用；跨核迁移只在它阻塞且 owner 核
  重新扫描时发生。
- **延迟回收**：线程在自己栈上退出时把自己的 TCB 槽位交出来，但槽位要等
  本核换到别的栈上才发布（`kthread_reap`），否则另一个核的 fork 会立刻
  拿到同一张栈。

### 5.2 已知问题（必须写明）

`smpon` + `smpuser` 打开后，**部分负载仍会崩**，典型复现是：

```
# ls /bin      ← 子进程在 AP 上跑完并退出
# （回到提示符）
*** kernel TRAP 12 (stack fault) err=0 cpu=0
eip=fe116d50 cs=8 esp=fef001e0
eax=6c6000 ebx=fef2cf10 esi=300 edi=fb000000
cr2=6c6000
```

- 崩点在 `proc_sched()` 里读 `mt->t_state`，而 `mt` 变成了一个页目录地址
  （`0x6c6000`），`esp` 落在 TCB 池（`0xFEF001E0`）——即 BSP 在一次
  `kthread_run()` 的栈切换后跑在了错误的内核栈上。
- 只在「一个进程在 AP 上退出 + 后续再 fork/exec」之后出现；单独跑
  `ls`、`cat`、`ps`、`echo`、`proctest`（1000 子进程）都通过过。
- 已排除：`cpu_id()` 位宽问题（已改成显式掩码）、TCB 槽位复用（已加延迟
  回收并验证 `alloc slot` 不再撞车）、内核栈溢出（已加 `KSTACK_CANARY`
  金丝雀，未触发）。
- S414 已把 `sched_esp`/`curk` 从全局改成每 CPU 一份（见 §11.1），
  「两核写坏同一个全局调度栈」这条路已排除。
- **S417/S418 已修复**（见 §12、§13）：表面症状是 `proc_sched()` 的栈上局部
  `mt`（`cur_thread` 入口快照）变成页目录值；S417 把它移到 per-CPU 全局
  规避了症状。**真正根因是 S418 查明的全局运行队列无锁并发**（见 §13），
  给 `rq_enqueue`/`rq_dequeue` 加锁后，保留栈局部 `mt` 也不再崩。两处修复
  都保留。`smp_user_dispatch` 因验证样本有限仍默认 0，可手动打开演示。

所以默认 `smp_user_dispatch = 0`。要演示多核用户调度：

```
bsd45> smpon       # 4 CPU(s) online
bsd45> smpuser     # AP user dispatch on
bsd45> shell
```

S414 把内核线程改成「每核调度栈」（见 §11.1），全局 `sched_esp` 已消除；
TRAP 12 仍未彻底收敛（见上）。

---

## 6. S413 实现细节

### 6.1 每 CPU 身份：用 TSS 选择子，不用全局变量

SMP 内核不能有「当前 CPU 号」这种全局变量。这里用任务寄存器：

```c
static __inline int cpu_id(void) {
        unsigned sel = 0;
        __asm __volatile("str %0" : "=r" (sel));
        sel &= 0xffffu;                 /* str 只写 16 位，必须显式掩码 */
        sel >>= 3;
        if (sel < CPU_TSS_SEL_BASE || sel >= CPU_TSS_SEL_BASE + MAXCPU)
                return (0);
        return ((int)(sel - CPU_TSS_SEL_BASE));
}
```

- GDT 里 CPU0 的 TSS 在 6 号槽，CPU n 在 `6+n`；`str` 读回的选择子就是
  CPU 号，不需要读 LAPIC MMIO，也不需要 GS base。
- **踩过的坑**：`str %0` 用 `unsigned short` 接收时，GCC 选中的 32 位寄存器
  高 16 位是未定义的；不掩码会偶发算出垃圾 CPU 号，于是 AP 把
  `hal_kernel_stack_set()` 写进了 **BSP 的 TSS**，两核共用一张内核栈。

### 6.2 每 CPU TSS

- `GPROC0_SEL = 6`，`NGDT = GPROC0_SEL + MAXCPU`（16 核 → 22 项）。
- CPU0 沿用 `proc0` u 区里的 TSS（`pcb_tss` 在 `struct user` 偏移 0，所以
  描述符 base 就是 `kstack`）；
- AP n 用 `ap_tss[n]`，`smp_gdt_set_tss()` 填描述符后 `ltr((6+n)<<3)`；
- `hal_kernel_stack_set()` 改成写**当前 CPU** 的 TSS esp0：
  这是 ring3→ring0 唯一的内核栈来源，写错就是两核共栈。

### 6.3 TCB 归属：`t_oncpu`

```c
int  kthread_claim_cpu(struct kthread *t, int id);   /* cmpxchg(-1 -> id) */
void kthread_release_cpu(struct kthread *t);         /* xchg(-1) */
```

- `-1` = 无人占用（新 fork 出来的线程都是），`0..MAXCPU-1` = owner。
- 调度器扫描时 `kthread_owned_by_other(t, cid)` 直接跳过；
  认领失败也跳过。**一句判断就把一个共享扫描变成 N 个独立分发器**：
  所有核走同一张 TCB 表，但只能领没人占的，于是自然分摊。
- 释放点只有三处：显式放弃（本核扫描到 blocked 且 owner 是自己）、
  退出、以及 kernel thread 退出。

### 6.4 每 CPU 调度状态

`usermode.c` 里 `cur` / `cur_thread` / `cur_pd` / `need_resched` 原来是四个
全局变量，现在是一个数组 + 宏：

```c
struct sched_cpu { struct proc *sc_cur; struct kthread *sc_curthread;
                   unsigned sc_cur_pd; int sc_need_resched;
                   struct kthread *sc_idle; int sc_online; unsigned sc_switches; };
static struct sched_cpu sched_cpu[MAXCPU];
#define cur          (sched_cpu[cpu_id()].sc_cur)
#define cur_thread   (sched_cpu[cpu_id()].sc_curthread)
#define cur_pd       (sched_cpu[cpu_id()].sc_cur_pd)
#define need_resched (sched_cpu[cpu_id()].sc_need_resched)
```

上面 397 处 `cur` 引用一行都不用改。`timer.c` 看不到宏，改为调用
`smp_need_resched()`。

### 6.5 AP 进调度

- `sched_percpu_init(id, stack_top)`：给 AP 一个**池外**的 idle 载体 TCB
  （`per_cpu_idle[id]`，不在 `kthreads[]` 里，别的核永远选不中），
  `sc_curthread` 指向它，esp0 指向 AP 启动栈。
- `ap_sched()` → `proc_sched()`：同一个循环，`me == 0` 时跳过「回落自己」。
- 没活干时：`smp_enabled` 为真就 `sti; hlt` 留在循环里（IPI/tick 叫醒），
  不再像单核那样打印 `[no runnable process]` 后永远停机。

### 6.6 锁

| 锁 | 位置 | 保护对象 |
|---|---|---|
| `smp_bkl_lock` | `usermode.c` | 整个内核态（系统调用 + 用户缺页） |
| `proc_table_lock` | `usermode.c` | `procs[]` 槽位分配（`P_ALLOC` 占位） |
| `kthread_alloc_lock` | `kthread.c` | TCB 槽位分配（初始化完成才放锁） |
| `console_lock` | `glue.c` | `printf` 整行输出 |

`next_pid` 用 `atomic_add()`（`lock xadd`），`t_oncpu` 用
`atomic_cmpxchg()`。**不能用 `__sync_*`**：这个 `-march=i386` 目标会把它们
展开成 libatomic 调用，内核不链接，链接期报
`undefined reference to __sync_fetch_and_add_4`。

### 6.7 唤醒 IPI

- `proc_wake()`：被唤醒线程有 owner → `smp_resched_cpu(owner)`。
- fork/vfork 发布子线程后 `sched_kick_other()` 轮转踢一个 AP，
  否则空闲 AP 要等下一个时钟才看见新线程。
- `smp_handle_ipi(IPI_RESCHEDULE)` 只置本核 `need_resched`，**不在 IPI
  帧里切栈**。

---

## 7. 相关文件

| 内容 | 位置 |
|---|---|
| CPUID 拓扑、AP 启动、IPI、统计 | `kernel/arch/i386/smp.c`、`kernel/include/sys/smp.h` |
| AP 实模式 trampoline（raw 0x7000） | `kernel/arch/i386/ap_tramp.s` |
| IPI 中断 stub | `kernel/arch/i386/smp_ipi.s` |
| 构建嵌入 | `build_kernel.ps1`、`build_kernel.sh` |
| monitor 命令 | `kernel/sys/mon.c`（`cpuinfo` / `smpon` / `smpuser` / `smpping` / `smpinfo`） |
| VMware 测试机 | `vms/BSD45-SMP.vmx`（4 vCPU / 2GB）、`vms/BSD45-SMP8.vmx`（8 vCPU / 3GB） |
| 无窗口发按键 | `tools/vmkeys.py`（Python + user32，支持 `/ - .` 等符号） |
| 每 CPU TSS | `kernel/arch/i386/isa/machdep.c`（`smp_gdt_set_tss`） |
| 每 CPU esp0 | `kernel/arch/i386/ctx.c`（`hal_kernel_stack_set`） |
| 每 CPU 调度状态 / BKL | `kernel/sys/usermode.c` |
| TCB 归属 / 延迟回收 / 栈金丝雀 | `kernel/thread/kthread.c` |

---

## 8. 复现

```powershell
cd bsd45
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
# 用 VMware 打开 vms/BSD45-SMP.vmx（4 vCPU / 2GB），串口日志 bsd45-smp-serial.log
```

monitor 里：

```
cpuinfo     # 拓扑：vendor / logical / cores / smt / htt / apic id / maxcpu
smpon       # 映射 LAPIC、建 AP 页目录、INIT-SIPI-SIPI、AP 上线（AP 只 idle）
smpuser     # 切换：AP 是否参与用户线程调度（默认 off）
smpping     # 向所有 AP 发 reschedule IPI
smpinfo     # active/ncpu/maxcpu、IPI 计数、每 CPU ticks + 每 CPU 调度统计
```

`smpinfo` 新增每核一行：

```
smp: sched cpu0 switches 12 running tid 3
smp: sched cpu1 switches 4 running tid 5
```

`switches` 不同、`running tid` 不同，就是「用户线程真的并行跑在多个核上」
的证据；全部集中在 cpu0 说明 AP 没参与（`smpuser` 关着）。

---

## 9. 踩坑记录（S412）

1. **trampoline 不能跟内核一起链接**：实模式绝对地址必须是物理地址；
   必须单独 `ld -Ttext 0x7000` 出 raw blob。
2. **AP 实模式读不到 1MB 以上的 GDT**：内核 GDT 在 0x120b00，必须用
   trampoline 页内的临时 GDT。
3. **低端恒等映射是只读的**，且 `pmap_pte()` 不解析用户区 VA；写 0x7000
   要用高地址窗口 `pmap_map(0xFEB00000, 0x7000, ...)`。
4. **IdlePTD 的低端映射不可靠**：给 AP 建独立页目录（0..4MB 可写恒等 +
   拷贝内核 PDE），否则一开分页就 triple fault。
5. **`vector | 0x500` 是 INIT 不是 IPI**：固定投递只写向量号。
6. **AP 必须 `sti`**：trampoline 开头 `cli`，不开中断 IPI 永远挂起。
7. **AP 必须 `lidt(r_idt)`**：否则 IPI 没有门，直接 triple fault/丢失。
8. **不要从 AP 直接 printf**：控制台没有跨核锁，输出会交错；AP 上线由
   BSP 的 `smpon: N CPU(s) online` 汇总。

---

## 10. S413 实测（VMware，4 逻辑 CPU / 3 GB）

### 10.1 拓扑与 AP 上线

```
bsd45> cpuinfo
cpu: vendor AuthenticAMD family 25 model 1 step 2
cpu: logical 4, cores 4, smt 1, htt 1
cpu: apic id 0, maxcpu 16

bsd45> smpon
smp: MP table absent, CPUID logical 4 cores 4 smt 1 htt 1
smp: enabling, 4 CPU(s) detected, lapic mapped
smp: AP page dir 689000 (low PT 68a000), tramp win feb00000
smp: AP trampoline 792 bytes at 7000, pd 689000
smp: starting 3 AP(s) via INIT/SIPI
smpon: 4 CPU(s) online
```

VMware 对 32 位 guest 只暴露 4 个逻辑 CPU（`numvcpus=8` 也报 logical 4），
代码以 CPUID 为准；`MAXCPU=16` 只是编译期上限，核数仍由 CPUID 决定，
所以同一份二进制在 2/4/6/8/12/16 核机器上都能直接用。

### 10.2 单核等价性（默认，`smpuser` 关）

`smpon` → `smpinfo` → `shell` → `ls /bin` → `cat /readme.txt` → `ps` →
`exit 0` 全流程无 TRAP，输出与单核版一致。AP 在 `hlt` 里待命，
`smpping` 能唤醒它们（`cpu1 ticks` 增加）。

### 10.3 多核用户调度（`smpuser` 开）

```
# ls /bin
smp: cpu3 dispatch tid 2 pid 2 pd 6f3000 eip 80480a7
execve /bin/ls: entry 8048000 ...
aiotest.elf ... wc.elf                ← 子进程完整跑在 CPU3 上
# ps
smp: cpu1 dispatch tid 3 pid 3
PID PPID PGRP STAT COMM
1 0 1 BLOCK sh
3 1 3 RUN ps                          ← ps 自己跑在 CPU1 上
```

- `ls`、`cat`、`ps`、`echo`、`uname` 能在 AP 上完整跑完并正确输出；
- `proctest`（1000 个同时存活子进程，`/dev/ps` 列出 1002 条）在 4 核下跑完，
  期间 TCB 槽位在 768 个上限内反复复用；
- 之后仍有 §5.2 描述的 TRAP 12，所以默认不开。

### 10.4 S413 踩坑记录

1. **`str` 的 16 位输出必须掩码**。`unsigned short sel; asm("str %0")` 让 GCC
   选中一个 32 位寄存器，高半未定义。不掩码时 AP 偶尔得到垃圾 CPU 号，
   `hal_kernel_stack_set()` 写进 BSP 的 TSS，两核共用一张内核栈，
   表现为 `cat` 输出到一半 TRAP 12、帧里全是用户地址。
2. **BKL 必须在所有离开内核的路径上释放**，包括「没活干 → `hlt`」。
   只在 `resume_user()` 之前放锁，会让 BSP 持锁 `hlt`，AP 永远卡在
   `smp_bkl_enter()` 里（第一版 SMP 就死在这里，表现为 `ls` 无输出）。
3. **新 TCB 不能先置 `KT_RUN` 再填上下文**。`kthread_user_spawn()` 里
   `t_state = KT_RUN` 之后调用者才写 `t_regs`/`uk_regs`，另一个核能在窗口里
   派发它并 `iret` 到 eip=0。改成创建时 `KT_BLOCK`，上下文填完再发布。
4. **释放 TCB 槽位要延迟**。线程在自己栈上退出时释放自己的槽位，另一个核
   立刻 fork 会拿到同一张栈。`kthread_reap()` 等本核换栈后再发布。
5. **不要误释放别人的 claim**。扫描到「已阻塞」线程时无条件
   `kthread_release_cpu()` 会把 owner 核的占位撤掉，导致 shell 被 AP 抢走。
   只有 `t_oncpu == cid` 才能释放。
6. **`__sync_*` 在 `-march=i386` 下要 libatomic**。用内联汇编
   `lock cmpxchgl` / `lock xaddl` / `xchgl` 代替。
7. **AP 也要 `lldt`**。用户态选择子都在 LDT 里，AP 不加载 LDTR，
   `resume_user` 的 `mov %edi,%ds`（选择子 0x1b/0x23）直接 9 号异常。
8. **控制台要加锁**。两个核同时 `printf` 会把一行撕成交错字符，
   trap dump 会变得不可读；`printf`/`kprintf` 现在整行持 `console_lock`。

---

## 11. S414：每核调度栈 + IPI 段修复（2026-11-24）

### 11.1 per-CPU `sched_esp` / `curk`

S413 的内核线程半调度用**一个全局** `sched_esp`（kswitch 的返回栈）
和一个全局 `curk`。两个核同时 `kthread_poll()` 时会互相覆盖这两个变量，
第一个核「返回」时就落到别人的栈上。S414 把它们改成按 `cpu_id()` 索引
的数组，`curk` 变成头文件里的宏 `curk_percpu[cpu_id()]`，约 15 处既有引用
零改动（`sync.c` 的 `extern` 随之删除）。

配套地把 `kthread_exit()` 的槽位释放改成延迟回收：线程不能在自己还站着的
栈上把槽位发布出去，否则另一个核的 fork 会拿到同一张栈。现在由
`kthread_reap()` 在本核换栈后再发布。

`kthread_run()` 仍固定 BSP：运行队列是全局链表，AP 要 poll 还得先给它
加自己的锁或按核分队列。用户线程不受此限，`proc_sched()` 在所有核分发。

### 11.2 IPI 段寄存器修复

`resume_user()` 在 `iret` 之前把用户数据选择子（0x27，limit `0xFDC00000`）
装进 `ds/es`。IPI stub 原本不切段，若中断正好落在这个窗口，
`smp_handle_ipi()` 第一条读内核全局（`lapic_va`）的指令就用用户段访问
`0xFE...`，触发 #GP（陷阱打印成 TRAP 9，AP 上 `cs=8 ds=0x27`）。
S414 的 `IPI_PROLOGUE` 现在保存 `ds/es`、切到内核段 0x10、返回时恢复。
改完后 AP 上那条 #NM/#GP 消失，剩余崩溃集中到 BSP 的 TRAP 12（§5.2）。

### 11.3 诊断与 TRAP 现状

`hal_kernel_stack_set()` 加了「esp0 落进 TCB 池」断言，`kthread_poll()`
加了 `t_ksp` 范围断言，`proc_sched()` 入口加了 `cur_thread` 合法性断言，
循环末尾加了栈帧诊断。**这些断言全部为 0**——崩溃时 `mt` 在入口是合法的，
是循环运行中被写坏的。加上循环末尾那段只读诊断后，同一负载连续 20+ 次
fork/exec 不再复现 TRAP，说明故障对栈帧布局敏感。**S417 随后定位并修复
了这条 TRAP（见 §12）**：`mt` 栈槽被深层调用覆盖，改为 per-CPU 全局。
该诊断块在 S417 里已移除。`smp_user_dispatch` 因验证样本有限继续默认 0；
多核用户调度仍需手动 `smpon` + `smpuser`。

### 11.4 S415：SMT / cache 亲和性

拓扑已在 S412 由 CPUID 检测（`cpu_logical/cores/smt/htt`，见 `cpu_topology_detect`）。
S415 补上放置侧：

- `smp.c` 新增 `smt_phys_core(apic_id)`（`apic_id / cpu_smt`）与
  `smt_sibling(apic_id)`（无 SMT 返回 -1）。
- `fair_pick_next()` 增加 `smt_affinity_score(t, cid)`，仅作**同优先级裁决**：
  上次在本核跑 +3（cache 热）、上次在同物理核的 SMT 兄弟上 +1（核内迁移
  便宜）、其余 0。它不改动态优先级、不改抢占判定，BSD 调度语义不变。
- `t_lastcpu` 在 `kthread_claim_cpu()` / `kthread_poll()` 里持续更新，
  作为亲和性依据。

效果依赖硬件：VMware 报 4 逻辑 / 4 物理核 / smt=1，兄弟路径不触发，
只有「线程留在上次那颗核」的 cache 亲和生效；真 SMT 机器上才会走 +1
的核内兄弟迁移。原子 claim 保证即使提示过期也不会两核跑同一线程。

### 11.5 本轮验证

- VMware 8 vCPU 配置（`vms/BSD45-SMP8.vmx`），`smpon` → 4 CPU 上线，
  `cpuinfo` 报 logical 4 / cores 4 / smt 1 / htt 1；
- `smpuser` + `shell` 下 `ls /bin`、`cat /readme.txt`、`date`、`ps`、`uname`
  反复执行，TRAP / 栈诊断计数均为 0，`smp_print` 显示多个 CPU 都有切换；
- 默认（不敲 `smpon`）仍为纯单核，与 S412 之前行为一致。

---

## 12. S417：TRAP 12 诊断收尾与修复（2026-11-24）

### 12.1 诊断过程（关键证据）

S414 留下的问题：`smpon`+`smpuser` 后，BSP 在 `proc_sched()` 循环末尾读
`mt->t_state` 时崩，`mt=0x6c6000`（页目录），`esp` 打印成 `fef001e0`
（TCB 池）。S417 用一系列**不占被怀疑栈帧**的探针定位：

1. **切栈环形日志**（`hal_switch_stack` 里写 .bss）：崩溃全程只有 2 次
   kswitch，都是 cpu0 切进 softirq/aio（`fc00bda0 → fc003fec/fc007fec`），
   **没有任何一次切到 TCB 池**。排除「kswitch 恢复错栈」。
2. **trap 内读真实 esp**：`trap runs on esp=fc00bd90 frame=fc00bdbc`，
   都在合法栈池；打印出的 `tf_esp=fef001e0 / ss=0` 是 `pushal` 的 esp 槽
   保留的旧值（x86 PUSHA 的 esp 槽怪癖），**不是崩溃时的真实栈**。排除
   「栈跳进 TCB 池」。
3. **反汇编定位**：崩溃指令 `cmpl $0x1,(%eax)`，`eax` 来自 `0x10(%esp)`
   ——即 `mt` 这个**栈上局部变量**。全函数体里 `0x10(%esp)` 只读不写，
   但相邻的 `me`（`0x14(%esp)`）崩溃时仍是合法 proc 指针 `fb000000`。
   只有 `mt` 这一个槽被写成页目录值 = 精确的栈槽覆盖，不是整体栈错位。
4. **布局敏感性**：任何加进 `proc_sched()` 的探针（哪怕只是一句读 esp）
   都让崩溃消失——典型的栈局部被深层调用链越界写覆盖的确证。

### 12.2 修复

`mt` 是「调用调度器的那个 TCB」，作用范围覆盖整个 for 循环，却在循环里
经历 `sig_deliver` / `resume_user`(iret) / syscall 重入等深层调用。把它从
栈局部移到 **per-CPU 全局** `ps_mt[MAXCPU]`：入口与断言分支各写一次，
循环顶端 `mt = ps_mt[cid]` 重新加载。栈上的副本不再作为权威来源，即使
该槽被覆盖也不影响调度。语义与原代码一致（原本 `mt` 也是入口快照，
循环内不更新）。

### 12.3 验证

同样的 VMware 8 vCPU / `smpon`+`smpuser`+shell 流程，**修复前每次必崩**，
修复后连续 `ls /bin`×4 + `cat`/`ps`/`date`/`uname` 混合负载全部通过，
TRAP 计数 0，系统未卡死（`echo` 有响应）。

### 12.4 仍存的次要问题

- 多核 idle 时 `smp_print()` 与用户命令的串口输出会字符交错
  （`printf` 有 `console_lock`，但用户态 `write` 走另一条路径）。
- 高负载下观察到一次系统无响应，疑似 `console_lock` 在中断上下文自锁；
  未稳定复现，留作后续。
- 因验证样本仍有限，`smp_user_dispatch` **继续默认 0**；手动 `smpon`+
  `smpuser` 可用于演示。

---

## 13. S418：真正的根因——全局运行队列无锁（2026-11-24）

`os_smp.md` §12 把 TRAP 12 归因于 `mt` 栈槽被覆盖，并用 S417（把 `mt` 移到
per-CPU 全局）规避。这个结论**不完整**：它只让那条特定路径不再读到坏值，
并没有消除破坏源。用户指出真正的方向——**内核多线程更新无锁链表**——经
代码审查与对照实验证实：

### 13.1 证据

- `kthread.c` 的 `rq_head`/`rq_tail` 是**全局单向链表，全程无锁**；
  `rq_enqueue()` 的 `rq_tail->t_rqnext = t; rq_tail = t;` 和 `rq_dequeue()`
  的前驱遍历 + `rq_head`/`rq_tail` 改写都是读-改-写。
- SMP 后这些操作来自多个核：`kthread_wake()`（line 579，`rq_enqueue`）可由
  AP 上的 aio/softirq/管道唤醒调用；`kthread_block()`/`kthread_exit()`
  （line 563/592，`rq_dequeue`）同样可在 AP 发生；BSP 的 `kthread_poll()`
  也在 dequeue。两个核同时改链表 → `t_rqnext` 变成野指针、`rq_tail` 指向
  已出队 TCB → TCB 槽被写坏 → 表现为 `cur_thread`/`mt` 变成页目录值。

### 13.2 对照实验（关键）

| 构建 | mt 位置 | rq 锁 | 结果 |
|---|---|---|---|
| 362961f | 栈局部 | 无 | 每次 `ls /bin` 必崩 TRAP 12 |
| 实验 A | 栈局部 | **有** | 连续 7× `ls /bin` 不崩 |
| S417 (90f3c55) | per-CPU 全局 | 无 | 混合负载不崩（只是规避了症状） |

实验 A 保留了会崩的栈局部 `mt`，只加 rq 锁就不崩——**证明根因是链表竞争，
不是栈槽本身**。而且 rq 锁加在 `kthread.c`，不改变 `proc_sched` 的栈帧布局，
排除了"又一次布局掩盖"。

### 13.3 修复

`rq_enqueue()`/`rq_dequeue()` 全程持 `rq_lock`（普通自旋锁；中断处理不碰
该队列，`sched_tick` 只读抢占状态，故无需关中断）。`t_onrq` 幂等检查和
`uk_rq_threads` 记账一并移入临界区。S417 的 per-CPU `ps_mt` 保留作防御。

S417 + rq 锁在普通负载（`ls`/`cat`/`ps`/`date`/`uname`）下 TRAP=0，
但 `proctest`（1000 并发子进程）加压又暴露第三处无锁共享状态——scratch
临时映射窗口，见 §14（S420 每核独立 scratch）。

---

## 14. S420：加压暴露的 scratch 窗口竞态（2026-11-24）

用户要求加压确认（`proctest`，1000 并发子进程）。结果**又复现了 TRAP 12**，
但这次不在 `proc_sched`，而在 `page_alloc`/`proc_exit`，`cpu=1/2`，
`cr2=fe202000`——是一片**新的**无锁共享状态。

### 14.1 第三处根因：scratch 临时映射窗口

`SCRATCH_PD/PT/PAGE`（0xFE200000/1000/2000）与 `SCRATCH2_*`（0xFE203000..)
是**6 个固定 VA 的临时映射窗口**，`map_page()` 每次把不同物理页映射到这些
VA，调用方随即读写。BKL 本应串行化所有使用者，但插桩计数显示
**proctest 运行中 54205 次 map_page 有 467 次在 BKL 之外**——AP 的
`map_page()` 会在 BSP 正使用窗口时把它重指向别的物理页，踩坏页表/TCB。
（741 行的 `in_zcomp`/`in_fault` 只是防重入标志，跨核不是锁。）

### 14.2 修复：每核独立 scratch 窗口

把 6 个宏改为按 `cpu_id()` 索引：
`#define SCRATCH_GRP (6*cpu_id())`，各 VA `+ SCRATCH_GRP*0x1000`。
16 CPU × 6 页 = 384 KB，落在 PDE 0x3F8 的 4 MB 窗口内，止于 0xFE260000，
低于进程表 0xFE340000；PTE 由 `pmap_map()` 按需安装。共享消失，BKL 漏洞
不再能造成跨核踩踏。

### 14.3 验证与遗留

- 每核 scratch 版：`proctest` 与普通 ls/cat 负载下**内核 TRAP=0**；
  普通负载（smpon+smpuser+shell+ls）回归通过。
- **遗留 1（已由 S421 修复，见 §15）**：proctest 下的子进程
  `user fault sig11 eip=va=80480a7`，根因是 CR3 缓存失配，不是 fork 页表复制。
- **遗留 2**：连续多次 `vmkeys` 发命令时出现 `foreground was ...` 焦点提示
  且命令不再生效；对照实验显示共享 scratch 版同样如此，是键盘工具/VMware
  窗口焦点问题，不是内核死锁（两版 TRAP 均为 0）。
- 因遗留问题，`smp_user_dispatch` 继续默认 0。

---

## 15. S421：AP 子进程 SIGSEGV 的真因——CR3 缓存失配

用户要求继续查 §14.3 遗留 1。这次用"先分流、再抓现场、逐个否证"的方法，
没有像前几轮那样在栈布局上猜。

### 15.1 分流：是不是 SMP 特有？

| 配置 | 内核 TRAP | user fault |
|---|---|---|
| 单核（不 smpon）proctest | 0 | **0** |
| SMP（smpon+smpuser）proctest | 0 | **19~191（逐轮波动）** |

单核完美通过、SMP 必现且数量波动 → 确是 SMP 竞态，值得查。

### 15.2 抓现场：不是缺页，是 #SS

在 `trap()` 的用户态分支给 trap 12 插桩，一轮拿到全部寄存器：

```
S421 #SS: cpu2 err=4 eip=80480a7 cs=1f ss=27 cr3=1d2000
```

- `eip=80480a7` 是 `___syscall` 里 `int $0x80` 之后的第一条 `pop %ebx`，
  即子进程的 `_exit(0)` 返回点；反汇编 `80480a5: int $0x80; 80480a7: pop %ebx`。
- `cs=1f`、`ss=27` 是合法的用户 LDT 选择子（TI=1），LDTR=0x18 已加载，正常。
- **`err=4`、`cr3=1d2000`** —— `1d2000` 是 **IdlePTD（内核页目录）的物理地址**，
  不是该进程的 `702000`/`724000`。用户态 iret 时 CR3 还停在内核目录 → #SS。
- 全部发生在 cpu=1/2（AP），单核无。

### 15.3 根因：cur_pd 缓存被 proc_exit 绕过

`cur_pd` 是 per-CPU 缓存（`sched_cpu[cid].sc_cur_pd`），`switch_pd_for()`
和 `sched_tick()` 都用 `p->uk_pd != cur_pd` 判断要不要切 CR3。但：

- `proc_exit()`（line 4118）直接调 `hal_address_space_activate(IdlePTD)`
  切到内核目录，**完全绕过 cur_pd 缓存**——CR3 变成 `1d2000`，
  `cur_pd` 却还记着刚退出进程的用户 pd。
- 之后 AP 上 clock IRQ 的 `sched_tick()` 选中一个 `p->uk_pd == cur_pd`（缓存
  命中）的线程，判定"无需切 CR3"，于是 `*r = *regs; iret` 回用户态，
  但 **CR3 仍是 IdlePTD** → 取指立刻 #SS err=4。
- 单核上 proc_exit 后总能切到阻塞在 wait4 的父进程、CR3 被正常重设，故不复现。

先后的几个假设都被实验否证（记录在案，避免重走）：
1. **page_alloc 无锁竞态**：加了 page_lock（next_pa/free_list/refcount）后
   user fault **未降**（191）→ 否证，改动已还原。
2. **proc_sched fall-back 到已退出进程**：把条件收紧成
   `uk_state==P_RUN && uk_pd!=0` 后 user fault **未降**（191）→ 否证，已还原。
3. **AP 未加载 LDTR / LDT**：现场 `LDTR=0x18` 有效 → 否证。

### 15.4 修复：CR3 判断读真实值

两处判断改为读 `hal_address_space_current()`（真实 CR3）而非缓存：
- `switch_pd_for()`：用 live CR3 比较，命中时顺手 `cur_pd = live_pd` 重同步；
- `sched_tick()`：同样用 live CR3，避免 IdlePTD 被误判为"已是目标 PD"。

这样任何绕过缓存直接改 CR3 的路径（proc_exit 的 IdlePTD、以及所有临时
`activate(save)` 恢复）都不会再让 iret 带着错误 CR3 回用户态。

### 15.5 验证

| 配置 | TRAP | user fault | proctest phase B |
|---|---|---|---|
| 修前 SMP（三轮） | 0 | 19 / 57 / 191 | 1000 live |
| **修后 SMP（三轮）** | 0 / 1 / 0 | **0 / 0 / 0** | 1000 live |

修后三轮 user fault 全为 0，1000 子进程全部驻留。第二轮偶发 1 个 BSP
TRAP 12（`cpu=0`、崩在 `proc_sched` 尾部 `fe1171a3`、esp 在 TCB 池），当时
判为 §12/§13 老问题的偶发复现，与本次 CR3 修复（AP、#SS、IdlePTD）表现
不同。

### 15.6 TRAP 12 收敛判定（2026-11-24 收尾）

那 1 次 BSP TRAP 12 之后又做了大量加压，均未再复现：

| 负载 | 轮数 | TRAP 12 |
|---|---|---|
| proctest（reap 加锁版） | 7 | 0 |
| proctest（原始版对照） | 5 | 0 |
| thrash（8 进程 × 400 fork/exit/reap 并发压测，重建后丢弃） | 4 | 0 |

合计 **16 轮 0 复现**。据此判定 BSP TRAP 12 已收敛——很可能是 S421 的
CR3 修复连带消除：崩溃时 `cpus[cid].ci_curthread = mt` 的下标
（`0x1c(%esp)+0x20(%esp)`）被写成了栈地址 `fc31dd60`，本质是 `proc_sched`
在错误的地址空间上下文里执行，而 CR3 与 `cur_pd` 失配正能让内核"看着
正确的栈、却用着错误的页表"。当时把这次偶发判成"无关老问题"可能判错了。

曾怀疑 `kthread_reap()` 发布 `t_used=0` 不持 `kthread_alloc_lock`（与
`tcb_reserve()` 锁域错配）是根因，加了锁并验证 8 轮全过；但**不加锁的
对照基线同样 8 轮全过**，对照无区分度，故不把该改动作为修复提交，已回退。
该锁域错配是真实存在的结构性隐患，留给后续在能稳定复现时再验证。

`smp_user_dispatch` 仍默认 0：user fault 已清零、TRAP 12 判定收敛，但
故障曾为极低频偶发，默认打开前建议再长时间 soak。
