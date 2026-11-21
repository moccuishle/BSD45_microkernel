/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * kthread.c - kernel thread pool plus user-thread slots (S21/S25).
 *
 * Kernel threads have their own 8K stacks and run through kswitch(); user
 * threads share the process address space and are resumed with resume_user()
 * by the scheduler in usermode.c.  Both live in one table so the scheduler
 * only ever walks threads, never processes.
 */
#include <sys/param.h>
#include <sys/systm.h>

#include <sys/proc.h>		/* struct proc: owning kernel process object */
#include "thread/kthread.h"
#include "sys/platform.h"

#ifndef PAGE_SIZE
#define	PAGE_SIZE	4096
#endif
#ifndef VM_PROT_READ
#define	VM_PROT_READ	0x1
#define	VM_PROT_WRITE	0x2
#endif

extern void pmap_map(vm_offset_t, vm_offset_t, vm_offset_t, int);
extern unsigned vfs_page_alloc(void);
extern void bzero(void *, unsigned);

/*
 * The TCB pool lives in a kernel VA window rather than .bss: the boot loader
 * stages the kernel image at 0x10000 and stage 2 runs at 0x90000, so .bss
 * has to stay below 0x90000.  A window (backed by pages reserved from the
 * bump allocator) removes that constraint: capacity becomes a memory
 * question, and kthreads[i] / &kthreads[i] / (me - kthreads) keep working
 * exactly as before - it is still one contiguous array of NKTHREAD TCBs.
 * TCB 池放在内核 VA 窗口而不是 .bss：引导器把镜像暂存在 0x10000、stage2 在
 * 0x90000 运行，因此 .bss 必须留在 0x90000 以下。窗口（物理页由 bump 分配器
 * 预留）解除这个限制，容量只受内存约束，下标语义不变——它仍是一段连续的
 * NKTHREAD 个 TCB。
 *
 * S410: 1024 TCBs at 0xFEF00000, i.e. between the process table (0xFED00000
 * + 1.77 MB) and the stack window (0xFF000000).  The old 0xFE3C0000 slot was
 * sized for 256 TCBs inside the gap under CD_IMAGE_VA.
 */
#define	KTHREAD_TABLE_VA	0xFEF00000	/* after procs (0xFED00000+1.77MB),
						 * below the stack window 0xFF000000 */
struct kthread	*kthreads;

extern void *kern_table_alloc(unsigned va, unsigned size);

void
kthread_table_init(void)
{

	if (kthreads != 0)
		return;
	kthreads = (struct kthread *)kern_table_alloc((unsigned)KTHREAD_TABLE_VA,
	    NKTHREAD * (unsigned)sizeof(struct kthread));
}

/*
 * Build the page tables for every 4 MB window of the stack pool.
 *
 * The stacks are filled lazily, so without this the PDE for the second half
 * of the pool (0xFF400000, slot 512 onwards) would not exist until the 513th
 * thread appeared.  User page directories copy the kernel PDEs when they are
 * created - at boot - so every process started before that point would fault
 * on its first high-slot thread.  Priming maps one page in each window and
 * deliberately leaks it (8 KB): the slot that lands there re-maps the VA with
 * a fresh page on first use.
 * 为栈池的每个 4MB 窗口建好页表。栈是按需映射的，不预建的话 0xFF400000
 * （第 512 个槽起）的 PDE 要等到第 513 个线程才出现；而用户页目录在创建
 * （开机早期）时就拷走了内核 PDE，那些进程一用到高槽位线程就缺页。预建做法
 * 是在每个窗口映射一页，并故意泄漏这 8KB——落在该地址的槽位首次使用时会用
 * 新页重新映射同一 VA。
 */
void
kthread_prime_stacks(void)
{
	unsigned va, end = (unsigned)KSTACK_VA + NKTHREAD * KSTACK_SIZE;

	/*
	 * kern_table_alloc() opens the 4 MB window for the address it is
	 * given, which is the whole point here: the PDE must exist in
	 * kernel_pmap before user page directories copy it.
	 * kern_table_alloc 会为传入地址建 4MB 窗口页表，这正是本函数的目的：
	 * 用户页目录拷贝内核 PDE 之前，它必须已经存在于 kernel_pmap 里。
	 */
	for (va = (unsigned)KSTACK_VA; va < end; va += 4 * 1024 * 1024)
		(void)kern_table_alloc(va, PAGE_SIZE);
}
static unsigned	sched_esp;
struct kthread	*curk;
unsigned	kthread_switches;

/*
 * Explicit run queue of TCBs.  The scheduler only ever looks at this queue;
 * a struct proc is a resource container, never a scheduling entity.  A thread
 * is enqueued when it becomes runnable and dequeued when it blocks/exits.
 */
static struct kthread *rq_head, *rq_tail;

/* EN: Enqueue a TCB at the tail of the run queue (idempotent via t_onrq). */
static void
rq_enqueue(struct kthread *t)
{

	if (t == 0 || t->t_onrq)
		return;
	t->t_rqnext = 0;
	t->t_onrq = 1;
	if (rq_tail != 0)
		rq_tail->t_rqnext = t;
	else
		rq_head = t;
	rq_tail = t;
	if (t->t_proc != 0)
		t->t_proc->uk_rq_threads++;
}

/* EN: Remove a TCB from the run queue by walking with a predecessor pointer. */
static void
rq_dequeue(struct kthread *t)
{
	struct kthread *p, *prev = 0;

	if (t == 0 || !t->t_onrq)
		return;
	for (p = rq_head; p != 0; prev = p, p = p->t_rqnext) {
		if (p == t) {
			if (prev != 0)
				prev->t_rqnext = t->t_rqnext;
			else
				rq_head = t->t_rqnext;
			if (rq_tail == t)
				rq_tail = prev;
			break;
		}
	}
	t->t_rqnext = 0;
	t->t_onrq = 0;
	if (t->t_proc != 0 && t->t_proc->uk_rq_threads > 0)
		t->t_proc->uk_rq_threads--;
}

/* 中文：内核线程入口跳板，调用线程的实际入口函数并在返回后退出线程 */
/* EN: Kernel thread trampoline that calls the thread's real entry function and exits on return */
static void
kthread_trampoline(void)
{
	struct kthread *t = curk;

	if (t != 0 && t->t_entry != 0)
		t->t_entry(t->t_arg);
	kthread_exit();
}

/*
 * Kernel stacks are per TCB slot and mapped exactly once.
 *
 * Each slot owns a fixed 8 KB window at KSTACK_VA + i*KSTACK_SIZE, so a slot
 * that is handed out again does not need new physical pages: the previous
 * thread's frames are simply wiped.  Before this, kstack_alloc() called
 * vfs_page_alloc() on *every* spawn and nothing ever returned those pages -
 * a fork/exit cycle leaked 8 KB each time (200 sequential forks = 1.6 MB,
 * and page tables/TCBs were recycled fine, so only the idle page counter
 * revealed it).  Caching bounds kernel-stack memory at NKTHREAD*8 KB = 2 MB
 * and makes churn allocation-free after the first pass.
 *
 * The pages are deliberately not unmapped on thread exit: a user thread
 * frees its own TCB while still running on that very stack (kthread_exit /
 * thread_exit_current), so returning the pages there would free memory in
 * active use.  Deferred reclamation would buy back at most 2 MB on a
 * multi-hundred-MB guest - not worth a use-after-free risk.
 * 内核栈按 TCB 槽位分配一次即固定复用：每个槽位独占 KSTACK_VA + i*8KB 这段
 * 8KB 窗口，重新分配时只要把上一个线程留下的栈帧清零，无需再取物理页。此前
 * 每次 spawn 都调 vfs_page_alloc() 且从不归还，一个 fork/exit 周期泄漏 8KB
 * （200 次顺序 fork = 1.6MB；页表与 TCB 都正常回收，只有空闲页计数能暴露）。
 * 缓存后内核栈上限固定为 NKTHREAD*8KB = 2MB，抖动期零分配。
 * 故意不在线程退出时解除映射：用户线程释放自己的 TCB 时仍运行在该栈上
 * （kthread_exit / thread_exit_current），此时归还页面等于释放正在使用的内存。
 * 延迟回收最多只能拿回 2MB，不值得冒 use-after-free 的风险。
 */
static unsigned char	kstack_mapped[NKTHREAD];

/*
 * Map (or reuse) the kernel stack for TCB slot i.  Returns 0, or -1 for a
 * slot index outside the pool.
 * 为第 i 个槽位映射（或复用）内核栈；槽位越界返回 -1。
 */
static int
kstack_alloc(int i)
{
	unsigned char *va;
	unsigned pa;
	int j;

	if (i < 0 || i >= NKTHREAD)
		return (-1);
	va = (unsigned char *)(KSTACK_VA + i * KSTACK_SIZE);
	if (kstack_mapped[i]) {
		/* Reused slot: drop the previous thread's frames. */
		bzero(va, KSTACK_SIZE);
		return (0);
	}
	for (j = 0; j < KSTACK_SIZE; j += PAGE_SIZE) {
		pa = vfs_page_alloc();
		pmap_map((vm_offset_t)(va + j), (vm_offset_t)pa,
		    (vm_offset_t)pa + PAGE_SIZE,
		    VM_PROT_READ | VM_PROT_WRITE);
	}
	kstack_mapped[i] = 1;
	return (0);
}

/* 中文：初始化内核线程池，清空所有线程槽位为 FREE 状态 */
/* EN: Initialize the kernel thread pool, marking all thread slots FREE */
void
kthread_init(void)
{
	int i;

	if (kthreads == 0)
		kthread_table_init();
	bzero(kthreads, NKTHREAD * sizeof(struct kthread));
	bzero(kstack_mapped, sizeof(kstack_mapped));	/* no stack mapped yet */
	curk = 0;
	sched_esp = 0;
	kthread_switches = 0;
	rq_head = rq_tail = 0;
	for (i = 0; i < NKTHREAD; i++) {
		kthreads[i].t_state = KT_FREE;
		kthreads[i].t_proc = 0;
		kthreads[i].t_used = 0;
	}
	printf("kthread: %d thread slots at %x\n", NKTHREAD, KSTACK_VA);
}

/* 中文：创建一个内核线程，分配槽位、设置入口和栈并初始化切换上下文 */
/* EN: Spawn a kernel thread: grab a free slot, set its entry/stack and initialize the switch context */
struct kthread *
kthread_spawn(const char *name, void (*entry)(void *), void *arg)
{
	struct kthread *t = 0;
	unsigned *sp;
	int i;

	for (i = 0; i < NKTHREAD; i++)
		if (!kthreads[i].t_used) {
			t = &kthreads[i];
			break;
		}
	if (t == 0)
		return (0);
	bzero(t, sizeof(*t));
	t->t_state = KT_RUN;
	t->t_tid = TID_KERN_BASE + i;	/* high range: never collides with pid/tid */
	t->t_name = name;
	t->t_entry = entry;
	t->t_arg = arg;
	t->t_used = 1;
	t->t_proc = 0;
	t->t_base_ipl = 0;
	t->t_eff_ipl = 0;
	t->t_preemptable = 1;
	kstack_alloc(i);
	t->t_stack_va = (void *)(KSTACK_VA + i * KSTACK_SIZE);
	t->t_stack_top = (unsigned)t->t_stack_va + KSTACK_SIZE;
	t->t_stack_top &= ~15u;

	sp = (unsigned *)t->t_stack_top;
	*--sp = (unsigned)kthread_trampoline;	/* kswitch ret target */
	*--sp = 0;				/* ebp */
	*--sp = 0;				/* ebx */
	*--sp = 0;				/* esi */
	*--sp = 0;				/* edi */
	t->t_ksp = (unsigned)sp;
	t->t_lastcpu = 0;		/* no affinity yet */
	sched_prio_init(t, PRI_NORMAL);
	rq_enqueue(t);			/* runnable TCB enters the run queue */
	printf("kthread: spawn %s tid %d\n", name, t->t_tid);
	return (t);
}

/* 中文：为一个用户进程创建用户线程槽位，分配独立内核栈并绑定进程与 tid */
/* EN: Create a user-thread slot for a process, allocating a private kernel stack and binding proc/tid */
struct kthread *
kthread_user_spawn(struct proc *p, int tid)
{
	struct kthread *t = 0;
	int i;

	for (i = 0; i < NKTHREAD; i++)
		if (!kthreads[i].t_used) {
			t = &kthreads[i];
			break;
		}
	if (t == 0)
		return (0);
	bzero(t, sizeof(*t));
	t->t_state = KT_RUN;
	t->t_tid = tid;
	t->t_name = "user";
	t->t_used = 1;
	t->t_proc = p;
	t->t_base_ipl = 0;
	t->t_eff_ipl = 0;
	t->t_preemptable = 1;
	sched_prio_init(t, PRI_NORMAL);
	/* KLT: link this thread into its owning process's thread list */
	if (p != 0) {
		struct kthread **pp = &p->uk_p_threads;
		while (*pp != 0)
			pp = &(*pp)->t_next;
		*pp = t;
		t->t_next = 0;
		p->uk_p_nthreads++;
	}
	/*
	 * Every user thread gets its own 8 KB kernel stack so a ring-3
	 * interrupt or syscall for one thread can never overwrite another
	 * thread's kernel frames.
	 * 每个用户线程拥有独立的 8 KB 内核栈，某个线程的 ring3 中断或系统调用
	 * 不会覆盖其它线程的内核栈帧。
	 */
	kstack_alloc(i);
	t->t_stack_va = (void *)(KSTACK_VA + i * KSTACK_SIZE);
	t->t_stack_top = (unsigned)t->t_stack_va + KSTACK_SIZE;
	t->t_stack_top &= ~15u;
	return (t);
}

/* 中文：按 tid 查找属于用户进程的用户线程 */
/* EN: Find a user thread belonging to a process by its tid */
struct kthread *
kthread_user_find(int tid)
{
	int i;

	for (i = 0; i < NKTHREAD; i++)
		if (kthreads[i].t_used && kthreads[i].t_proc != 0 &&
		    kthreads[i].t_tid == tid)
			return (&kthreads[i]);
	return (0);
}

/* 中文：释放一个用户线程槽位，标记为空闲并解除进程绑定 */
/* EN: Free a user-thread slot, marking it FREE and detaching its process */
void
kthread_user_free(struct kthread *t)
{
	struct kthread **pp;

	if (t == 0)
		return;
	/* KLT: unlink from the owning process's thread list */
	if (t->t_proc != 0) {
		for (pp = &t->t_proc->uk_p_threads; *pp != 0;
		     pp = &(*pp)->t_next) {
			if (*pp == t) {
				*pp = t->t_next;
				break;
			}
		}
		if (t->t_proc->uk_p_nthreads > 0)
			t->t_proc->uk_p_nthreads--;
		if (t->t_proc->uk_p_thread == t)
			t->t_proc->uk_p_thread = t->t_next;
	}
	t->t_state = KT_FREE;
	t->t_proc = 0;
	t->t_next = 0;
	t->t_used = 0;
}

/* 中文：阻塞当前内核线程，置为 KT_BLOCK 并切换回调度栈 */
/* EN: Block the current kernel thread, marking it KT_BLOCK and switching back to the scheduler stack */
void
kthread_block(void)
{
	struct kthread *t = curk;

	if (t == 0)
		return;
	rq_dequeue(t);			/* blocked TCB leaves the run queue */
	sched_on_block(t);		/* start sleep-length accounting */
	t->t_state = KT_BLOCK;
	hal_switch_stack(&t->t_ksp, sched_esp);
}

/* 中文：唤醒处于阻塞状态的线程，将其状态改回 KT_RUN */
/* EN: Wake a blocked thread, setting its state back to KT_RUN */
void
kthread_wake(struct kthread *t)
{

	if (t != 0 && t->t_state == KT_BLOCK) {
		t->t_sleep_ticks++;	/* it really slept a tick */
		sched_on_wake(t);	/* I/O/interactive boost + quantum class */
		t->t_state = KT_RUN;
		rq_enqueue(t);		/* affinity: re-enter at tail for now */
	}
}

/* 中文：终止当前内核线程，释放槽位并切换回调度栈（不再返回） */
/* EN: Terminate the current kernel thread, freeing its slot and switching to the scheduler stack (never returns) */
void
kthread_exit(void)
{
	struct kthread *t = curk;

	if (t == 0)
		return;
	rq_dequeue(t);			/* exiting TCB leaves the run queue */
	t->t_state = KT_FREE;
	t->t_used = 0;
	hal_switch_stack(&t->t_ksp, sched_esp);
	for (;;)
		;
}

/* 中文：检查是否存在可运行的内核线程（已使用、非用户线程且状态为 KT_RUN） */
/* EN: Check whether any kernel thread (used, non-user, KT_RUN) is runnable */
int
kthread_runnable(void)
{

	return (rq_head != 0);
}

/*
 * Pick the runnable TCB at the head of the run queue and switch to it.
 * This is the kernel-thread half of the unified scheduler: the run queue
 * holds TCBs, never struct proc.  Affinity is honoured by re-enqueuing a
 * woken thread on the CPU it last ran on (t_lastcpu) when SMP arrives.
 */
void
kthread_poll(void)
{
	struct kthread *t;

	t = rq_head;
	if (t == 0)
		return;
	rq_dequeue(t);			/* running: off the queue */
	curk = t;
	t->t_lastcpu = 0;		/* single CPU in this port */
	kthread_switches++;
	hal_switch_stack(&sched_esp, t->t_ksp);
	curk = 0;
}

/* 中文：运行所有可运行的内核线程，直到没有可运行者为止 */
/* EN: Run all runnable kernel threads until none remain runnable */
void
kthread_run(void)
{

	while (kthread_runnable())
		kthread_poll();
}

/* 中文：调试输出，打印切换次数及所有已用线程的 tid、类型、名称、状态等信息 */
/* EN: Debug output of the switch count and each used thread's tid, type, name and state */
void
kthread_print(void)
{
	int i;

	printf("kthread: switches %d\n", kthread_switches);
	for (i = 0; i < NKTHREAD; i++) {
		struct kthread *t = &kthreads[i];

		if (!t->t_used)
			continue;
		printf("kthread: tid %d %s %s state %d ksp %x\n", t->t_tid,
		    t->t_proc ? "user" : "kern",
		    t->t_name ? t->t_name : "?", t->t_state, t->t_ksp);
	}
}

/* 中文：统计当前已使用的线程槽位数量 */
/* EN: Count the number of currently used thread slots */
int
kthread_count(void)
{
	int i, n = 0;

	for (i = 0; i < NKTHREAD; i++)
		if (kthreads[i].t_used)
			n++;
	return (n);
}

/* ------------------------------------------------- preemption control */

/* EN: Enter a non-preemptable kernel critical section (nestable). */
void
preempt_disable(void)
{

	if (curk != 0)
		curk->t_preempt_cnt++;
	else if (cur_proc_kthread() != 0)	/* user thread running in kernel */
		cur_proc_kthread()->t_preempt_cnt++;
}

/* EN: Leave a critical section.  The actual reschedule is deferred to the
 * next safe point (syscall exit / irq return to user mode). */
void
preempt_enable(void)
{
	struct kthread *t = (curk != 0) ? curk : cur_proc_kthread();

	if (t == 0)
		return;
	if (t->t_preempt_cnt > 0)
		t->t_preempt_cnt--;
	/* need_resched is consumed only at a safe point, never here */
}

/* EN: Is the current thread in a kernel critical section? */
int
preempt_is_disabled(void)
{
	struct kthread *t = (curk != 0) ? curk : cur_proc_kthread();

	return (t != 0 && t->t_preempt_cnt > 0);
}

/* ------------------------------------------------- dynamic priority */

/* EN: Initialize a thread's base/effective priority and default quantum. */
void
sched_prio_init(struct kthread *t, int base)
{

	if (t == 0)
		return;
	if (base < 0)
		base = PRI_NORMAL;
	if (base >= NPRIO)
		base = PRI_HIGHEST;
	t->t_prio = base;
	t->t_curprio = base;
	t->t_base_prio = base;
	t->t_inherit_prio = 0;
	t->t_blocked_on = -1;
	t->t_quantum = QUANTUM_BASE;
	t->t_ticks = 0;
	t->t_sleep_ticks = 0;
	t->t_starve = 0;
	t->t_cpu_burst = 0;
}

/*
 * EN: Effective priority, 0 lowest .. 19 highest.  Higher is better.
 * Boosts (I/O wake, starvation, inheritance) ADD to the priority; the
 * result is clamped to [0, PRI_HIGHEST].
 */
int
sched_effective_prio(struct kthread *t)
{
	int p;

	if (t == 0)
		return (PRI_LOWEST);
	p = t->t_prio;
	if (t->t_sleep_ticks > 0 && t->t_state == KT_RUN)
		p += PRI_BOOST_IO;		/* woken I/O thread: interactive */
	if (t->t_starve >= STARVE_THRESHOLD)
		p += PRI_BOOST_STARVE;		/* anti-starvation */
	/* BSD interaction score: high estcpu (CPU hog) and high nice lower
	 * the effective priority; interactive (low estcpu) tasks stay high. */
	p -= sched_interaction_penalty(t);
	/* priority inheritance dominates: run at the donor's level */
	if (t->t_inherit_prio > 0 && t->t_inherit_prio > p)
		p = t->t_inherit_prio;
	if (p < PRI_LOWEST)
		p = PRI_LOWEST;
	if (p > PRI_HIGHEST)
		p = PRI_HIGHEST;
	return (p);
}

/*
 * EN: Priority inheritance.  A high-priority WAITER blocks on a lock held
 * by HOLDER; temporarily raise the holder to the waiter's priority so it
 * finishes quickly and releases the resource.  Chain-safe: if the holder
 * is itself blocked on a third thread, propagate the donation further.
 */
void
prio_inherit(struct kthread *waiter, struct kthread *holder)
{
	int donated;

	if (waiter == 0 || holder == 0 || waiter == holder)
		return;
	donated = sched_effective_prio(waiter);
	if (donated <= sched_effective_prio(holder)) {
		holder->t_blocked_on = -1;
		return;
	}
	/* remember the original priority once (before the first donation) */
	if (holder->t_inherit_prio == 0)
		holder->t_base_prio = holder->t_prio;
	if (donated > holder->t_inherit_prio)
		holder->t_inherit_prio = donated;
	ipl_donations++;
	/* transitive inheritance: holder waits on another holder */
	if (holder->t_blocked_on >= 0) {
		struct kthread *next = kthread_find(holder->t_blocked_on);

		if (next != 0 && next != holder)
			prio_inherit(holder, next);
	}
}

/* EN: Restore a lock holder's original priority after it releases the
 * resource.  Releases the whole inherited chain as needed. */
void
prio_restore(struct kthread *holder)
{

	if (holder == 0)
		return;
	holder->t_inherit_prio = 0;
	holder->t_prio = holder->t_base_prio;
	holder->t_blocked_on = -1;
}

/* EN: Called when a thread wakes from sleep/I/O: it is likely interactive. */
void
sched_on_wake(struct kthread *t)
{

	if (t == 0)
		return;
	/* slept a long time => interactive; reward with short quantum + boost */
	if (t->t_sleep_ticks > 2) {
		t->t_quantum = QUANTUM_IO;
	} else if (t->t_cpu_burst >= CPU_BOUND_THRESHOLD) {
		/* CPU-bound: longer quantum to amortise switch overhead */
		t->t_quantum = QUANTUM_CPU;
	} else {
		t->t_quantum = QUANTUM_BASE;
	}
	t->t_cpu_burst = 0;
	t->t_ticks = 0;
	t->t_starve = 0;
}

/* EN: Called when a thread blocks: start counting sleep for classification. */
void
sched_on_block(struct kthread *t)
{

	if (t == 0)
		return;
	t->t_sleep_ticks = 1;
}

/* EN: Called each tick the thread is running: detect CPU-bound workloads. */
void
sched_on_tick(struct kthread *t)
{

	if (t == 0)
		return;
	t->t_ticks++;
	if (t->t_ticks >= t->t_quantum) {
		t->t_cpu_burst++;
		if (t->t_cpu_burst > CPU_BOUND_THRESHOLD)
			t->t_cpu_burst = CPU_BOUND_THRESHOLD;
	}
}

/* EN: A thread is CPU-bound if it consumed several full quanta without sleeping. */
int
sched_is_cpu_bound(struct kthread *t)
{

	return (t != 0 && t->t_cpu_burst >= CPU_BOUND_THRESHOLD);
}

/* ------------------------------------------------- os.md mechanism 4 */

unsigned	ipl_boosts;
unsigned	ipl_donations;

/* 中文：设置线程的基准 IPL（限制在 0..15）与可抢占标志，未被提升时同步更新有效 IPL */
/* EN: Set a thread's base IPL (clamped 0..15) and preemptable flag, updating effective IPL when not boosted */
void
thread_ipl_set(struct kthread *t, int base_ipl, int preemptable)
{

	if (t == 0)
		return;
	if (base_ipl < 0)
		base_ipl = 0;
	if (base_ipl > 15)
		base_ipl = 15;
	t->t_base_ipl = base_ipl;
	if (!t->t_boosted)
		t->t_eff_ipl = base_ipl;
	t->t_preemptable = preemptable ? 1 : 0;
}

/* 中文：返回线程的有效 IPL，线程为空时返回 0 */
/* EN: Return the thread's effective IPL, or 0 when the thread is NULL */
int
thread_ipl_effective(struct kthread *t)
{

	return (t != 0 ? t->t_eff_ipl : 0);
}

/*
 * Priority inheritance: a thread that holds a resource a higher-IPL thread
 * wants runs at the requester's level until it releases the resource.
 * This is the os.md rule for non-preemptable threads (preemptable ones can
 * be suspended instead; this port records that policy in t_preemptable).
 */
/* 中文：优先级继承：把线程有效 IPL 提升到 ipl（仅升不降），并标记已提升、计数 */
/* EN: Priority inheritance: raise a thread's effective IPL to ipl (never lower), mark boosted and count it */
int
thread_ipl_boost(struct kthread *t, int ipl)
{

	if (t == 0 || ipl <= t->t_eff_ipl)
		return (t != 0 ? t->t_eff_ipl : 0);
	t->t_eff_ipl = ipl;
	t->t_boosted = 1;
	ipl_boosts++;
	return (t->t_eff_ipl);
}

/* 中文：取消线程的 IPL 提升，把有效 IPL 恢复为基准 IPL */
/* EN: Remove a thread's IPL boost, restoring its effective IPL to the base IPL */
void
thread_ipl_unboost(struct kthread *t)
{

	if (t == 0 || !t->t_boosted)
		return;
	t->t_boosted = 0;
	t->t_eff_ipl = t->t_base_ipl;
}

/* 中文：按 tid 在线程池中查找任意（内核或用户）线程 */
/* EN: Find any thread (kernel or user) in the pool by its tid */
struct kthread *
kthread_find(int tid)
{
	int i;

	for (i = 0; i < NKTHREAD; i++)
		if (kthreads[i].t_used && kthreads[i].t_tid == tid)
			return (&kthreads[i]);
	return (0);
}

/* 中文：自检线程的 IPL 设置与优先级继承：验证设置、提升、不可降级和取消提升 */
/* EN: Self-test thread IPL setting and priority inheritance: verify set, boost, no-downgrade and unboost */
int
kthread_ipl_selftest(void)
{
	struct kthread t;
	int ok = 1;

	bzero(&t, sizeof(t));
	thread_ipl_set(&t, 2, 1);
	if (t.t_base_ipl != 2 || thread_ipl_effective(&t) != 2)
		ok = 0;
	if (thread_ipl_boost(&t, 14) != 14)
		ok = 0;
	if (!t.t_boosted || thread_ipl_effective(&t) != 14)
		ok = 0;
	if (thread_ipl_boost(&t, 9) != 14)	/* no downgrade */
		ok = 0;
	thread_ipl_unboost(&t);
	if (t.t_boosted || thread_ipl_effective(&t) != 2)
		ok = 0;
	thread_ipl_set(&t, 5, 0);
	if (t.t_preemptable != 0 || t.t_base_ipl != 5)
		ok = 0;
	printf("ipltest: thread ipl/inheritance %s\n", ok ? "ok" : "FAIL");
	return (ok ? 0 : -1);
}

/* ---------------------------------------------------------------- test */

/* EN: Self-test of the 20-level priority model and priority inheritance.
 * Uses local TCBs so it does not depend on userland thread timing. */
int
sched_prio_selftest(void)
{
	struct kthread waiter, holder;
	int ok = 1;

	bzero(&waiter, sizeof(waiter));
	bzero(&holder, sizeof(holder));
	sched_prio_init(&waiter, PRI_HIGH);	/* 17: high-priority waiter */
	sched_prio_init(&holder, PRI_BATCH);	/* 5: low-priority holder */
	waiter.t_tid = 90;
	holder.t_tid = 91;
	waiter.t_state = holder.t_state = KT_RUN;

	/* higher must outrank lower */
	if (sched_effective_prio(&waiter) <= sched_effective_prio(&holder))
		ok = 0;
	/* donate: holder should jump to waiter's level */
	prio_inherit(&waiter, &holder);
	if (holder.t_inherit_prio < sched_effective_prio(&waiter))
		ok = 0;
	if (sched_effective_prio(&holder) < sched_effective_prio(&waiter))
		ok = 0;
	/* restore: holder returns to its original priority */
	prio_restore(&holder);
	if (sched_effective_prio(&holder) != PRI_BATCH || holder.t_inherit_prio != 0)
		ok = 0;
	/* preemption control: user preemptable, kernel critical section not */
	{
		struct kthread *tmp = curk;
		curk = &waiter;
		waiter.t_preempt_cnt = 0;
		if (preempt_is_disabled())
			ok = 0;		/* idle thread: preemptable */
		preempt_disable();
		preempt_disable();	/* nest */
		if (!preempt_is_disabled() || waiter.t_preempt_cnt != 2)
			ok = 0;
		preempt_enable();
		if (!preempt_is_disabled() || waiter.t_preempt_cnt != 1)
			ok = 0;
		preempt_enable();
		if (preempt_is_disabled() || waiter.t_preempt_cnt != 0)
			ok = 0;
		curk = tmp;
	}
	ipl_donations = 0;
	printf("schedtest: 20-level prio + inheritance + preempt-gate %s\n",
	    ok ? "PASS" : "FAIL");
	return (ok ? 0 : -1);
}

static int ktest_counter;

/* 中文：自测用的线程入口，循环三次递增计数器并主动阻塞让出 */
/* EN: Test thread entry that increments a counter three times and blocks to yield */
static void
ktest_entry(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < 3; i++) {
		ktest_counter++;
		kthread_block();
	}
}

/* 中文：自检线程池：派生测试线程、反复唤醒并调度直至其结束，返回计数值 */
/* EN: Self-test of the thread pool: spawn a test thread, repeatedly wake and schedule it, returning its count */
unsigned
kthread_selftest(void)
{
	struct kthread *t;

	ktest_counter = 0;
	t = kthread_spawn("ktest", ktest_entry, 0);
	if (t == 0)
		return (0);
	while (t->t_state != KT_FREE) {
		kthread_wake(t);
		kthread_poll();
	}
	return (unsigned)ktest_counter;
}
