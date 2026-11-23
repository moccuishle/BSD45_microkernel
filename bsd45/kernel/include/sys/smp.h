/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * smp.h - symmetric multiprocessing support for BSD45 i386.
 *
 * Layout:
 *   - CPU 0 is the BSP (boot processor), started by stage1/stage2.
 *   - APs are started via the MP startup protocol: INIT-SIPI-SIPI to the
 *     Local APIC, with a 4K trampoline page in low physical memory.
 *   - per-CPU data is addressed through the GS segment (flat, base = &cpu0).
 *   - cross-CPU coordination uses IPI + a simple spinlock.
 */
#ifndef _SMP_H_
#define _SMP_H_

#define	MAXCPU		8
#define	CPU_TRAMPOLINE_PA	0x7000	/* low phys page for AP trampoline */

#ifndef ASSEMBLER
struct cpu_info {
	int		ci_id;		/* 0 = BSP */
	int		ci_active;	/* AP finished basic init */
	unsigned	ci_apicid;	/* hardware local APIC ID */
	unsigned	ci_lapic;	/* local APIC MMIO base (VA) */
	unsigned	ci_idle_tss;	/* per-CPU TSS selector */
	void		*ci_curthread;	/* currently running TCB on this CPU */
	unsigned	ci_ticks;
};

extern struct cpu_info cpus[MAXCPU];
extern int ncpu;			/* number of active CPUs */

/*
 * S412: topology discovered from CPUID (not CMOS guesses).
 *   logical  = schedulable logical processors (includes SMT siblings)
 *   cores    = physical cores
 *   smt      = logical / cores
 */
extern int cpu_logical, cpu_cores, cpu_smt, cpu_htt;
extern char cpu_vendor[13];
int	cpu_topology_detect(void);
void	cpuinfo_print(void);

/* boot / AP startup */
void	smp_init(void);		/* called by BSP after pmap+GDT ready */
void	ap_start(int id);	/* C entry point for each AP */
void	smp_start_aps(int count); /* BSP: INIT/SIPI the APs */
int	mp_detect(void);	/* parse MP table / ACPI, return #CPUs */

/* AP trampoline symbols (from ap_tramp.s) */
extern char ap_tramp_start[], ap_tramp_end[];
extern unsigned ap_gdtr[], ap_pd[], ap_stack[], ap_cpuid[];

/* local APIC */
void	lapic_init(void);
void	lapic_eoi(void);
void	lapic_timer_init(void);
unsigned lapic_id(void);
void	lapic_send_ipi(int target, int vector);
#define	IPI_TLB_SHOOTDOWN	0x40
#define	IPI_RESCHEDULE		0x41
#define	IPI_STOP		0x42

/* spinlock (test-and-set) */
struct spinlock { volatile int locked; };
void	spin_init(struct spinlock *lk);
void	spin_lock(struct spinlock *lk);
void	spin_unlock(struct spinlock *lk);

/* per-CPU current thread via GS: we store a small header at GS:0 */
struct kthread *curthread_cpu(void);

/* run queue helpers (per-CPU) */
void	smp_balance(void);	/* migrate threads from busy to idle CPU */

/* S200: synchronization + IPI helpers */
extern int smp_enabled;
int	smp_enable(void);		/* runtime opt-in: map APIC, start APs */
void	smp_tlb_shootdown(unsigned va);
void	smp_reschedule(void);
void	smp_handle_ipi(int vector);
void	smp_print(void);
int	ipi_selftest(void);
#endif /* !ASSEMBLER */

#endif /* !_SMP_H_ */
