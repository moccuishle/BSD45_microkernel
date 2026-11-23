/* 
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * The Mach Operating System project at Carnegie-Mellon University.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)vm_glue.c	8.9 (Berkeley) 3/4/95
 *
 *
 * Copyright (c) 1987, 1990 Carnegie-Mellon University.
 * All rights reserved.
 * 
 * Permission to use, copy, modify and distribute this software and
 * its documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS" 
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND 
 * FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kprocess.h>
#include <sys/resourcevar.h>
#include <sys/buf.h>
#include <sys/user.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>

#include <machine/cpu.h>

int	avefree = 0;		/* XXX */
unsigned maxdmap = MAXDSIZ;	/* XXX */
int	readbuffers = 0;	/* XXX allow kgdb to read kernel buffer pool */

/* 中文：检查内核地址范围是否可按 rw 指定方式（读/写）访问，缓冲区缓存区域除外 */
/* EN: Check whether a kernel address range is accessible for read/write as given, excluding the buffer cache area */
int
kernacc(addr, len, rw)
	caddr_t addr;
	int len, rw;
{
	boolean_t rv;
	vm_offset_t saddr, eaddr;
	vm_prot_t prot = rw == B_READ ? VM_PROT_READ : VM_PROT_WRITE;

	saddr = trunc_page(addr);
	eaddr = round_page(addr+len);
	rv = vm_map_check_protection(kernel_map, saddr, eaddr, prot);
	/*
	 * XXX there are still some things (e.g. the buffer cache) that
	 * are managed behind the VM system's back so even though an
	 * address is accessible in the mind of the VM system, there may
	 * not be physical pages where the VM thinks there is.  This can
	 * lead to bogus allocation of pages in the kernel address space
	 * or worse, inconsistencies at the pmap level.  We only worry
	 * about the buffer cache for now.
	 */
	if (!readbuffers && rv && (eaddr > (vm_offset_t)buffers &&
		   saddr < (vm_offset_t)buffers + MAXBSIZE * nbuf))
		rv = FALSE;
	return(rv == TRUE);
}

/* 中文：检查用户地址范围在进程 vm_map 中是否可按 rw 指定方式访问 */
/* EN: Check whether a user address range is accessible for read/write in the process vm_map */
int
useracc(addr, len, rw)
	caddr_t addr;
	int len, rw;
{
	boolean_t rv;
	vm_prot_t prot = rw == B_READ ? VM_PROT_READ : VM_PROT_WRITE;

	rv = vm_map_check_protection(&curproc->p_vmspace->vm_map,
	    trunc_page(addr), round_page(addr+len), prot);
	return(rv == TRUE);
}

#ifdef KGDB
/*
 * Change protections on kernel pages from addr to addr+len
 * (presumably so debugger can plant a breakpoint).
 *
 * We force the protection change at the pmap level.  If we were
 * to use vm_map_protect a change to allow writing would be lazily-
 * applied meaning we would still take a protection fault, something
 * we really don't want to do.  It would also fragment the kernel
 * map unnecessarily.  We cannot use pmap_protect since it also won't
 * enforce a write-enable request.  Using pmap_enter is the only way
 * we can ensure the change takes place properly.
 */
/* 中文：在 pmap 层强制改变内核页保护（供调试器插入断点），逐页重映射 */
/* EN: Force a protection change on kernel pages at the pmap level (for debugger breakpoints), page by page */
void
chgkprot(addr, len, rw)
	register caddr_t addr;
	int len, rw;
{
	vm_prot_t prot;
	vm_offset_t pa, sva, eva;

	prot = rw == B_READ ? VM_PROT_READ : VM_PROT_READ|VM_PROT_WRITE;
	eva = round_page(addr + len);
	for (sva = trunc_page(addr); sva < eva; sva += PAGE_SIZE) {
		/*
		 * Extract physical address for the page.
		 * We use a cheezy hack to differentiate physical
		 * page 0 from an invalid mapping, not that it
		 * really matters...
		 */
		pa = pmap_extract(kernel_pmap, sva|1);
		if (pa == 0)
			panic("chgkprot: invalid page");
		pmap_enter(kernel_pmap, sva, pa&~1, prot, TRUE);
	}
}
#endif

/* 中文：锁定（标记为不可分页）当前进程地址空间中的指定范围 */
/* EN: Wire (mark non-pageable) the given range in the current process's address space */
void
vslock(addr, len)
	caddr_t	addr;
	u_int	len;
{
	vm_map_pageable(&curproc->p_vmspace->vm_map, trunc_page(addr),
			round_page(addr+len), FALSE);
}

/* 中文：解锁（标记为可分页）当前进程地址空间中的指定范围 */
/* EN: Unwire (mark pageable) the given range in the current process's address space */
void
vsunlock(addr, len, dirtied)
	caddr_t	addr;
	u_int	len;
	int dirtied;
{
#ifdef	lint
	dirtied++;
#endif
	vm_map_pageable(&curproc->p_vmspace->vm_map, trunc_page(addr),
			round_page(addr+len), TRUE);
}

/*
 * Implement fork's actions on an address space.
 * Here we arrange for the address space to be copied or referenced,
 * allocate a user struct (pcb and kernel stack), then call the
 * machine-dependent layer to fill those in and make the new process
 * ready to run.
 * NOTE: the kernel stack may be at a different location in the child
 * process, and thus addresses of automatic variables may be invalid
 * after cpu_fork returns in the child process.  We do nothing here
 * after cpu_fork returns.
 */
/* 中文：实现 fork 对地址空间的操作：复制/共享地址空间、分配 user 结构与内核栈，再调用机器相关层 */
/* EN: Implement fork's address-space actions: copy/share the space, allocate user struct and kernel stack, then call cpu_fork */
int
vm_fork(p1, p2, isvfork)
	register struct process *p1, *p2;
	int isvfork;
{
	register struct user *up;
	vm_offset_t addr;

#ifdef i386
	/*
	 * avoid copying any of the parent's pagetables or other per-process
	 * objects that reside in the map by marking all of them non-inheritable
	 */
	(void)vm_map_inherit(&p1->p_vmspace->vm_map,
		UPT_MIN_ADDRESS-UPAGES*NBPG, VM_MAX_ADDRESS, VM_INHERIT_NONE);
#endif
	p2->p_vmspace = vmspace_fork(p1->p_vmspace);

#ifdef SYSVSHM
	if (p1->p_vmspace->vm_shm)
		shmfork(p1, p2, isvfork);
#endif

#ifndef	i386
	/*
	 * Allocate a wired-down (for now) pcb and kernel stack for the process
	 */
	addr = kmem_alloc_pageable(kernel_map, ctob(UPAGES));
	if (addr == 0)
		panic("vm_fork: no more kernel virtual memory");
	vm_map_pageable(kernel_map, addr, addr + ctob(UPAGES), FALSE);
#else
/* XXX somehow, on 386, ocassionally pageout removes active, wired down kstack,
and pagetables, WITHOUT going thru vm_page_unwire! Why this appears to work is
not yet clear, yet it does... */
	addr = kmem_alloc(kernel_map, ctob(UPAGES));
	if (addr == 0)
		panic("vm_fork: no more kernel virtual memory");
#endif
	up = (struct user *)addr;
	p2->p_addr = up;

	/*
	 * p_stats and p_sigacts currently point at fields
	 * in the user struct but not at &u, instead at p_addr.
	 * Copy p_sigacts and parts of p_stats; zero the rest
	 * of p_stats (statistics).
	 */
	p2->p_stats = &up->u_stats;
	p2->p_sigacts = &up->u_sigacts;
	up->u_sigacts = *p1->p_sigacts;
	bzero(&up->u_stats.pstat_startzero,
	    (unsigned) ((caddr_t)&up->u_stats.pstat_endzero -
	    (caddr_t)&up->u_stats.pstat_startzero));
	bcopy(&p1->p_stats->pstat_startcopy, &up->u_stats.pstat_startcopy,
	    ((caddr_t)&up->u_stats.pstat_endcopy -
	     (caddr_t)&up->u_stats.pstat_startcopy));

#ifdef i386
	{ u_int addr = UPT_MIN_ADDRESS - UPAGES*NBPG; struct vm_map *vp;

	vp = &p2->p_vmspace->vm_map;
	(void)vm_deallocate(vp, addr, UPT_MAX_ADDRESS - addr);
	(void)vm_allocate(vp, &addr, UPT_MAX_ADDRESS - addr, FALSE);
	(void)vm_map_inherit(vp, addr, UPT_MAX_ADDRESS, VM_INHERIT_NONE);
	}
#endif
	/*
	 * cpu_fork will copy and update the kernel stack and pcb,
	 * and make the child ready to run.  It marks the child
	 * so that it can return differently than the parent.
	 * It returns twice, once in the parent process and
	 * once in the child.
	 */
	return (cpu_fork(p1, p2));
}

/*
 * Set default limits for VM system.
 * Called for proc 0, and then inherited by all others.
 */
/* 中文：为进程设置 VM 系统的默认资源限制（栈、数据、常驻集大小） */
/* EN: Set default VM resource limits (stack, data, resident set size) for a process */
void
vm_init_limits(p)
	register struct process *p;
{

	/*
	 * Set up the initial limits on process VM.
	 * Set the maximum resident set size to be all
	 * of (reasonably) available memory.  This causes
	 * any single, large process to start random page
	 * replacement once it fills memory.
	 */
        p->p_rlimit[RLIMIT_STACK].rlim_cur = DFLSSIZ;
        p->p_rlimit[RLIMIT_STACK].rlim_max = MAXSSIZ;
        p->p_rlimit[RLIMIT_DATA].rlim_cur = DFLDSIZ;
        p->p_rlimit[RLIMIT_DATA].rlim_max = MAXDSIZ;
	p->p_rlimit[RLIMIT_RSS].rlim_cur = ptoa(cnt.v_free_count);
}

#include <vm/vm_pageout.h>

#ifdef DEBUG
int	enableswap = 1;
int	swapdebug = 0;
#define	SDB_FOLLOW	1
#define SDB_SWAPIN	2
#define SDB_SWAPOUT	4
#endif

/*
 * Brutally simple:
 *	1. Attempt to swapin every swaped-out, runnable process in
 *	   order of priority.
 *	2. If not enough memory, wake the pageout daemon and let it
 *	   clear some space.
 */
/* 中文：交换调度器：按优先级把已换出的可运行进程换入，内存不足时唤醒 pageout 守护进程 */
/* EN: Swap scheduler: swap in highest-priority swapped-out runnable processes, waking the pageout daemon when memory is low */
void
scheduler()
{
	register struct process *p;
	register int pri;
	struct process *pp;
	int ppri;
	vm_offset_t addr;
	vm_size_t size;

loop:
#ifdef DEBUG
	while (!enableswap)
		tsleep((caddr_t)&proc0, PVM, "noswap", 0);
#endif
	pp = NULL;
	ppri = INT_MIN;
	for (p = allproc.lh_first; p != 0; p = p->p_list.le_next) {
		if (p->p_stat == SRUN && (p->p_flag & P_INMEM) == 0) {
			/* XXX should also penalize based on vm_swrss */
			pri = p->p_swtime + p->p_slptime - p->p_nice * 8;
			if (pri > ppri) {
				pp = p;
				ppri = pri;
			}
		}
	}
#ifdef DEBUG
	if (swapdebug & SDB_FOLLOW)
		printf("scheduler: running, procp %x pri %d\n", pp, ppri);
#endif
	/*
	 * Nothing to do, back to sleep
	 */
	if ((p = pp) == NULL) {
		tsleep((caddr_t)&proc0, PVM, "scheduler", 0);
		goto loop;
	}

	/*
	 * We would like to bring someone in.
	 * This part is really bogus cuz we could deadlock on memory
	 * despite our feeble check.
	 * XXX should require at least vm_swrss / 2
	 */
	size = round_page(ctob(UPAGES));
	addr = (vm_offset_t) p->p_addr;
	if (cnt.v_free_count > atop(size)) {
#ifdef DEBUG
		if (swapdebug & SDB_SWAPIN)
			printf("swapin: pid %d(%s)@%x, pri %d free %d\n",
			       p->p_pid, p->p_comm, p->p_addr,
			       ppri, cnt.v_free_count);
#endif
		vm_map_pageable(kernel_map, addr, addr+size, FALSE);
		/*
		 * Some architectures need to be notified when the
		 * user area has moved to new physical page(s) (e.g.
		 * see pmax/pmax/vm_machdep.c).
		 */
		cpu_swapin(p);
		(void) splstatclock();
		if (p->p_stat == SRUN)
			setrunqueue(p);
		p->p_flag |= P_INMEM;
		(void) spl0();
		p->p_swtime = 0;
		goto loop;
	}
	/*
	 * Not enough memory, jab the pageout daemon and wait til the
	 * coast is clear.
	 */
#ifdef DEBUG
	if (swapdebug & SDB_FOLLOW)
		printf("scheduler: no room for pid %d(%s), free %d\n",
		       p->p_pid, p->p_comm, cnt.v_free_count);
#endif
	(void) splhigh();
	VM_WAIT;
	(void) spl0();
#ifdef DEBUG
	if (swapdebug & SDB_FOLLOW)
		printf("scheduler: room again, free %d\n", cnt.v_free_count);
#endif
	goto loop;
}

#define	swappable(p)							\
	(((p)->p_flag &							\
	    (P_SYSTEM | P_INMEM | P_NOSWAP | P_WEXIT | P_PHYSIO)) == P_INMEM)

/*
 * Swapout is driven by the pageout daemon.  Very simple, we find eligible
 * procs and unwire their u-areas.  We try to always "swap" at least one
 * process in case we need the room for a swapin.
 * If any procs have been sleeping/stopped for at least maxslp seconds,
 * they are swapped.  Else, we swap the longest-sleeping or stopped process,
 * if any, otherwise the longest-resident process.
 */
/* 中文：由 pageout 守护进程驱动，选择睡眠/停止时间最长或常驻最久的进程换出 */
/* EN: Driven by the pageout daemon: select and swap out the longest-sleeping/stopped or longest-resident process */
void
swapout_threads()
{
	register struct process *p;
	struct process *outp, *outp2;
	int outpri, outpri2;
	int didswap = 0;
	extern int maxslp;

#ifdef DEBUG
	if (!enableswap)
		return;
#endif
	outp = outp2 = NULL;
	outpri = outpri2 = 0;
	for (p = allproc.lh_first; p != 0; p = p->p_list.le_next) {
		if (!swappable(p))
			continue;
		switch (p->p_stat) {
		case SRUN:
			if (p->p_swtime > outpri2) {
				outp2 = p;
				outpri2 = p->p_swtime;
			}
			continue;
			
		case SSLEEP:
		case SSTOP:
			if (p->p_slptime >= maxslp) {
				swapout(p);
				didswap++;
			} else if (p->p_slptime > outpri) {
				outp = p;
				outpri = p->p_slptime;
			}
			continue;
		}
	}
	/*
	 * If we didn't get rid of any real duds, toss out the next most
	 * likely sleeping/stopped or running candidate.  We only do this
	 * if we are real low on memory since we don't gain much by doing
	 * it (UPAGES pages).
	 */
	if (didswap == 0 &&
	    cnt.v_free_count <= atop(round_page(ctob(UPAGES)))) {
		if ((p = outp) == 0)
			p = outp2;
#ifdef DEBUG
		if (swapdebug & SDB_SWAPOUT)
			printf("swapout_threads: no duds, try procp %x\n", p);
#endif
		if (p)
			swapout(p);
	}
}

/* 中文：换出进程 p：解除 u 区映射、清除 P_INMEM 标志并从运行队列移除 */
/* EN: Swap out process p: unwire its u-area, clear P_INMEM and remove it from the run queue */
void
swapout(p)
	register struct process *p;
{
	vm_offset_t addr;
	vm_size_t size;

#ifdef DEBUG
	if (swapdebug & SDB_SWAPOUT)
		printf("swapout: pid %d(%s)@%x, stat %x pri %d free %d\n",
		       p->p_pid, p->p_comm, p->p_addr, p->p_stat,
		       p->p_slptime, cnt.v_free_count);
#endif
	size = round_page(ctob(UPAGES));
	addr = (vm_offset_t) p->p_addr;
#if defined(hp300) || defined(luna68k)
	/*
	 * Ugh!  u-area is double mapped to a fixed address behind the
	 * back of the VM system and accesses are usually through that
	 * address rather than the per-process address.  Hence reference
	 * and modify information are recorded at the fixed address and
	 * lost at context switch time.  We assume the u-struct and
	 * kernel stack are always accessed/modified and force it to be so.
	 */
	{
		register int i;
		volatile long tmp;

		for (i = 0; i < UPAGES; i++) {
			tmp = *(long *)addr; *(long *)addr = tmp;
			addr += NBPG;
		}
		addr = (vm_offset_t) p->p_addr;
	}
#endif
#ifdef mips
	/*
	 * Be sure to save the floating point coprocessor state before
	 * paging out the u-struct.
	 */
	{
		extern struct process *machFPCurProcPtr;

		if (p == machFPCurProcPtr) {
			MachSaveCurFPState(p);
			machFPCurProcPtr = (struct process *)0;
		}
	}
#endif
#ifndef	i386 /* temporary measure till we find spontaineous unwire of kstack */
	vm_map_pageable(kernel_map, addr, addr+size, TRUE);
	pmap_collect(vm_map_pmap(&p->p_vmspace->vm_map));
#endif
	(void) splhigh();
	p->p_flag &= ~P_INMEM;
	if (p->p_stat == SRUN)
		remrq(p);
	(void) spl0();
	p->p_swtime = 0;
}

/*
 * The rest of these routines fake thread handling
 */

/* 中文：伪线程机制：记录当前进程将要等待的事件（ ruptible 参数未用） */
/* EN: Fake thread support: record the event the current process is about to wait on (ruptible unused) */
void
assert_wait(event, ruptible)
	void *event;
	boolean_t ruptible;
{
#ifdef lint
	ruptible++;
#endif
	curproc->p_thread = event;
}

/* 中文：伪线程机制：在已记录的等待事件上调用 tsleep 阻塞当前进程 */
/* EN: Fake thread support: block the current process with tsleep on the recorded wait event */
void
thread_block()
{
	int s = splhigh();

	if (curproc->p_thread)
		tsleep(curproc->p_thread, PVM, "thrd_block", 0);
	splx(s);
}

/* 中文：伪线程机制：记录等待事件、释放锁并在该事件上 tsleep 阻塞 */
/* EN: Fake thread support: record the wait event, unlock, then block with tsleep on that event */
void
thread_sleep(event, lock, ruptible)
	void *event;
	simple_lock_t lock;
	boolean_t ruptible;
{
	int s = splhigh();

#ifdef lint
	ruptible++;
#endif
	curproc->p_thread = event;
	simple_unlock(lock);
	if (curproc->p_thread)
		tsleep(event, PVM, "thrd_sleep", 0);
	splx(s);
}

/* 中文：伪线程机制：唤醒在指定事件上等待的进程 */
/* EN: Fake thread support: wake processes waiting on the given event */
void
thread_wakeup(event)
	void *event;
{
	int s = splhigh();

	wakeup(event);
	splx(s);
}

/*
 * DEBUG stuff
 */

int indent = 0;

#include <machine/stdarg.h>		/* see subr_prf.c */

/*ARGSUSED2*/
/* 中文：按全局 indent 缩进后按格式打印（调试辅助函数） */
/* EN: Print formatted output with indentation per the global indent (debug helper) */
void
#if __STDC__
iprintf(const char *fmt, ...)
#else
iprintf(fmt /* , va_alist */)
	char *fmt;
	/* va_dcl */
#endif
{
	register int i;
	va_list ap;

	for (i = indent; i >= 8; i -= 8)
		printf("\t");
	while (--i >= 0)
		printf(" ");
	va_start(ap, fmt);
	printf("%r", fmt, ap);
	va_end(ap);
}
