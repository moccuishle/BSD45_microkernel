# BSD45 调度系统与优先级模型

> 代码位置：`bsd45/kernel/thread/kthread.{c,h}`（TCB、优先级算法、继承、抢占门）、`bsd45/kernel/sys/usermode.c`（时钟抢占 `sched_tick`、选择算法 `fair_pick_next`、estcpu 衰减、协作式调度 `proc_sched`）、`bsd45/kernel/arch/i386/timer.c`（IRQ0 挂接）
> 性质：**单 CPU、抢占式、20 级动态优先级 + 时间片轮转**的混合调度器；内核线程与用户线程共用同一套 TCB 池，但走两条不同的选择路径。

## 一、调度对象与容量

| 项 | 值 | 位置 |
|---|---|---|
| TCB 总数 | `NKTHREAD = 16` | `kthread.h:21` |
| 进程数上限 | `NPROC = 1024`（S410，进程表在 `0xFED00000`） | `usermode.c` |
| TCB 数 | `NKTHREAD = 1024`（TCB 池在 `0xFEF00000`） | `kthread.h` |
| 内核栈 | 每个 TCB 8 KB，VA 起点 `0xFF000000`（8MB，S410） | `kthread.h` |
| CPU 数 | 1（`t_lastcpu` 已预留亲和性字段，SMP 未启用） | `kthread.c:343` |

调度单位是 **TCB（`struct kthread`）而不是进程**；TCB 通过 `t_proc` 反指所属进程，同一进程的多个线程共享页目录（切换线程时若 `p->uk_pd == cur_pd` 就不重载 CR3）。

常驻的内核线程只有两个：`softirq`（软中断）和 `aio`（异步 I/O 提交队列），都由 `kthread_spawn()` 创建、初始优先级 `PRI_NORMAL`。

## 二、优先级尺度：0 最低，19 最高

与 Unix 传统方向**相反**：这里的**数值越大优先级越高**，共 `NPRIO = 20` 级。

| 常量 | 值 | 用途 |
|---|---|---|
| `PRI_LOWEST` | 0 | 保底 |
| `PRI_IDLE` | 2 | 空闲 |
| `PRI_BATCH` | 5 | CPU 密集型基线（低） |
| `PRI_NORMAL` | 10 | 默认（新建线程、`schedtest` 起点） |
| `PRI_INTERACTIVE` | 14 | 交互 / I/O 密集型基线 |
| `PRI_HIGH` | 17 | 高优先级 |
| `PRI_HIGHEST` | 19 | 上限 |

**新建线程一律 `PRI_NORMAL = 10`**（`kthread.c:175`、`kthread.c:205`），用户态可以用系统调用 446 自行改成 0..19。

## 三、有效优先级公式

`t_prio` 只是基准，真正参与比较的是 `sched_effective_prio()`（`kthread.c:457`）：

```
p = t_prio
  + PRI_BOOST_IO      (6)   若 t_sleep_ticks > 0 且 状态为 KT_RUN   ← 睡醒/刚做完 I/O
  + PRI_BOOST_STARVE  (8)   若 t_starve >= STARVE_THRESHOLD (40)   ← 反饥饿
  - sched_interaction_penalty()                                    ← estcpu + nice 惩罚
  p = max(t_inherit_prio, p)                                       ← 优先级继承覆盖
  p = clamp(p, 0, 19)
```

交互性惩罚（`usermode.c:2544`，参照 4.4BSD 的 `usrpri = PUSER + estcpu/4 + 2*nice`，压缩到 0..8 级）：

```
penalty = estcpu / 32 + (2 * nice) / 4        # nice 只取正值，nice<=0 不计惩罚
penalty = min(penalty, 8)
```

即：**CPU 占用越多（estcpu 高）、nice 越大（主动让出）→ 扣得越多；常常睡觉等 I/O 的进程 estcpu 低 → 保持高优先级**。这就是"交互式进程响应快"的实现方式。

## 四、时间片与量子

| 常量 | 值（tick） | 说明 |
|---|---|---|
| `CLOCK_HZ` | 100 Hz | PIT 分频，`printf("clock: 100 Hz, IRQ0 enabled, quantum 2 ticks")` |
| `clock_quantum` | 2 | 定时器层面的重调度粒度（`timer.c:34`），每 2 tick 置一次 `need_resched` |
| `QUANTUM_BASE` | 4 | 默认时间片 |
| `QUANTUM_CPU` | 8 | 判定为 CPU 密集型后加长，摊薄切换开销 |
| `QUANTUM_IO` | 2 | 长睡眠线程的短时间片（**代码路径存在但实际取不到，见第八节**） |

时间片是**每线程**的（`t_quantum`），公平性是**每进程**的（`uk_estcpu` 破平局）。

## 五、抢占规则（时钟中断路径）

IRQ0 → `timer.c` → 仅当被中断的是**用户态**（`r->cs & 3 == 3`）才调 `sched_tick(r)`（`timer.c:103`）。内核态不直接抢，只置 `need_resched`。

`sched_tick()`（`usermode.c:2593`）的判定顺序：

1. **门禁**：`t_preempt_cnt > 0`（在内核临界区）或 `t_base_ipl >= KTHREAD_NO_PREEMPT (5)` → 只置 `need_resched = 1` 返回，**推迟到安全点**。
2. **记账**：`uk_estcpu++`（封顶 255）、`sched_on_tick()` 累计 `t_ticks` 并检测 CPU 密集、每 100 tick 做一次全局 estcpu 衰减、给其他就绪线程 `t_starve++`（封顶 80）。
3. **选下一个**：`fair_pick_next(me)`。
4. **是否切**：
   ```c
   if (t == 0 || (!quantum_expired && !need_resched && next_prio <= me_prio))
           return;      /* 继续跑当前线程 */
   ```
   即 **只有严格更高优先级才能立即抢占**；同优先级必须等自己量子耗尽（`t_ticks >= t_quantum`）才轮转 —— 这就是"优先级 + 时间片"的混合语义。
5. **切换**：必要时 `hal_address_space_activate()` 切页目录、换内核栈、用 `hal_intr_user_resume()` 直接改写中断帧返回新线程（不经过 `iret` 回原线程）。

## 六、选择算法 `fair_pick_next()`

`usermode.c:2567`：

```
从 (me 的下一个槽位) 开始遍历全部 16 个 TCB（环形，避免固定偏向低槽位）
  跳过 自己 / 不可运行
  取 sched_effective_prio() 最大的
  优先级相同时，取 uk_estcpu 更小的（进程级公平：线程多的进程不能靠数量碾压）
```

`tcb_runnable()` 的额外规则（`usermode.c:2522`）：进程处于 `P_FREE/P_ZOMBIE` → 不可运行；进程整体 `P_BLOCKED` 且只有 1 个线程 → 不可运行（经典 wait/pipe 语义）；**多线程进程某个 TCB 阻塞时其余兄弟照常运行**。

## 七、优先级继承（priority inheritance）

只在一个地方触发：`thread_join()`（`usermode.c:3777`）。

- 高优先级线程 join 低优先级线程时，`prio_inherit(joiner, joinee)` 把 joiner 的有效优先级**借给** joinee，让它尽快跑完、尽快释放 joiner。
- 记原值：首次捐赠时把 `t_prio` 存进 `t_base_prio`；`t_inherit_prio` 取多次捐赠的最大值。
- **可传递**：若 holder 自己也阻塞在第三个线程上（`t_blocked_on >= 0`），会继续沿链 `prio_inherit(holder, next)`。
- `prio_restore()` 在释放时清 `t_inherit_prio` 并恢复 `t_prio = t_base_prio`。
- 计数器：`ipl_donations`（每次成功捐赠 +1）。

## 八、抢占门禁（preemption gate）与 IPL

两套并存的控制手段：

| 手段 | 字段 | 语义 |
|---|---|---|
| 临界区计数 | `t_preempt_cnt` | `preempt_disable()` 可嵌套累加，归零前不可抢占；`preempt_enable()` 归零时可能触发调度 |
| 基准 IPL | `t_base_ipl`（0..15） | `>= KTHREAD_NO_PREEMPT (5)` 表示该线程主动退出抢占；`thread_ipl_set()` 设定，`thread_ipl_boost()/unboost()` 临时提升 |
| 有效 IPL | `t_eff_ipl` | `base_ipl` 加上临时提升后的值，计数器 `ipl_boosts` |
| 可抢占标志 | `t_preemptable` | 资源是否可被接管 |

**安全点**（`usermode.c:5113`）：系统调用返回用户态前，若 `need_resched` 且 `t_preempt_cnt == 0 && t_base_ipl == 0 && t_preemptable`，才真正调度 —— **绝不在内核执行中途抢占**。

## 九、两条调度路径（重要实现细节）

| 路径 | 触发 | 选择依据 |
|---|---|---|
| **时钟抢占** | IRQ0 + 用户态 + 门禁通过 | `fair_pick_next()`：**按有效优先级**、estcpu 破平局 |
| **协作式** | `thread_yield` / 阻塞 / 退出 / 系统调用安全点 → `proc_sched()` | **按 TCB 槽位下标顺序**取第一个可运行者，**不看优先级** |

也就是说：**优先级只在"抢占"这一刻起作用**；一旦线程主动让出或系统调用返回时真的发生重调度，`proc_sched()` 是按槽位顺序选人的。这是当前实现的一个不对称之处，不是设计文档里写的"纯优先级调度"。

## 十、`proc_sched()` 的循环结构

`usermode.c` 中 `proc_sched()`：

```
for (;;) {
    kthread_run();            /* 先把内核线程（softirq/aio）跑完 */
    遍历 16 个 TCB（下标顺序），第一个可运行的就切过去并 resume_user()
    若当前线程仍可运行 → 恢复自己
    kthread_run(); 若仍无可运行者 → 打印 "[no runnable process]" 并 halt
}
```

内核线程在这里有优先权：**每轮先把 `kthread_run()`（FIFO 取队头）跑空**，才轮到用户线程。`kthread_poll()` 只取 `rq_head`，是纯 FIFO，不参与优先级比较。

## 十一、参数总表（可调点）

| 宏 | 值 | 作用 |
|---|---|---|
| `NPRIO` | 20 | 优先级级数 |
| `PRI_BOOST_IO` | 6 | 睡醒 / I/O 唤醒加成 |
| `PRI_BOOST_STARVE` | 8 | 反饥饿最大加成 |
| `STARVE_THRESHOLD` | 40 | 等待多少 tick 后触发反饥饿（`t_starve` 上限 80） |
| `CPU_BOUND_THRESHOLD` | 3 | 连续 3 个满量子 → 判定 CPU 密集型 |
| `QUANTUM_BASE / CPU / IO` | 4 / 8 / 2 | 时间片 |
| `ESTCPU_HZ` | 100 | 每 100 tick（1 秒）衰减一次 estcpu |
| `ESTCPU_MAX` | 255 | estcpu 上限 |
| `KTHREAD_NO_PREEMPT` | 5 | `t_base_ipl` 达到此值即退出抢占 |

estcpu 衰减公式（`usermode.c:2504`）：`estcpu = estcpu * 2L/(2L+1) + nice`，其中 `L = max(1, 就绪线程数)`。忙时衰减慢、闲时衰减快（L=1 时约 ×2/3）。

## 十二、用户态接口与验证手段

| 接口 | 说明 |
|---|---|
| 系统调用 **446** `sched_setprio(prio)` | 0..19，越界返回 `EINVAL`，设完置 `need_resched` |
| 系统调用 **447** `sched_getprio()` | 返回当前线程的**有效**优先级 |
| `libexec/schedtest` | 用户态自测：setprio/getprio 往返 + join 继承（worker 观察到的优先级应 ≥ joiner 的 18） |
| monitor `schedtest` | 内核态自测 `sched_prio_selftest()`：20 级比较 + 继承 + 抢占门嵌套计数，打印 `schedtest: 20-level prio + inheritance + preempt-gate PASS/FAIL` |
| monitor `kthread` | `kthread_print()`：切换次数、每个 TCB 的 tid/类型/名字/状态/栈指针 |
| monitor `clock` / `lat` | tick 计数、PIT 频率、quantum、切换次数 |
| `ps(1)` | 读 `/dev/ps`，只输出 `PID PPID PGRP STAT COMM`，**不显示优先级** |

## 十三、代码阅读发现的三处行为特征（未做运行时验证）

1. **`QUANTUM_IO = 2` 实际取不到**：`sched_on_wake()` 用 `t_sleep_ticks > 2` 判断"长睡眠"，但 `t_sleep_ticks` 只在**唤醒那一刻**被 `++` 一次（`proc_wake` / `kthread_wake`），睡眠期间并不累计，所以唤醒时它恒为 1，永远走不到 `QUANTUM_IO` 分支。
2. **`t_sleep_ticks` 唤醒后不清零**：`sched_on_wake()` 不重置它（只在 `sched_prio_init()` 里清零），因此一个线程睡过一次之后，`sched_effective_prio()` 里的 `+6` I/O 加成就**长期挂着**。基准 17 和 14 的线程都会因此饱和到 19（clamp），彼此区分度被抹平。
3. **优先级不参与协作式选择**：见第九节。若要做成严格的优先级调度，`proc_sched()` 里也应当调 `fair_pick_next()` 而不是按下标取第一个。

## 十四、一句话总结

**20 级、数值越大越优先的抢占式调度器**：每 100 Hz 的 IRQ0 在用户态检查一次，严格更高优先级立即抢占、同优先级等量子（默认 4 tick / CPU 密集 8 tick）耗尽轮转；有效优先级 = 基准 + I/O 加成 6 + 反饥饿 8 −（estcpu/32 + nice 惩罚，封顶 8），并受 join 场景的优先级继承（可链式传递）覆盖；内核临界区与 `base_ipl >= 5` 的线程通过 `need_resched` 把抢占推迟到系统调用返回的安全点。
