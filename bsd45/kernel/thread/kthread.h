/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 */

/*
 * kthread.h - kernel-only threads (S21).
 *
 * A kernel thread is an execution carrier that never enters user mode.  It
 * has its own kernel stack and runs the scheduler's cooperative context
 * switch.  Softirq processing and page reclaim use these threads, so those
 * jobs no longer run on whatever user thread happened to call into the
 * kernel.
 * 内核线程是永不进入用户态的执行载体：它有自己的内核栈，并走调度器的
 * 协作式上下文切换；软中断与页回收在这些线程上运行，不再借用用户线程的栈。
 */
#ifndef _KTHREAD_H_
#define _KTHREAD_H_

#include "sys/platform.h"

/*
 * KLT pool (S407): NKTHREAD is the TCB table capacity.  Each slot owns an
 * 8 KB kernel stack mapped at KSTACK_VA in the shared kernel PDE 0x3fa,
 * which covers 0xFE800000-0xFEBFFFFF = 2 MB, i.e. 256 stacks.  Going above
 * 256 would spill into PDE 0x3fb and needs its page table built first.
 * The table itself lives in the KTHREAD_TABLE_VA window, so it costs no
 * image bytes and capacity is a RAM/VA budget question only - the 1:1
 * kernel-thread model is unchanged.
 * 实际存活上限由物理内存经 page_alloc 决定，内存耗尽时 fork/spawn 返回
 * -EAGAIN 而非 panic。1:1 内核线程模型不变。
 */
#define	NKTHREAD	256		/* TCB slots = 2 MB / 8 KB stack window */

/*
 * TID namespace partition (pid may reach NPROC now):
 *   main thread    tid == pid                     (1 .. NPROC+1)
 *   extra user tid = TID_USER_BASE + pid*TID_STRIDE + n
 *   kernel thread  tid = TID_KERN_BASE + slot      (high, never a pid)
 * kthread_find()/kthread_user_find() key on tid, so ranges must not overlap.
 */
#define	TID_USER_BASE	1000
#define	TID_STRIDE	16
#define	TID_KERN_BASE	0x40000000

#define	KT_FREE		0
#define	KT_RUN		1
#define	KT_BLOCK	2
#define	KT_ZOMBIE	3

/* priority scale: 0 = LOWEST, 19 = HIGHEST (20 levels) */
#define	NPRIO		20
#define	PRI_LOWEST	0
#define	PRI_IDLE	2
#define	PRI_BATCH	5		/* CPU-bound baseline (low) */
#define	PRI_NORMAL	10
#define	PRI_INTERACTIVE	14	/* interactive / I/O-bound baseline */
#define	PRI_HIGH	17
#define	PRI_HIGHEST	19

/* quantum (ticks) -- fixed base; CPU-bound threads get a longer one */
#define	QUANTUM_BASE	4
#define	QUANTUM_CPU	8
#define	QUANTUM_IO	2

/* dynamic priority tuning */
#define	PRI_BOOST_IO	6	/* boost on wake from sleep/I/O */
#define	PRI_BOOST_STARVE	8	/* max anti-starvation boost */
#define	STARVE_THRESHOLD	40	/* ticks before anti-starvation kicks in */
#define	CPU_BOUND_THRESHOLD	3	/* consecutive CPU quanta => CPU-bound */

#define	KSTACK_VA	0xFEA00000
#define	KSTACK_SIZE	8192

struct proc;

struct kthread {
	int		t_state;
	int		t_tid;
	const char	*t_name;
	void		(*t_entry)(void *);	/* kernel threads only */
	void		*t_arg;
	unsigned	t_ksp;		/* saved kernel esp while switched out */
	unsigned	t_stack_top;
	void		*t_stack_va;
	int		t_used;
	/* KLT: threads belonging to a process form a list via t_next;
	 * t_proc points back to the owning kernel process object. */
	struct proc	*t_proc;
	struct kthread	*t_next;	/* next thread in proc->uk_p_threads list */
	struct kthread	*t_rqnext;	/* next TCB in the scheduler run queue */
	int		t_onrq;		/* 1 if currently linked on the run queue */
	/* Scheduling: the quantum is per-thread, fairness is per-process. */
	/* ---- scheduling: dynamic priority, quantum, workload class ---- */
	int		t_prio;		/* base priority 0(low)..19(high) */
	int		t_curprio;	/* effective priority (dynamic boosts/decay) */
	/* priority inheritance: a low-prio holder temporarily runs at a
	 * high-prio waiter's level, then restores its original priority. */
	int		t_base_prio;	/* original priority before inheritance */
	int		t_inherit_prio;	/* donated priority (0 = none) */
	int		t_blocked_on;	/* tid of thread holding the wanted lock, -1 none */
	/* preemption control: >0 means in a kernel critical section; the
	 * thread cannot be preempted until it returns to zero.  User mode is
	 * always preemptable; preemption happens at safe points only. */
	int		t_preempt_cnt;
	int		t_ticks;	/* ticks consumed in current quantum */
	int		t_quantum;	/* quantum for this slice (workload-tuned) */
	int		t_sleep_ticks;	/* ticks slept: I/O-bound detector */
	int		t_starve;	/* ticks waiting: anti-starvation boost */
	int		t_cpu_burst;	/* consecutive CPU ticks: CPU-bound detector */
	int		t_lastcpu;	/* affinity: last CPU this thread ran on */
	void		*t_wchan;	/* non-NULL => blocked on this channel */
	struct kthread	*t_wqnext;	/* S200: next TCB on a sync wait queue */
	struct sysregs	t_regs;
	int		t_wait_pid;
	unsigned	t_wait_status;
	int		t_wait_options;
	int		t_pipeidx;
	int		t_piperw;
	unsigned	t_pipeat;
	unsigned	t_pipelen;
	int		t_exitcode;
	int		t_join_tid;	/* thread waiting in thread_join */
	int		t_detached;
	unsigned	t_sigmask;	/* thread-directed blocked mask */
	unsigned	t_sig;		/* thread-directed pending signals */
	/* os.md mechanism 4: scheduling/IPL properties */
	int		t_base_ipl;	/* preemption sensitivity (0..15) */
	int		t_eff_ipl;	/* base + priority inheritance */
	int		t_preemptable;	/* may its resources be taken over? */
	int		t_boosted;	/* inheriting a higher level */
};

extern struct kthread *curk;
/* TCB pool lives in a VA window, not in .bss (see kthread.c). */
extern struct kthread *kthreads;
void kthread_table_init(void);

void	kthread_init(void);
struct kthread *kthread_spawn(const char *name, void (*entry)(void *),
	    void *arg);
struct kthread *kthread_user_spawn(struct proc *p, int tid);
struct kthread *kthread_user_find(int tid);
void	kthread_user_free(struct kthread *t);
void	kthread_exit(void);
void	kthread_block(void);
void	kthread_wake(struct kthread *t);
void	kthread_poll(void);	/* run one runnable kernel thread */
void	kthread_run(void);	/* run until none are runnable */
void	kthread_print(void);
int	kthread_count(void);
int	kthread_runnable(void);
unsigned kthread_selftest(void);
extern unsigned kthread_switches;

/* context switch: save current esp in *old, continue on new stack */
void	kswitch(unsigned *old, unsigned new);

/* interaction scoring hook provided by the process layer (usermode.c):
 * returns a penalty 0..N derived from BSD estcpu + p_nice. */
int	sched_interaction_penalty(struct kthread *t);

/* dynamic priority / workload scheduling */
void	sched_prio_init(struct kthread *t, int base);
int	sched_effective_prio(struct kthread *t);
void	sched_on_wake(struct kthread *t);	/* boost on I/O/sleep wake */
void	sched_on_block(struct kthread *t);	/* record sleep for classification */
void	sched_on_tick(struct kthread *t);	/* update cpu/starve counters */
int	sched_is_cpu_bound(struct kthread *t);
/* priority inheritance: waiter donates its priority to the lock holder */
void	prio_inherit(struct kthread *waiter, struct kthread *holder);
void	prio_restore(struct kthread *holder);
int	sched_prio_selftest(void);
/* preemption control: kernel critical sections disable preemption */
void	preempt_disable(void);
void	preempt_enable(void);	/* may schedule when count drops to 0 */
int	preempt_is_disabled(void);
/* current user thread's TCB (provided by usermode.c), or 0 if none */
struct kthread *cur_proc_kthread(void);

/* os.md mechanism 4: per-thread IPL policy + priority inheritance */
void	thread_ipl_set(struct kthread *t, int base_ipl, int preemptable);
int	thread_ipl_effective(struct kthread *t);
int	thread_ipl_boost(struct kthread *t, int ipl);
void	thread_ipl_unboost(struct kthread *t);
struct kthread *kthread_find(int tid);
int	kthread_ipl_selftest(void);
extern unsigned ipl_boosts;
extern unsigned ipl_donations;

/* softirq */
#define	SOFT_RECLAIM	0
#define	SOFT_INPUT	1
#define	SOFT_NET	2
#define	SOFT_TEST	3
#define	NSOFT		4

void	softirq_init(void);
void	softint_schedule(int id);
void	softint_register(int id, void (*fn)(void));
void	softirq_print(void);
unsigned softirq_count(void);
int	softirq_selftest(void);

/* page cache reclaim hook (pagecache.c) */
int	pagecache_reclaim(unsigned n);

#endif
