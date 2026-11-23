/*-
 * Copyright (c) 1982, 1987, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * William Jolitz.
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
 *	@(#)machdep.c	8.3 (Berkeley) 5/9/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/signalvar.h>
#include <sys/kernel.h>
#include <sys/map.h>
#include <sys/kprocess.h>
#include <sys/user.h>
#include <sys/buf.h>
#include <sys/reboot.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/clist.h>
#include <sys/callout.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/msgbuf.h>
#include <sys/ioctl.h>
#include <sys/tty.h>
#include <sys/sysctl.h>

#include <net/netisr.h>

#include <vm/vm.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>

vm_map_t buffer_map;
extern vm_offset_t avail_end;

#include <machine/cpu.h>
#include <machine/reg.h>
#include <machine/psl.h>

/* S413: MAXCPU / CPU_TSS_SEL_BASE for the per-CPU GDT TSS slots. */
#include "sys/smp.h"
#include <machine/specialreg.h>
#include <i386/isa/rtc.h>
#include <i386/i386/cons.h>

#ifdef BSD45_TRACE
#define MTRACE(c) outb(0xE9, (c))
#else
#define MTRACE(c)
#endif

/*
 * Declare these as initialized data so we can patch them.
 */
int	nswbuf = 0;
#ifdef	NBUF
int	nbuf = NBUF;
#else
int	nbuf = 0;
#endif
#ifdef	BUFPAGES
int	bufpages = BUFPAGES;
#else
int	bufpages = 0;
#endif
int	msgbufmapped;		/* set when safe to use msgbuf */

/*
 * Machine-dependent startup code
 */
int boothowto = 0, Maxmem = 0;
long dumplo;
int physmem, maxmem;
extern int bootdev;
#ifdef SMALL
extern int forcemaxmem;
#endif
int biosmem;

extern cyloffset;

/* EN: Maps msgbuf at end of core, two-pass allocates kernel tables, buffers and callout free list, creates buffer/exec/phys/mbuf submaps, then initcpu, bufinit and configure. */
/* 中文：在核心末尾映射 msgbuf，两遍分配内核表、缓冲区与 callout 空闲链，建立 buffer/exec/phys/mbuf 子映射，最后调用 initcpu、bufinit 和 configure。 */
cpu_startup(firstaddr)
	int firstaddr;
{
	register int unixsize;
	register unsigned i;
	register struct pte *pte;
	int mapaddr, j;
	register caddr_t v;
	int maxbufs, base, residual;
	extern long Usrptsize;
	vm_offset_t minaddr, maxaddr;
	vm_size_t size;

	/*
	 * Initialize error message buffer (at end of core).
	 */

	/* avail_end was pre-decremented in pmap_bootstrap to compensate */
	for (i = 0; i < btoc(sizeof (struct msgbuf)); i++)
		pmap_enter(kernel_pmap, msgbufp, avail_end + i * NBPG,
			   VM_PROT_ALL, TRUE);
	msgbufmapped = 1;

#ifdef KDB
	kdb_init();			/* startup kernel debugger */
#endif
	/*
	 * Good {morning,afternoon,evening,night}.
	 */
	printf(version);
	printf("real mem  = %d\n", ctob(physmem));

	/*
	 * Allocate space for system data structures.
	 * The first available real memory address is in "firstaddr".
	 * The first available kernel virtual address is in "v".
	 * As pages of kernel virtual memory are allocated, "v" is incremented.
	 * As pages of memory are allocated and cleared,
	 * "firstaddr" is incremented.
	 * An index into the kernel page table corresponding to the
	 * virtual memory address maintained in "v" is kept in "mapaddr".
	 */

	/*
	 * Make two passes.  The first pass calculates how much memory is
	 * needed and allocates it.  The second pass assigns virtual
	 * addresses to the various data structures.
	 */
	firstaddr = 0;
again:
	v = (caddr_t)firstaddr;

#define	valloc(name, type, num) \
	    (name) = (type *)v; v = (caddr_t)((name)+(num))
#define	valloclim(name, type, num, lim) \
	    (name) = (type *)v; v = (caddr_t)((lim) = ((name)+(num)))
	valloc(cfree, struct cblock, nclist);
	valloc(callout, struct callout, ncallout);
	valloc(swapmap, struct map, nswapmap = maxproc * 2);
#ifdef SYSVSHM
	valloc(shmsegs, struct shmid_ds, shminfo.shmmni);
#endif
	/*
	 * Determine how many buffers to allocate.
	 * Use 10% of memory for the first 2 Meg, 5% of the remaining
	 * memory. Insure a minimum of 16 buffers.
	 * We allocate 1/2 as many swap buffer headers as file i/o buffers.
	 */
	if (bufpages == 0)
		if (physmem < (2 * 1024 * 1024))
			bufpages = physmem / 10 / CLSIZE;
		else
			bufpages = ((2 * 1024 * 1024 + physmem) / 20) / CLSIZE;
	if (nbuf == 0) {
		nbuf = bufpages / 2;
		if (nbuf < 16)
			nbuf = 16;
	}
	if (nswbuf == 0) {
		nswbuf = (nbuf / 2) &~ 1;	/* force even */
		if (nswbuf > 256)
			nswbuf = 256;		/* sanity */
	}
	valloc(swbuf, struct buf, nswbuf);
	valloc(buf, struct buf, nbuf);

	/*
	 * End of first pass, size has been calculated so allocate memory
	 */
	if (firstaddr == 0) {
		size = (vm_size_t)(v - firstaddr);
		firstaddr = (int)kmem_alloc(kernel_map, round_page(size));
		if (firstaddr == 0)
			panic("startup: no room for tables");
		goto again;
	}
	/*
	 * End of second pass, addresses have been assigned
	 */
	if ((vm_size_t)(v - firstaddr) != size)
		panic("startup: table size inconsistency");
	/*
	 * Now allocate buffers proper.  They are different than the above
	 * in that they usually occupy more virtual memory than physical.
	 */
	size = MAXBSIZE * nbuf;
	buffer_map = kmem_suballoc(kernel_map, (vm_offset_t)&buffers,
				   &maxaddr, size, TRUE);
	minaddr = (vm_offset_t)buffers;
	if (vm_map_find(buffer_map, vm_object_allocate(size), (vm_offset_t)0,
			&minaddr, size, FALSE) != KERN_SUCCESS)
		panic("startup: cannot allocate buffers");
	base = bufpages / nbuf;
	residual = bufpages % nbuf;
	for (i = 0; i < nbuf; i++) {
		vm_size_t curbufsize;
		vm_offset_t curbuf;

		/*
		 * First <residual> buffers get (base+1) physical pages
		 * allocated for them.  The rest get (base) physical pages.
		 *
		 * The rest of each buffer occupies virtual space,
		 * but has no physical memory allocated for it.
		 */
		curbuf = (vm_offset_t)buffers + i * MAXBSIZE;
		curbufsize = CLBYTES * (i < residual ? base+1 : base);
		vm_map_pageable(buffer_map, curbuf, curbuf+curbufsize, FALSE);
		vm_map_simplify(buffer_map, curbuf);
	}
	/*
	 * Allocate a submap for exec arguments.  This map effectively
	 * limits the number of processes exec'ing at any time.
	 */
	exec_map = kmem_suballoc(kernel_map, &minaddr, &maxaddr,
				 16*NCARGS, TRUE);
	/*
	 * Allocate a submap for physio
	 */
	phys_map = kmem_suballoc(kernel_map, &minaddr, &maxaddr,
				 VM_PHYS_SIZE, TRUE);

	/*
	 * Finally, allocate mbuf pool.  Since mclrefcnt is an off-size
	 * we use the more space efficient malloc in place of kmem_alloc.
	 */
	mclrefcnt = (char *)malloc(NMBCLUSTERS+CLBYTES/MCLBYTES,
				   M_MBUF, M_NOWAIT);
	bzero(mclrefcnt, NMBCLUSTERS+CLBYTES/MCLBYTES);
	mb_map = kmem_suballoc(kernel_map, (vm_offset_t)&mbutl, &maxaddr,
			       VM_MBUF_SIZE, FALSE);
	/*
	 * Initialize callouts
	 */
	callfree = callout;
	for (i = 1; i < ncallout; i++)
		callout[i-1].c_next = &callout[i];
	callout[i-1].c_next = NULL;

	/*printf("avail mem = %d\n", ptoa(vm_page_free_count));*/
	printf("using %d buffers containing %d bytes of memory\n",
		nbuf, bufpages * CLBYTES);

	/*
	 * Set up CPU-specific registers, cache, etc.
	 */
	initcpu();

	/*
	 * Set up buffers, so they can be used to read disk labels.
	 */
	bufinit();

	/*
	 * Configure the system.
	 */
	configure();
}

#ifdef PGINPROF
/*
 * Return the difference (in microseconds)
 * between the  current time and a previous
 * time as represented  by the arguments.
 * If there is a pending clock interrupt
 * which has not been serviced due to high
 * ipl, return error code.
 */
/*ARGSUSED*/
/* EN: PGINPROF-only helper: returns elapsed microseconds as ((time.tv_sec-otime)*60 + lbolt-olbolt)*16667 against saved time/lbolt values, compiled under PGINPROF. */
/* 中文：仅在 PGINPROF 下编译的辅助函数：按 ((time.tv_sec-otime)*60+lbolt-olbolt)*16667 计算相对保存时刻的微秒差。 */
vmtime(otime, olbolt, oicr)
	register int otime, olbolt, oicr;
{

	return (((time.tv_sec-otime)*60 + lbolt-olbolt)*16667);
}
#endif

struct sigframe {
	int	sf_signum;
	int	sf_code;
	struct	sigcontext *sf_scp;
	sig_t	sf_handler;
	int	sf_eax;	
	int	sf_edx;	
	int	sf_ecx;	
	struct	sigcontext sf_sc;
} ;

extern int kstack[];

/*
 * Send an interrupt to process.
 *
 * Stack is set up to allow sigcode stored
 * in u. to call routine, followed by kcall
 * to sigreturn routine below.  After sigreturn
 * resets the signal mask, the stack, and the
 * frame pointer, it returns to the user
 * specified pc, psl.
 */
/* EN: Builds a sigframe on the user or alternate stack, grows and useracc-checks it, saves regs and sigcontext, sets ESP/EIP to the sigcode trampoline; bad stack gets SIGILL. */
/* 中文：在用户或备用栈上构造 sigframe，扩展并 useracc 校验，保存寄存器与 sigcontext，把 ESP/EIP 指向 sigcode 跳板；栈无效时改投 SIGILL。 */
void
sendsig(catcher, sig, mask, code)
	sig_t catcher;
	int sig, mask;
	u_long code;
{
	register struct process *p = curproc;
	register int *regs;
	register struct sigframe *fp;
	struct sigacts *psp = p->p_sigacts;
	int oonstack, frmtrap;

	regs = p->p_md.md_regs;
        oonstack = psp->ps_sigstk.ss_flags & SA_ONSTACK;
	frmtrap = curpcb->pcb_flags & FM_TRAP;
	/*
	 * Allocate and validate space for the signal handler
	 * context. Note that if the stack is in P0 space, the
	 * call to grow() is a nop, and the useracc() check
	 * will fail if the process has not already allocated
	 * the space with a `brk'.
	 */
	if ((psp->ps_flags & SAS_ALTSTACK) &&
	    (psp->ps_sigstk.ss_flags & SA_ONSTACK) == 0 &&
	    (psp->ps_sigonstack & sigmask(sig))) {
		fp = (struct sigframe *)(psp->ps_sigstk.ss_base +
		    psp->ps_sigstk.ss_size - sizeof(struct sigframe));
		psp->ps_sigstk.ss_flags |= SA_ONSTACK;
	} else {
		if (frmtrap)
			fp = (struct sigframe *)(regs[tESP]
				- sizeof(struct sigframe));
		else
			fp = (struct sigframe *)(regs[sESP]
				- sizeof(struct sigframe));
	}

	if ((unsigned)fp <= USRSTACK - ctob(p->p_vmspace->vm_ssize)) 
		(void)grow(p, (unsigned)fp);

	if (useracc((caddr_t)fp, sizeof (struct sigframe), B_WRITE) == 0) {
		/*
		 * Process has trashed its stack; give it an illegal
		 * instruction to halt it in its tracks.
		 */
		SIGACTION(p, SIGILL) = SIG_DFL;
		sig = sigmask(SIGILL);
		p->p_sigignore &= ~sig;
		p->p_sigcatch &= ~sig;
		p->p_sigmask &= ~sig;
		psignal(p, SIGILL);
		return;
	}

	/* 
	 * Build the argument list for the signal handler.
	 */
	fp->sf_signum = sig;
	fp->sf_code = code;
	fp->sf_scp = &fp->sf_sc;
	fp->sf_handler = catcher;

	/* save scratch registers */
	if(frmtrap) {
		fp->sf_eax = regs[tEAX];
		fp->sf_edx = regs[tEDX];
		fp->sf_ecx = regs[tECX];
	} else {
		fp->sf_eax = regs[sEAX];
		fp->sf_edx = regs[sEDX];
		fp->sf_ecx = regs[sECX];
	}
	/*
	 * Build the signal context to be used by sigreturn.
	 */
	fp->sf_sc.sc_onstack = oonstack;
	fp->sf_sc.sc_mask = mask;
	if(frmtrap) {
		fp->sf_sc.sc_sp = regs[tESP];
		fp->sf_sc.sc_fp = regs[tEBP];
		fp->sf_sc.sc_pc = regs[tEIP];
		fp->sf_sc.sc_ps = regs[tEFLAGS];
		regs[tESP] = (int)fp;
		regs[tEIP] = (int)((struct pcb *)kstack)->pcb_sigc;
	} else {
		fp->sf_sc.sc_sp = regs[sESP];
		fp->sf_sc.sc_fp = regs[sEBP];
		fp->sf_sc.sc_pc = regs[sEIP];
		fp->sf_sc.sc_ps = regs[sEFLAGS];
		regs[sESP] = (int)fp;
		regs[sEIP] = (int)((struct pcb *)kstack)->pcb_sigc;
	}
}

/*
 * System call to cleanup state after a signal
 * has been taken.  Reset signal mask and
 * stack state from context left by sendsig (above).
 * Return to previous pc and psl as specified by
 * context left by sendsig. Check carefully to
 * make sure that the user has not modified the
 * psl to gain improper priviledges or to cause
 * a machine fault.
 */
struct sigreturn_args {
	struct sigcontext *sigcntxp;
};
/* EN: Restores scratch regs and sigcontext (sp/fp/pc/ps, signal mask, onstack flag) from the frame left by sendsig after useracc checks, returns EJUSTRETURN to resume user code. */
/* 中文：经 useracc 校验后从 sendsig 留下的帧恢复寄存器与 sigcontext（sp/fp/pc/ps、信号屏蔽、onstack 标志），返回 EJUSTRETURN 继续用户态执行。 */
sigreturn(p, uap, retval)
	struct process *p;
	struct sigreturn_args *uap;
	int *retval;
{
	register struct sigcontext *scp;
	register struct sigframe *fp;
	register int *regs = p->p_md.md_regs;


	fp = (struct sigframe *) regs[sESP] ;

	if (useracc((caddr_t)fp, sizeof (*fp), 0) == 0)
		return(EINVAL);

	/* restore scratch registers */
	regs[sEAX] = fp->sf_eax ;
	regs[sEDX] = fp->sf_edx ;
	regs[sECX] = fp->sf_ecx ;

	scp = fp->sf_scp;
	if (useracc((caddr_t)scp, sizeof (*scp), 0) == 0)
		return(EINVAL);
#ifdef notyet
	if ((scp->sc_ps & PSL_MBZ) != 0 || (scp->sc_ps & PSL_MBO) != PSL_MBO) {
		return(EINVAL);
	}
#endif
	if (scp->sc_onstack & 01)
		p->p_sigacts->ps_sigstk.ss_flags |= SA_ONSTACK;
	else
		p->p_sigacts->ps_sigstk.ss_flags &= ~SA_ONSTACK;
	p->p_sigmask = scp->sc_mask &~
	    (sigmask(SIGKILL)|sigmask(SIGCONT)|sigmask(SIGSTOP));
	regs[sEBP] = scp->sc_fp;
	regs[sESP] = scp->sc_sp;
	regs[sEIP] = scp->sc_pc;
	regs[sEFLAGS] = scp->sc_ps;
	return(EJUSTRETURN);
}

int	waittime = -1;

/* EN: Reboots: unless RB_NOSYNC with waittime<0 it syncs disks, unmounts filesystems and waits up to 20 rounds for busy buffers, then halts, dumps or resets the CPU. */
/* 中文：重启流程：非 RB_NOSYNC 时同步磁盘、卸载文件系统并最多等待 20 轮忙缓冲区，随后按 RB_HALT 停机、按 RB_DUMP 转储或复位 CPU。 */
boot(arghowto)
	int arghowto;
{
	register long dummy;		/* r12 is reserved */
	register int howto;		/* r11 == how to boot */
	register int devtype;		/* r10 == major of root dev */
	extern const char *panicstr;
extern int cold;

	howto = arghowto;
	if ((howto&RB_NOSYNC) == 0 && waittime < 0) {
		register struct buf *bp;
		int iter, nbusy;

		waittime = 0;
		(void) splnet();
		printf("syncing disks... ");
		/*
		 * Release inodes held by texts before update.
		 */
		if (panicstr == 0)
			vnode_pager_umount(NULL);
		sync((struct sigcontext *)0);
		/*
		 * Unmount filesystems
		 */
		if (panicstr == 0)
			vfs_unmountall();

		for (iter = 0; iter < 20; iter++) {
			nbusy = 0;
			for (bp = &buf[nbuf]; --bp >= buf; )
				if ((bp->b_flags & (B_BUSY|B_INVAL)) == B_BUSY)
					nbusy++;
			if (nbusy == 0)
				break;
			printf("%d ", nbusy);
			DELAY(40000 * iter);
		}
		if (nbusy)
			printf("giving up\n");
		else
			printf("done\n");
		DELAY(10000);			/* wait for printf to finish */
	}
	splhigh();
	devtype = major(rootdev);
	if (howto&RB_HALT) {
		printf("halting (in tight loop); hit reset\n\n");
		splx(0xfffd);	/* all but keyboard XXX */
		for (;;) ;
	} else {
		if (howto & RB_DUMP) {
			dumpsys();	
			/*NOTREACHED*/
		}
	}
#ifdef lint
	dummy = 0; dummy = dummy;
	printf("howto %d, devtype %d\n", arghowto, devtype);
#endif
#ifdef	notdef
	pg("pausing (hit any key to reset)");
#endif
	reset_cpu();
	for(;;) ;
	/*NOTREACHED*/
}

int	dumpmag = 0x8fca0101;	/* magic number for savecore */
int	dumpsize = 0;		/* also for savecore */
/*
 * Doadump comes here after turning off memory management and
 * getting on the dump stack, either when called above, or by
 * the auto-restart code.
 */
/* EN: When dumpdev is valid and its minor&7 == 1, sets dumpsize=physmem and calls the device d_dump, printing success or ENXIO/EFAULT/EINVAL/EIO diagnostics. */
/* 中文：当 dumpdev 有效且 minor&7==1 时置 dumpsize=physmem 并调用块设备 d_dump 转储内存，按返回码打印成功或 ENXIO/EFAULT/EINVAL/EIO 信息。 */
dumpsys()
{

	if (dumpdev == NODEV)
		return;
	if ((minor(dumpdev)&07) != 1)
		return;
	dumpsize = physmem;
	printf("\ndumping to dev %x, offset %d\n", dumpdev, dumplo);
	printf("dump ");
	switch ((*bdevsw[major(dumpdev)].d_dump)(dumpdev)) {

	case ENXIO:
		printf("device bad\n");
		break;

	case EFAULT:
		printf("device not ready\n");
		break;

	case EINVAL:
		printf("area improper\n");
		break;

	case EIO:
		printf("i/o error\n");
		break;

	default:
		printf("succeeded\n");
		break;
	}
	printf("\n\n");
	DELAY(1000);
}

/* EN: Returns the current time with microsecond resolution: copies time into *tvp under splhigh, adds the pending tick and normalizes tv_usec below 1000000. */
/* 中文：给出微秒级当前时间：在 splhigh 下把 time 复制到 *tvp，加上未结算的 tick，并把 tv_usec 归一到 1000000 以内。 */
microtime(tvp)
	register struct timeval *tvp;
{
	int s = splhigh();

	*tvp = time;
	tvp->tv_usec += tick;
	while (tvp->tv_usec > 1000000) {
		tvp->tv_sec++;
		tvp->tv_usec -= 1000000;
	}
	splx(s);
}

/* 中文：物理 I/O 策略例程：vmapbuf 映射缓冲区后调用下层 strat，脏页推送不等待，否则在 splbio 下睡眠等待 B_DONE，最后还原映射与地址 */
/* EN: Physical I/O strategy: maps the buffer with vmapbuf and calls strat; returns immediately for dirty-page pushes, otherwise sleeps at splbio until B_DONE, then unmaps and restores the address */
physstrat(bp, strat, prio)
	struct buf *bp;
	int (*strat)(), prio;
{
	register int s;
	caddr_t baddr;

	/*
	 * vmapbuf clobbers b_addr so we must remember it so that it
	 * can be restored after vunmapbuf.  This is truely rude, we
	 * should really be storing this in a field in the buf struct
	 * but none are available and I didn't want to add one at
	 * this time.  Note that b_addr for dirty page pushes is 
	 * restored in vunmapbuf. (ugh!)
	 */
	baddr = bp->b_un.b_addr;
	vmapbuf(bp);
	(*strat)(bp);
	/* pageout daemon doesn't wait for pushed pages */
	if (bp->b_flags & B_DIRTY)
		return;
	s = splbio();
	while ((bp->b_flags & B_DONE) == 0)
		sleep((caddr_t)bp, prio);
	splx(s);
	vunmapbuf(bp);
	bp->b_un.b_addr = baddr;
}

/* EN: Empty stub compiled on this port: performs no CPU register or cache setup; cpu_startup calls it after system memory has been configured. */
/* 中文：本移植版中的空函数：不做任何 CPU 寄存器或缓存设置，由 cpu_startup 在系统内存配置完成后调用。 */
initcpu()
{
}

/*
 * Clear registers on exec
 */
/* EN: Resets registers for exec: EBP=0, EIP=entry, pcb_flags=0, sets CR0_EM to emulate floating point, and reinitializes the NPX via npxinit when configured. */
/* 中文：为 exec 重置寄存器：EBP 清零、EIP=entry、pcb_flags=0，置 CR0_EM 进入浮点模拟，并在配置了 NPX 时调用 npxinit 重新初始化。 */
setregs(p, entry, retval)
	register struct process *p;
	u_long entry;
	int retval[2];
{
	p->p_md.md_regs[sEBP] = 0;	/* bottom of the fp chain */
	p->p_md.md_regs[sEIP] = entry;

	p->p_addr->u_pcb.pcb_flags = 0;	/* no fp at all */
	load_cr0(rcr0() | CR0_EM);	/* start emulating */
#include "npx.h"
#if NNPX > 0
	npxinit(0x262);
#endif
}

/*
 * machine dependent system variables.
 */
/* EN: Machine-dependent sysctl handler: only CPU_CONSDEV is supported, returning cn_tty->t_dev via sysctl_rdstruct; namelen!=1 gives ENOTDIR, other names EOPNOTSUPP. */
/* 中文：机器相关 sysctl 处理：仅支持 CPU_CONSDEV，用 sysctl_rdstruct 返回 cn_tty->t_dev；namelen!=1 返回 ENOTDIR，其余名字返回 EOPNOTSUPP。 */
cpu_sysctl(name, namelen, oldp, oldlenp, newp, newlen, p)
	int *name;
	u_int namelen;
	void *oldp;
	size_t *oldlenp;
	void *newp;
	size_t newlen;
	struct process *p;
{

	/* all sysctl names at this level are terminal */
	if (namelen != 1)
		return (ENOTDIR);		/* overloaded */

	switch (name[0]) {
	case CPU_CONSDEV:
		return (sysctl_rdstruct(oldp, oldlenp, newp, &cn_tty->t_dev,
		    sizeof cn_tty->t_dev));
	default:
		return (EOPNOTSUPP);
	}
	/* NOTREACHED */
}

/*
 * Initialize 386 and configure to run kernel
 */

/*
 * Initialize segments & interrupt table
 */


#define	GNULL_SEL	0	/* Null Descriptor */
#define	GCODE_SEL	1	/* Kernel Code Descriptor */
#define	GDATA_SEL	2	/* Kernel Data Descriptor */
#define	GLDT_SEL	3	/* LDT - eventually one per process */
#define	GTGATE_SEL	4	/* Process task switch gate */
#define	GPANIC_SEL	5	/* Task state to consider panic from */
#define	GPROC0_SEL	6	/* Task state process slot zero and up */
/*
 * S413: one TSS per CPU.  CPU 0 keeps slot GPROC0_SEL (proc0's u-area, the
 * TSS init386 has always used); AP n gets GPROC0_SEL + n, which is also the
 * value smp.h's cpu_id() decodes back out of `str`.
 * S413：每个 CPU 一个 TSS。CPU0 仍用 GPROC0_SEL（proc0 的 u 区，init386 一直
 * 用的那个）；AP n 用 GPROC0_SEL+n，smp.h 的 cpu_id() 正是从 `str` 的这个选择子
 * 反解出 CPU 号。
 */
#define	NGDT 	(GPROC0_SEL + MAXCPU)

union descriptor gdt[NGDT];

/* interrupt descriptor table */
struct gate_descriptor idt[256];

/* local descriptor table */
union descriptor ldt[5];
#define	LSYS5CALLS_SEL	0	/* forced by intel BCS */
#define	LSYS5SIGR_SEL	1

#define	L43BSDCALLS_SEL	2	/* notyet */
#define	LUCODE_SEL	3
#define	LUDATA_SEL	4
/* seperate stack, es,fs,gs sels ? */
/* #define	LPOSIXCALLS_SEL	5	/* notyet */

struct	i386tss	tss, panic_tss;

extern  struct user *proc0paddr;

/* software prototypes -- in more palitable form */
struct soft_segment_descriptor gdt_segs[] = {
	/* Null Descriptor */
{	0x0,			/* segment base address  */
	0x0,			/* length - all address space */
	0,			/* segment type */
	0,			/* segment descriptor priority level */
	0,			/* segment descriptor present */
	0,0,
	0,			/* default 32 vs 16 bit size */
	0  			/* limit granularity (byte/page units)*/ },
	/* Code Descriptor for kernel */
{	0x0,			/* segment base address  */
	0xfffff,		/* length - all address space */
	SDT_MEMERA,		/* segment type */
	0,			/* segment descriptor priority level */
	1,			/* segment descriptor present */
	0,0,
	1,			/* default 32 vs 16 bit size */
	1  			/* limit granularity (byte/page units)*/ },
	/* Data Descriptor for kernel */
{	0x0,			/* segment base address  */
	0xfffff,		/* length - all address space */
	SDT_MEMRWA,		/* segment type */
	0,			/* segment descriptor priority level */
	1,			/* segment descriptor present */
	0,0,
	1,			/* default 32 vs 16 bit size */
	1  			/* limit granularity (byte/page units)*/ },
	/* LDT Descriptor */
{	(int) ldt,			/* segment base address  */
	sizeof(ldt)-1,		/* length - all address space */
	SDT_SYSLDT,		/* segment type */
	0,			/* segment descriptor priority level */
	1,			/* segment descriptor present */
	0,0,
	0,			/* unused - default 32 vs 16 bit size */
	0  			/* limit granularity (byte/page units)*/ },
	/* Null Descriptor - Placeholder */
{	0x0,			/* segment base address  */
	0x0,			/* length - all address space */
	0,			/* segment type */
	0,			/* segment descriptor priority level */
	0,			/* segment descriptor present */
	0,0,
	0,			/* default 32 vs 16 bit size */
	0  			/* limit granularity (byte/page units)*/ },
	/* Panic Tss Descriptor */
{	(int) &panic_tss,		/* segment base address  */
	sizeof(tss)-1,		/* length - all address space */
	SDT_SYS386TSS,		/* segment type */
	0,			/* segment descriptor priority level */
	1,			/* segment descriptor present */
	0,0,
	0,			/* unused - default 32 vs 16 bit size */
	0  			/* limit granularity (byte/page units)*/ },
	/* Proc 0 Tss Descriptor */
{	(int) kstack,			/* segment base address  */
	sizeof(tss)-1,		/* length - all address space */
	SDT_SYS386TSS,		/* segment type */
	0,			/* segment descriptor priority level */
	1,			/* segment descriptor present */
	0,0,
	0,			/* unused - default 32 vs 16 bit size */
	0  			/* limit granularity (byte/page units)*/ }};

struct soft_segment_descriptor ldt_segs[] = {
	/* Null Descriptor - overwritten by call gate */
{	0x0,			/* segment base address  */
	0x0,			/* length - all address space */
	0,			/* segment type */
	0,			/* segment descriptor priority level */
	0,			/* segment descriptor present */
	0,0,
	0,			/* default 32 vs 16 bit size */
	0  			/* limit granularity (byte/page units)*/ },
	/* Null Descriptor - overwritten by call gate */
{	0x0,			/* segment base address  */
	0x0,			/* length - all address space */
	0,			/* segment type */
	0,			/* segment descriptor priority level */
	0,			/* segment descriptor present */
	0,0,
	0,			/* default 32 vs 16 bit size */
	0  			/* limit granularity (byte/page units)*/ },
	/* Null Descriptor - overwritten by call gate */
{	0x0,			/* segment base address  */
	0x0,			/* length - all address space */
	0,			/* segment type */
	0,			/* segment descriptor priority level */
	0,			/* segment descriptor present */
	0,0,
	0,			/* default 32 vs 16 bit size */
	0  			/* limit granularity (byte/page units)*/ },
	/* Code Descriptor for user */
{	0x0,			/* segment base address  */
	0xfffff,		/* length - all address space */
	SDT_MEMERA,		/* segment type */
	SEL_UPL,		/* segment descriptor priority level */
	1,			/* segment descriptor present */
	0,0,
	1,			/* default 32 vs 16 bit size */
	1  			/* limit granularity (byte/page units)*/ },
	/* Data Descriptor for user */
{	0x0,			/* segment base address  */
	0xfffff,		/* length - all address space */
	SDT_MEMRWA,		/* segment type */
	SEL_UPL,		/* segment descriptor priority level */
	1,			/* segment descriptor present */
	0,0,
	1,			/* default 32 vs 16 bit size */
	1  			/* limit granularity (byte/page units)*/ } };

/* table descriptors - used to load tables by microp */
struct region_descriptor r_gdt = {
	sizeof(gdt)-1,(char *)gdt
};

struct region_descriptor r_idt = {
	sizeof(idt)-1,(char *)idt
};

/* 中文：在 IDT 的 idx 号表项中填入中断/陷阱门：设置处理函数偏移、内核代码选择子、类型 typ、权限 dpl 并置存在位 */
/* EN: Fill IDT entry idx with an interrupt/trap gate: set the handler offset, kernel code selector, type typ, privilege dpl and present bit */
setidt(idx, func, typ, dpl) char *func; {
	struct gate_descriptor *ip = idt + idx;

	ip->gd_looffset = (int)func;
	ip->gd_selector = 8;
	ip->gd_stkcpy = 0;
	ip->gd_xx = 0;
	ip->gd_type = typ;
	ip->gd_dpl = dpl;
	ip->gd_p = 1;
	ip->gd_hioffset = ((int)func)>>16 ;
}

/*
 * S413: install the TSS descriptor for CPU `id`.
 *
 * CPU 0 keeps the descriptor init386 built over proc0's u-area; an AP calls
 * this once, before its first ltr, with its own TSS.  The descriptor has to
 * live in the GDT the AP has already loaded (r_gdt covers NGDT slots), so
 * installing it after lgdt() is enough - the GDT itself is shared.
 * S413：为 CPU id 安装 TSS 描述符。CPU0 沿用 init386 基于 proc0 u 区建好的
 * 那个；AP 在第一次 ltr 前调用本函数填自己的 TSS。AP 此时已加载 r_gdt
 * （覆盖 NGDT 个槽），所以安装后立刻可用——GDT 本身是全核共享的。
 */
void
smp_gdt_set_tss(int id, struct i386tss *t)
{
	struct soft_segment_descriptor ssd;
	char *p = (char *)&ssd;
	int i;
	extern ssdtosd();

	if (id < 0 || id >= MAXCPU || t == 0)
		return;
	for (i = 0; i < (int)sizeof(ssd); i++)
		p[i] = 0;
	ssd.ssd_base = (unsigned)t;
	ssd.ssd_limit = sizeof(struct i386tss) - 1;
	ssd.ssd_type = SDT_SYS386TSS;
	ssd.ssd_dpl = SEL_KPL;
	ssd.ssd_p = 1;
	ssd.ssd_def32 = 0;		/* TSS is a 16-bit "segment" */
	ssd.ssd_gran = 0;
	ssdtosd(&ssd, &gdt[GPROC0_SEL + id]);
}

#define	IDTVEC(name)	__CONCAT(X, name)
extern	IDTVEC(div), IDTVEC(dbg), IDTVEC(nmi), IDTVEC(bpt), IDTVEC(ofl),
	IDTVEC(bnd), IDTVEC(ill), IDTVEC(dna), IDTVEC(dble), IDTVEC(fpusegm),
	IDTVEC(tss), IDTVEC(missing), IDTVEC(stk), IDTVEC(prot),
	IDTVEC(page), IDTVEC(rsvd), IDTVEC(fpu), IDTVEC(rsvd0),
	IDTVEC(rsvd1), IDTVEC(rsvd2), IDTVEC(rsvd3), IDTVEC(rsvd4),
	IDTVEC(rsvd5), IDTVEC(rsvd6), IDTVEC(rsvd7), IDTVEC(rsvd8),
	IDTVEC(rsvd9), IDTVEC(rsvd10), IDTVEC(rsvd11), IDTVEC(rsvd12),
	IDTVEC(rsvd13), IDTVEC(rsvd14), IDTVEC(rsvd14), IDTVEC(syscall);

int lcr0(), lcr3(), rcr0(), rcr2();
int _udatasel, _ucodesel, _gsel_tss;

/* 中文：初始化 386 保护模式运行环境：初始化控制台、构造 GDT/LDT 段与 IDT 异常门、加载表寄存器、读取 BIOS 内存大小、引导 pmap 并切换到用户态 */
/* EN: Initialize the 386 protected-mode environment: set up console, build GDT/LDT segments and IDT exception gates, load table registers, read BIOS memory size, bootstrap pmap and prepare user-mode entry */
init386(first) { extern ssdtosd(), lgdt(), lidt(), lldt(), etext; 
	MTRACE('1');
	int x, *pi;
	unsigned biosbasemem, biosextmem, ext16;
	struct gate_descriptor *gdp;
	extern int sigcode,szsigcode;

	proc0.p_addr = proc0paddr;

	/*
	 * Initialize the console before we print anything out.
	 */

	MTRACE('2');
	cninit (KERNBASE+0xa0000);
	MTRACE('3');

	/* make gdt memory segments */
	gdt_segs[GCODE_SEL].ssd_limit = btoc((int) &etext + NBPG);
	for (x=0; x <= GPROC0_SEL; x++) ssdtosd(gdt_segs+x, gdt+x);
	/*
	 * S413: the per-CPU TSS slots (GPROC0_SEL+1 .. NGDT-1) start out
	 * not-present.  cpu_tss_install() fills one in when an AP loads its
	 * own TSS; leaving them valid-but-empty would let a stray ltr pick up
	 * a TSS whose base is 0.
	 * S413：AP 的 TSS 槽位（GPROC0_SEL+1..NGDT-1）先置为 not-present，
	 * 等 cpu_tss_install() 在 AP 加载自己的 TSS 时再填；否则一次误 ltr 会
	 * 选中基址为 0 的 TSS。
	 */
	{
		char *p = (char *)&gdt[GPROC0_SEL + 1];
		int n = (int)sizeof(union descriptor) * (NGDT - GPROC0_SEL - 1);

		while (n-- > 0)
			*p++ = 0;
	}
	/* make ldt memory segments */
	ldt_segs[LUCODE_SEL].ssd_limit = btoc(UPT_MIN_ADDRESS);
	ldt_segs[LUDATA_SEL].ssd_limit = btoc(UPT_MIN_ADDRESS);
	/* Note. eventually want private ldts per process */
	for (x=0; x < 5; x++) ssdtosd(ldt_segs+x, ldt+x);
	MTRACE('g');

	/* exceptions */
	setidt(0, &IDTVEC(div),  SDT_SYS386TGT, SEL_KPL);
	setidt(1, &IDTVEC(dbg),  SDT_SYS386TGT, SEL_KPL);
	setidt(2, &IDTVEC(nmi),  SDT_SYS386TGT, SEL_KPL);
 	setidt(3, &IDTVEC(bpt),  SDT_SYS386TGT, SEL_UPL);
	setidt(4, &IDTVEC(ofl),  SDT_SYS386TGT, SEL_KPL);
	setidt(5, &IDTVEC(bnd),  SDT_SYS386TGT, SEL_KPL);
	setidt(6, &IDTVEC(ill),  SDT_SYS386TGT, SEL_KPL);
	setidt(7, &IDTVEC(dna),  SDT_SYS386TGT, SEL_KPL);
	setidt(8, &IDTVEC(dble),  SDT_SYS386TGT, SEL_KPL);
	setidt(9, &IDTVEC(fpusegm),  SDT_SYS386TGT, SEL_KPL);
	setidt(10, &IDTVEC(tss),  SDT_SYS386TGT, SEL_KPL);
	setidt(11, &IDTVEC(missing),  SDT_SYS386TGT, SEL_KPL);
	setidt(12, &IDTVEC(stk),  SDT_SYS386TGT, SEL_KPL);
	setidt(13, &IDTVEC(prot),  SDT_SYS386TGT, SEL_KPL);
	setidt(14, &IDTVEC(page),  SDT_SYS386TGT, SEL_KPL);
	setidt(15, &IDTVEC(rsvd),  SDT_SYS386TGT, SEL_KPL);
	setidt(16, &IDTVEC(fpu),  SDT_SYS386TGT, SEL_KPL);
	setidt(17, &IDTVEC(rsvd0),  SDT_SYS386TGT, SEL_KPL);
	setidt(18, &IDTVEC(rsvd1),  SDT_SYS386TGT, SEL_KPL);
	setidt(19, &IDTVEC(rsvd2),  SDT_SYS386TGT, SEL_KPL);
	setidt(20, &IDTVEC(rsvd3),  SDT_SYS386TGT, SEL_KPL);
	setidt(21, &IDTVEC(rsvd4),  SDT_SYS386TGT, SEL_KPL);
	setidt(22, &IDTVEC(rsvd5),  SDT_SYS386TGT, SEL_KPL);
	setidt(23, &IDTVEC(rsvd6),  SDT_SYS386TGT, SEL_KPL);
	setidt(24, &IDTVEC(rsvd7),  SDT_SYS386TGT, SEL_KPL);
	setidt(25, &IDTVEC(rsvd8),  SDT_SYS386TGT, SEL_KPL);
	setidt(26, &IDTVEC(rsvd9),  SDT_SYS386TGT, SEL_KPL);
	setidt(27, &IDTVEC(rsvd10),  SDT_SYS386TGT, SEL_KPL);
	setidt(28, &IDTVEC(rsvd11),  SDT_SYS386TGT, SEL_KPL);
	setidt(29, &IDTVEC(rsvd12),  SDT_SYS386TGT, SEL_KPL);
	setidt(30, &IDTVEC(rsvd13),  SDT_SYS386TGT, SEL_KPL);
	setidt(31, &IDTVEC(rsvd14),  SDT_SYS386TGT, SEL_KPL);
	MTRACE('i');

#include "isa.h"
#if	NISA >0
	isa_defaultirq();
#endif

	lgdt(&r_gdt);
	lidt(&r_idt);
	lldt(GSEL(GLDT_SEL, SEL_KPL));
	MTRACE('l');

	/*
	 * This memory size stuff is a real mess.  Here is a simple
	 * setup that just believes the BIOS.  After the rest of
	 * the system is a little more stable, we'll come back to
	 * this and deal with issues if incorrect BIOS information,
	 * and when physical memory is > 16 megabytes.
	 *
	 * S408: the "> 16 megabytes" case is now handled.  CMOS 0x17/0x18
	 * (like 0x30/0x31) saturate at 64 MB - 1 KB, so a VMware guest given
	 * 512 MB reported 63.5 MB and page_alloc()'s dynamic ceiling could
	 * never grow past it.  There is no CMOS register that describes a
	 * bigger machine (VMware leaves 0x34/0x35 as 0x01D0, which is not a
	 * memory size at all), so stage 1 asks the BIOS with INT 15h AX=E801
	 * while it is still in real mode and leaves the answer at 0x9000:
	 * magic "MEM1", then one word with AX (KB between 1 MB and 16 MB) in
	 * the low half and BX (64 KB blocks above 16 MB) in the high half.
	 * Reading low physical memory is safe here - locore has already
	 * identity-mapped 0.._end at KERNBASE and this runs before
	 * pmap_bootstrap().
	 * 内存探测：0x17/0x18 在 64MB-1KB 处饱和，512MB 的 VMware guest 只报
	 * 63.5MB，page_alloc 的动态上界再对也涨不上去。CMOS 里没有能描述更大机器
	 * 的寄存器（VMware 的 0x34/0x35 是 0x01D0，根本不是内存大小），所以由
	 * stage1 在实模式下用 INT 15h AX=E801 问 BIOS，把答案留在 0x9000：魔数
	 * "MEM1"，随后一个字，低半为 AX（1MB~16MB 的 KB 数），高半为 BX（16MB
	 * 以上的 64KB 块数）。此时 locore 已把 0.._end 恒等映射到 KERNBASE，且
	 * 还没进 pmap_bootstrap，读低端物理内存是安全的。
	 */
	biosbasemem = rtcin(RTC_BASELO)+ (rtcin(RTC_BASEHI)<<8);
	biosextmem = rtcin(RTC_EXTLO)+ (rtcin(RTC_EXTHI)<<8);
	ext16 = 0;
	if (*(volatile unsigned *)(KERNBASE + 0x9000) == 0x4D454D31) {
		unsigned mw = *(volatile unsigned *)(KERNBASE + 0x9004);

		ext16 = mw & 0xffff;		/* AX: KB in 1 MB..16 MB */
		/* total pages = (1024 + AX) KB / 4 KB + BX * 16 pages */
		Maxmem = (1024 + ext16) / 4 + (mw >> 16) * 16;
		if (Maxmem < 4096)		/* < 16 MB: bogus, use CMOS */
			Maxmem = 0;
	}
	if (Maxmem != 0) {
		maxmem = Maxmem - 1;
		physmem = Maxmem - 1;		/* top page is BIOS-reserved */
	} else {
		Maxmem = btoc ((biosextmem + 1024) * 1024);
		maxmem = Maxmem - 1;
		physmem = btoc (biosbasemem * 1024 + (biosextmem - 1) * 1024);
	}
	printf ("bios %dK+%dK e801 %dK. maxmem %x, physmem %x\n",
		biosbasemem, biosextmem, ext16, ctob (maxmem),
		ctob (physmem));

	MTRACE('4');
	vm_set_page_size();
	MTRACE('5');
	/* call pmap initialization to make new kernel address space */
	MTRACE('6');
	pmap_bootstrap (first, 0);
	MTRACE('7');
	/* now running on new page tables, configured,and u/iom is accessible */

	/* make a initial tss so microp can get interrupt stack on syscall! */
	proc0.p_addr->u_pcb.pcb_tss.tss_esp0 = (int) kstack + UPAGES*NBPG;
	proc0.p_addr->u_pcb.pcb_tss.tss_ss0 = GSEL(GDATA_SEL, SEL_KPL) ;
	_gsel_tss = GSEL(GPROC0_SEL, SEL_KPL);
	ltr(_gsel_tss);

	/* make a call gate to reenter kernel with */
	gdp = &ldt[LSYS5CALLS_SEL].gd;
	
	x = (int) &IDTVEC(syscall);
	gdp->gd_looffset = x++;
	gdp->gd_selector = GSEL(GCODE_SEL,SEL_KPL);
	gdp->gd_stkcpy = 0;
	gdp->gd_type = SDT_SYS386CGT;
	gdp->gd_dpl = SEL_UPL;
	gdp->gd_p = 1;
	gdp->gd_hioffset = ((int) &IDTVEC(syscall)) >>16;

	/* transfer to user mode */

	_ucodesel = LSEL(LUCODE_SEL, SEL_UPL);
	_udatasel = LSEL(LUDATA_SEL, SEL_UPL);

	/* setup proc 0's pcb */
	bcopy(&sigcode, proc0.p_addr->u_pcb.pcb_sigc, szsigcode);
	proc0.p_addr->u_pcb.pcb_flags = 0;
	proc0.p_addr->u_pcb.pcb_ptd = IdlePTD;
}

extern struct pte	*CMAP1, *CMAP2;
extern caddr_t		CADDR1, CADDR2;
/*
 * zero out physical memory
 * specified in relocation units (NBPG bytes)
 */
/* 中文：通过 CMAP2 映射指定物理页并清零一页，用于清零以 NBPG 为单位给出的物理内存段 */
/* EN: Map a physical page through CMAP2 and zero one page, used to clear a physical memory segment given in NBPG relocation units */
clearseg(n) {

	*(int *)CMAP2 = PG_V | PG_KW | ctob(n);
	load_cr3(rcr3());
	bzero(CADDR2,NBPG);
	*(int *) CADDR2 = 0;
}

/*
 * copy a page of physical memory
 * specified in relocation units (NBPG bytes)
 */
/* 中文：通过 CMAP2 映射目标物理页，把以 NBPG 为单位给出的源物理页内容复制一页过去 */
/* EN: Map the destination physical page through CMAP2 and copy one page from the source physical page given in NBPG relocation units */
copyseg(frm, n) {

	*(int *)CMAP2 = PG_V | PG_KW | ctob(n);
	load_cr3(rcr3());
	bcopy((void *)frm, (void *)CADDR2, NBPG);
}

/*
 * copy a page of physical memory
 * specified in relocation units (NBPG bytes)
 */
/* 中文：通过 CMAP1 与 CMAP2 分别映射源、目的物理页，把一页物理内存从 frm 复制到 to */
/* EN: Map source and destination physical pages through CMAP1 and CMAP2 respectively and copy one page of physical memory from frm to to */
physcopyseg(frm, to) {

	*(int *)CMAP1 = PG_V | PG_KW | ctob(frm);
	*(int *)CMAP2 = PG_V | PG_KW | ctob(to);
	load_cr3(rcr3());
	bcopy(CADDR1, CADDR2, NBPG);
}

/*aston() {
	schednetisr(NETISR_AST);
}*/

/* 中文：调度软件时钟中断，向 netisr 提交 NETISR_SCLK */
/* EN: Schedule the software clock interrupt by posting NETISR_SCLK to the netisr queue */
setsoftclock() {
	schednetisr(NETISR_SCLK);
}

/*
 * insert an element into a queue 
 */
#undef insque
/* EN: Inserts element into the doubly linked prochd queue immediately after head, patching the successor's ph_rlink too; the body takes no locks or interrupt masking. */
/* 中文：把 element 插入以 head 为头的双向 prochd 队列的头节点之后，并修补后继节点的 ph_rlink；函数体不加锁也不屏蔽中断。 */
_insque(element, head)
	register struct prochd *element, *head;
{
	element->ph_link = head->ph_link;
	head->ph_link = (struct process *)element;
	element->ph_rlink = (struct process *)head;
	((struct prochd *)(element->ph_link))->ph_rlink=(struct process *)element;
}

/*
 * remove an element from a queue
 */
#undef remque
/* EN: Unlinks element from the doubly linked prochd queue by fixing its neighbours' ph_link/ph_rlink, then clears element->ph_rlink; no locking is performed. */
/* 中文：把 element 从双向 prochd 队列中摘除：修补前后节点的 ph_link/ph_rlink，再把 element->ph_rlink 清零；不做加锁。 */
_remque(element)
	register struct prochd *element;
{
	((struct prochd *)(element->ph_link))->ph_rlink = element->ph_rlink;
	((struct prochd *)(element->ph_rlink))->ph_link = element->ph_link;
	element->ph_rlink = (struct process *)0;
}

/* 中文：释放虚拟内存访问的空实现占位函数，本移植版中无需实际操作 */
/* EN: No-op placeholder for releasing virtual memory access; requires no real work in this port */
vmunaccess() {}

/*
 * Below written in C to allow access to debugging code
 */
/* 中文：以 fubyte 从用户态逐字节复制 NUL 结尾字符串到内核，最多 maxlength 字节，成功返回 0，出错返回 EFAULT/ENAMETOOLONG 并可选回写复制字节数 */
/* EN: Copy a NUL-terminated string from user space to the kernel byte by byte via fubyte, up to maxlength bytes; returns 0 on success or EFAULT/ENAMETOOLONG, optionally writing back the copied count */
copyinstr(fromaddr, toaddr, maxlength, lencopied) u_int *lencopied, maxlength;
	void *toaddr, *fromaddr; {
	int c,tally;

	tally = 0;
	while (maxlength--) {
		c = fubyte(fromaddr++);
		if (c == -1) {
			if(lencopied) *lencopied = tally;
			return(EFAULT);
		}
		tally++;
		*(char *)toaddr++ = (char) c;
		if (c == 0){
			if(lencopied) *lencopied = tally;
			return(0);
		}
	}
	if(lencopied) *lencopied = tally;
	return(ENAMETOOLONG);
}

/* 中文：以 subyte 从内核逐字节复制 NUL 结尾字符串到用户态，最多 maxlength 字节，成功返回 0，出错返回 EFAULT/ENAMETOOLONG 并可选回写复制字节数 */
/* EN: Copy a NUL-terminated string from the kernel to user space byte by byte via subyte, up to maxlength bytes; returns 0 on success or EFAULT/ENAMETOOLONG, optionally writing back the copied count */
copyoutstr(fromaddr, toaddr, maxlength, lencopied) u_int *lencopied, maxlength;
	void *fromaddr, *toaddr; {
	int c;
	int tally;

	tally = 0;
	while (maxlength--) {
		c = subyte(toaddr++, *(char *)fromaddr);
		if (c == -1) return(EFAULT);
		tally++;
		if (*(char *)fromaddr++ == 0){
			if(lencopied) *lencopied = tally;
			return(0);
		}
	}
	if(lencopied) *lencopied = tally;
	return(ENAMETOOLONG);
}

/* 中文：在内核地址空间之间以 C 实现复制 NUL 结尾字符串，最多 maxlength 字节，成功返回 0，超长返回 ENAMETOOLONG 并可选回写复制字节数 */
/* EN: Copy a NUL-terminated string between kernel addresses in C, up to maxlength bytes; returns 0 on success or ENAMETOOLONG on overflow, optionally writing back the copied count */
copystr(fromaddr, toaddr, maxlength, lencopied) u_int *lencopied, maxlength;
	void *fromaddr, *toaddr; {
	u_int tally;

	tally = 0;
	while (maxlength--) {
		*(u_char *)toaddr = *(u_char *)fromaddr++;
		tally++;
		if (*(u_char *)toaddr++ == 0) {
			if(lencopied) *lencopied = tally;
			return(0);
		}
	}
	if(lencopied) *lencopied = tally;
	return(ENAMETOOLONG);
}
