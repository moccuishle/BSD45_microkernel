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

## 5. 当前调度模型（重要）

- **AP 目前只跑内核 idle loop**（`hlt`），不参与 `proc_sched()`。
- BSP 仍是唯一调度器：用户进程、用户线程、softirq/aio 内核线程都还在
  CPU0 上按原 KLT 1:1 模型调度。
- 因此现在具备的是：
  - 动态拓扑（含 SMT）；
  - 多核 AP 上线、每 CPU 独立 LAPIC/页目录/IDT；
  - 跨核 IPI/TLB shootdown 通路；
  - 每 CPU 运行统计。
- **尚未具备**：用户进程/线程真正并行跑在多个 CPU 上。

### 5.1 为什么还没直接开（不是漏做）

现有调度器是单核全局态：

- `cur` / `cur_thread` / `cur_pd`（usermode.c）和 `curk` / `sched_esp`
  （kthread.c）都是全局变量；
- GDT 只有 **一个** TSS 槽（`GPROC0_SEL`），`hal_kernel_stack_set()` 写的是
  `proc0paddr->u_pcb.pcb_tss`；
- `fair_pick_next()` 没有「TCB 正在哪个 CPU 上跑」的概念，两个 CPU 同时
  `proc_sched()` 会把同一个 TCB 选中两次；
- proc 表/fd/管道/IPC 没有跨 CPU 锁。

直接让 AP 调 `proc_sched()` 会随机踩栈、双跑 TCB、TSS esp0 串台。

---

## 6. 下一阶段（多核调度接管）

按依赖顺序：

1. **per-CPU 执行上下文**
   - `struct cpu_info` 增加 `ci_curproc / ci_curthread / ci_curpd / ci_curk /
     ci_sched_esp / ci_tss`；
   - 用 GS base（MSR 0xC0000101）指向 `cpus[cpuid]`，`smp_cpu_index()`
     从 `%gs:0` 取当前 CPU，避免每次读 LAPIC MMIO。
2. **per-CPU TSS**
   - GDT 扩到 `GPROC0_SEL + MAXCPU`，每个 CPU 一个 TSS 描述符；
   - BSP 用 0x30，AP n 用 `(6+n)*8`，各自 `ltr`；
   - `hal_kernel_stack_set()` 写当前 CPU 的 TSS。
3. **TCB 抢占标记**
   - `struct kthread` 增加 `t_oncpu`（0=空闲，1..MAXCPU=正在该 CPU 运行）；
   - `fair_pick_next()` 跳过 `t_oncpu != 0`；
   - 派发/阻塞/退出在 `sched_lock` 下改 `t_oncpu`。
4. **AP 进入调度**
   - AP idle 循环调用 `proc_sched()`，没有可运行 TCB 时 `hlt`；
   - `kthread_block/exit` 切回**当前 CPU** 的 `sched_esp`，不再回 BSP。
5. **锁与 IPI**
   - proc 表、fd 表、pipe、IPC 端口加自旋锁；
   - pmap 改动调用 `smp_tlb_shootdown()`；唤醒远端 CPU 上的 TCB 用
     `IPI_RESCHEDULE`。
6. **SMT 亲和性**
   - 用 `cpu_smt` / 拓扑 leaf 计算 sibling 对；
   - 同进程线程优先分散到不同物理核，SMT 兄弟只作第二选择。

完成 1–5 后，`proctest` / `pthread` 才能做「多进程 × 多线程 × 多核」的
并行验收；第 6 步是性能优化，不影响正确性。

---

## 7. 相关文件

| 内容 | 位置 |
|---|---|
| CPUID 拓扑、AP 启动、IPI、统计 | `kernel/arch/i386/smp.c`、`kernel/include/sys/smp.h` |
| AP 实模式 trampoline（raw 0x7000） | `kernel/arch/i386/ap_tramp.s` |
| IPI 中断 stub | `kernel/arch/i386/smp_ipi.s` |
| 构建嵌入 | `build_kernel.ps1`、`build_kernel.sh` |
| monitor 命令 | `kernel/sys/mon.c`（`cpuinfo` / `smpon` / `smpping` / `smpinfo`） |
| VMware 2 核测试机 | `vms/BSD45-SMP.vmx` |

---

## 8. 复现

```powershell
cd bsd45
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
# 用 VMware 打开 vms/BSD45-SMP.vmx（4 vCPU / 2GB），串口日志 bsd45-smp-serial.log
```

monitor 里：

```
cpuinfo     # 拓扑：vendor / logical / cores / smt / htt / apic id
smpon       # 映射 LAPIC、建 AP 页目录、INIT-SIPI-SIPI、AP 上线
smpping     # 向所有 AP 发 reschedule IPI
smpinfo     # active/ncpu、IPI 计数、每 CPU ticks
```

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
