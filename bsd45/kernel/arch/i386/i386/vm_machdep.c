/*-
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department, and William Jolitz.
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
 *	@(#)vm_machdep.c	8.3 (Berkeley) 1/21/94
 *	Utah $Hdr: vm_machdep.c 1.16.1.1 89/06/23$
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kprocess.h>
#include <sys/malloc.h>
#include <sys/buf.h>
#include <sys/vnode.h>
#include <sys/user.h>

#include <machine/cpu.h>

#include <vm/vm.h>
#include <vm/vm_kern.h>

/*
 * Finish a fork operation, with process p2 nearly set up.
 * Copy and update the kernel stack and pcb, making the child
 * ready to run, and marking it so that it can return differently
 * than the parent.  Returns 1 in the child process, 0 in the parent.
 * We currently double-map the user area so that the stack is at the same
 * address in each process; in the future we will probably relocate
 * the frame pointers on the stack after copying.
 */
/* EN: Copies p1's PCB and live kernel stack into p2's u area, maps the child's UPAGES to its kernel stack with pmap_enter, activates the child pmap; returns 1 in the child via savectx. */
/* 中文：把 p1 的 PCB 和正在使用的内核栈复制到 p2 的 u 区，用 pmap_enter 把子进程的 UPAGES 映射到其内核栈并激活子进程 pmap，子进程经 savectx 返回 1。 */
cpu_fork(p1, p2)
	register struct process *p1, *p2;
{
	register struct user *up = p2->p_addr;
	int foo, offset, addr, i;
	extern char kstack[];
	extern int mvesp();

	/*
	 * Copy pcb and stack from proc p1 to p2. 
	 * We do this as cheaply as possible, copying only the active
	 * part of the stack.  The stack and pcb need to agree;
	 * this is tricky, as the final pcb is constructed by savectx,
	 * but its frame isn't yet on the stack when the stack is copied.
	 * mi_switch compensates for this when the child eventually runs.
	 * This should be done differently, with a single call
	 * that copies and updates the pcb+stack,
	 * replacing the bcopy and savectx.
	 */
	p2->p_addr->u_pcb = p1->p_addr->u_pcb;
	offset = mvesp() - (int)kstack;
	bcopy((caddr_t)kstack + offset, (caddr_t)p2->p_addr + offset,
	    (unsigned) ctob(UPAGES) - offset);
	p2->p_md.md_regs = p1->p_md.md_regs;

	/*
	 * Wire top of address space of child to it's u.
	 * First, fault in a page of pte's to map it.
	 */
        addr = trunc_page((u_int)vtopte(kstack));
	(void)vm_map_pageable(&p2->p_vmspace->vm_map, addr, addr+NBPG, FALSE);
	for (i=0; i < UPAGES; i++)
		pmap_enter(&p2->p_vmspace->vm_pmap, (vm_offset_t)kstack+i*NBPG,
			pmap_extract(kernel_pmap, ((int)p2->p_addr)+i*NBPG),
			VM_PROT_READ, 1);
	pmap_activate(&p2->p_vmspace->vm_pmap, &up->u_pcb);

	/*
	 * 
	 * Arrange for a non-local goto when the new process
	 * is started, to resume here, returning nonzero from setjmp.
	 */
	if (savectx(up, 1)) {
		/*
		 * Return 1 in child.
		 */
		return (1);
	}
	return (0);
}

#include "npx.h"
#if NNPX > 0
extern struct process *npxproc;
#endif

#ifdef notyet
/*
 * cpu_exit is called as the last action during exit.
 *
 * We change to an inactive address space and a "safe" stack,
 * passing thru an argument to the new stack. Now, safely isolated
 * from the resources we're shedding, we release the address space
 * and any remaining machine-dependent resources, including the
 * memory for the user structure and kernel stack.
 *
 * Next, we assign a dummy context to be written over by mi_switch,
 * calling it to send this process off to oblivion.
 * [The nullpcb allows us to minimize cost in mi_switch() by not having
 * a special case].
 */
struct process *switch_to_inactive();
/* EN: Active definition: releases the coprocessor when p owns it (NNPX), sets curproc = p and mi_switch()es away; vmspace and u area are freed later by cpu_wait(). */
/* 中文：第二个即实际编译的定义：当 p 占用协处理器时释放它（NNPX），置 curproc = p 后调用 mi_switch 让出 CPU；vmspace 与 u 区由 cpu_wait() 稍后释放。 */
/* EN: notyet variant only: frees the FPU, switches to an inactive stack, frees vmspace and u area, sets p_addr to nullpcb and mi_switch()es away; never returns. */
/* 中文：文件中第一个定义，仅在 #ifdef notyet 分支编译：释放浮点协处理器，切到非活动栈后释放 vmspace 与 u 区，把 p_addr 指向 nullpcb 再调用 mi_switch 离开，永不返回。 */
cpu_exit(p)
	register struct process *p;
{
	static struct pcb nullpcb;	/* pcb to overwrite on last switch */

#if NNPX > 0
	/* free cporcessor (if we have it) */
	if( p == npxproc) npxproc =0;
#endif

	/* move to inactive space and stack, passing arg accross */
	p = switch_to_inactive(p);

	/* drop per-process resources */
	vmspace_free(p->p_vmspace);
	kmem_free(kernel_map, (vm_offset_t)p->p_addr, ctob(UPAGES));

	p->p_addr = (struct user *) &nullpcb;
	mi_switch();
	/* NOTREACHED */
}
#else
/* EN: Active definition: releases the coprocessor when p owns it (NNPX), sets curproc = p and mi_switch()es away; vmspace and u area are freed later by cpu_wait(). */
/* 中文：第二个即实际编译的定义：当 p 占用协处理器时释放它（NNPX），置 curproc = p 后调用 mi_switch 让出 CPU；vmspace 与 u 区由 cpu_wait() 稍后释放。 */
/* EN: notyet variant only: frees the FPU, switches to an inactive stack, frees vmspace and u area, sets p_addr to nullpcb and mi_switch()es away; never returns. */
/* 中文：文件中第一个定义，仅在 #ifdef notyet 分支编译：释放浮点协处理器，切到非活动栈后释放 vmspace 与 u 区，把 p_addr 指向 nullpcb 再调用 mi_switch 离开，永不返回。 */
cpu_exit(p)
	register struct process *p;
{
	
	/* free coprocessor (if we have it) */
#if NNPX > 0
	if( p == npxproc) npxproc =0;
#endif

	curproc = p;
	mi_switch();
}

cpu_wait(p) struct process *p; {

	/* drop per-process resources */
	vmspace_free(p->p_vmspace);
	kmem_free(kernel_map, (vm_offset_t)p->p_addr, ctob(UPAGES));
}
#endif

/*
 * Dump the machine specific header information at the start of a core dump.
 */
/* EN: Writes the process u-area (p->p_addr, ctob(UPAGES) bytes at offset 0) to vnode vp via vn_rdwr with IO_NODELOCKED|IO_UNIT and returns that error. */
/* 中文：用 vn_rdwr 把进程 u 区（p->p_addr，偏移 0，ctob(UPAGES) 字节）写入 vnode vp，标志 IO_NODELOCKED|IO_UNIT，并返回其错误码。 */
cpu_coredump(p, vp, cred)
	struct process *p;
	struct vnode *vp;
	struct ucred *cred;
{

	return (vn_rdwr(UIO_WRITE, vp, (caddr_t) p->p_addr, ctob(UPAGES),
	    (off_t)0, UIO_SYSSPACE, IO_NODELOCKED|IO_UNIT, cred, (int *)NULL,
	    p));
}

/*
 * Set a red zone in the kernel stack after the u. area.
 */
/* EN: Empty stub with no statements: the comment says the kernel-stack red zone below the u-area should someday use an expand-down ss0 stack segment. */
/* 中文：空函数桩，函数体没有任何语句；注释说明日后应为 ss0 建立向下扩展的栈段，从而在 u 区下方设置内核栈红区。 */
setredzone(pte, vaddr)
	u_short *pte;
	caddr_t vaddr;
{
/* eventually do this by setting up an expand-down stack segment
   for ss0: selector, allowing stack access down to top of u.
   this means though that protection violations need to be handled
   thru a double fault exception that must do an integral task
   switch to a known good context, within which a dump can be
   taken. a sensible scheme might be to save the initial context
   used by sched (that has physical memory mapped 1:1 at bottom)
   and take the dump while still in mapped mode */
}

/*
 * Move pages from one kernel virtual address to another.
 * Both addresses are assumed to reside in the Sysmap,
 * and size must be a multiple of CLSIZE.
 */
/* EN: Moves Sysmap pages from from to to by copying PTEs in NBPG steps and zeroing each source PTE, then flushes the TLB; panics unless size is a CLBYTES multiple. */
/* 中文：在 Sysmap 内把页从 from 搬到 to：按 NBPG 步长复制页表项并把源 PTE 清零，最后刷新 TLB；size 不是 CLBYTES 整数倍时 panic。 */
pagemove(from, to, size)
	register caddr_t from, to;
	int size;
{
	register struct pte *fpte, *tpte;

	if (size % CLBYTES)
		panic("pagemove");
	fpte = kvtopte(from);
	tpte = kvtopte(to);
	while (size > 0) {
		*tpte++ = *fpte;
		*(int *)fpte++ = 0;
		from += NBPG;
		to += NBPG;
		size -= NBPG;
	}
	tlbflush();
}

/*
 * Convert kernel VA to physical address
 */
/* EN: Converts a kernel virtual address to physical via pmap_extract on kernel_pmap, panicking with 'kvtop: zero page frame' when no mapping exists. */
/* 中文：通过 pmap_extract 在 kernel_pmap 中把内核虚拟地址转换为物理地址；映射不存在时以 'kvtop: zero page frame' 触发 panic。 */
kvtop(addr)
	register caddr_t addr;
{
	vm_offset_t va;

	va = pmap_extract(kernel_pmap, (vm_offset_t)addr);
	if (va == 0)
		panic("kvtop: zero page frame");
	return((int)va);
}

#ifdef notdef
/*
 * The probe[rw] routines should probably be redone in assembler
 * for efficiency.
 */
/* EN: Read probe (compiled out under #ifdef notdef): returns 0 at or above USRSTACK, otherwise 1 when btop(addr) falls outside dptov(p_dsize)..sptov(p_ssize). */
/* 中文：只读用户地址探测（位于 #ifdef notdef 内）：addr >= USRSTACK 返回 0，否则当 btop(addr) 落在 dptov(p_dsize)..sptov(p_ssize) 之外时返回 1。 */
prober(addr)
	register u_int addr;
{
	register int page;
	register struct process *p;

	if (addr >= USRSTACK)
		return(0);
	p = u.u_procp;
	page = btop(addr);
	if (page < dptov(p, p->p_dsize) || page > sptov(p, p->p_ssize))
		return(1);
	return(0);
}

/* EN: Write probe like prober, but for pages inside the data..stack range it also requires the vtopte PTE to carry PG_UW user-write permission before returning 1. */
/* 中文：与 prober 类似的写探测：对落在 dptov..sptov 区间内的页，还要求 vtopte 取出的页表项带有 PG_UW 用户可写权限才返回 1。 */
probew(addr)
	register u_int addr;
{
	register int page;
	register struct process *p;

	if (addr >= USRSTACK)
		return(0);
	p = u.u_procp;
	page = btop(addr);
	if (page < dptov(p, p->p_dsize) || page > sptov(p, p->p_ssize))
		return((*(int *)vtopte(p, page) & PG_PROT) == PG_UW);
	return(0);
}

/*
 * NB: assumes a physically contiguous kernel page table
 *     (makes life a LOT simpler).
 */
/* EN: Checks that count bytes of kernel space at addr are mapped, requiring valid PDEs and valid Sysmap PTEs bounded by Syssize; returns 1 only if all are valid, rw is ignored. */
/* 中文：检查内核地址 addr 起 count 字节是否已映射：要求相关 PDE 与 Sysmap 中的 PTE 全部有效且不越过 Syssize，全部有效才返回 1，rw 参数被忽略。 */
kernacc(addr, count, rw)
	register u_int addr;
	int count, rw;
{
	register struct pde *pde;
	register struct pte *pte;
	register int ix, cnt;
	extern long Syssize;

	if (count <= 0)
		return(0);
	pde = (struct pde *)((u_int)u.u_procp->p_p0br + u.u_procp->p_szpt * NBPG);
	ix = (addr & PD_MASK) >> PD_SHIFT;
	cnt = ((addr + count + (1 << PD_SHIFT) - 1) & PD_MASK) >> PD_SHIFT;
	cnt -= ix;
	for (pde += ix; cnt; cnt--, pde++)
		if (pde->pd_v == 0)
			return(0);
	ix = btop(addr-0xfe000000);
	cnt = btop(addr-0xfe000000+count+NBPG-1);
	if (cnt > (int)&Syssize)
		return(0);
	cnt -= ix;
	for (pte = &Sysmap[ix]; cnt; cnt--, pte++)
		if (pte->pg_v == 0 /*|| (rw == B_WRITE && pte->pg_prot == 1)*/) 
			return(0);
	return(1);
}

/* EN: Checks a user range page by page using prober for B_READ or probew otherwise, returning 1 only when every page-aligned probe succeeds, 0 if any fails or count <= 0. */
/* 中文：按页检查用户区间：B_READ 用 prober、其余用 probew；所有页对齐的探测都成功才返回 1，任一失败或 count <= 0 返回 0。 */
useracc(addr, count, rw)
	register u_int addr;
	int count, rw;
{
	register int (*func)();
	register u_int addr2;
	extern int prober(), probew();

	if (count <= 0)
		return(0);
	addr2 = addr;
	addr += count;
	func = (rw == B_READ) ? prober : probew;
	do {
		if ((*func)(addr2) == 0)
			return(0);
		addr2 = (addr2 + NBPG) & ~PGOFSET;
	} while (addr2 < addr);
	return(1);
}
#endif

extern vm_map_t phys_map;

/*
 * Map an IO request into kernel virtual address space.  Requests fall into
 * one of five catagories:
 *
 *	B_PHYS|B_UAREA:	User u-area swap.
 *			Address is relative to start of u-area (p_addr).
 *	B_PHYS|B_PAGET:	User page table swap.
 *			Address is a kernel VA in usrpt (Usrptmap).
 *	B_PHYS|B_DIRTY:	Dirty page push.
 *			Address is a VA in proc2's address space.
 *	B_PHYS|B_PGIN:	Kernel pagein of user pages.
 *			Address is VA in user's address space.
 *	B_PHYS:		User "raw" IO request.
 *			Address is VA in user's address space.
 *
 * All requests are (re)mapped into kernel VA space via the useriomap
 * (a name with only slightly more meaning than "kernelmap")
 */
/* EN: Maps a B_PHYS buffer's user pages into phys_map: saves b_saveaddr, kmem_alloc_wait's npf pages, then pmap_enter's each extracted page RW and wired; panics on a null frame. */
/* 中文：把 B_PHYS 缓冲区的用户页映射进 phys_map：保存 b_saveaddr，用 kmem_alloc_wait 分配 npf 页，再对每个 pmap_extract 得到的物理页以读写、wired 方式 pmap_enter；空页框则 panic。 */
vmapbuf(bp)
	register struct buf *bp;
{
	register int npf;
	register caddr_t addr;
	register long flags = bp->b_flags;
	struct process *p;
	int off;
	vm_offset_t kva;
	register vm_offset_t pa;

	if ((flags & B_PHYS) == 0)
		panic("vmapbuf");
	addr = bp->b_saveaddr = bp->b_un.b_addr;
	off = (int)addr & PGOFSET;
	p = bp->b_proc;
	npf = btoc(round_page(bp->b_bcount + off));
	kva = kmem_alloc_wait(phys_map, ctob(npf));
	bp->b_un.b_addr = (caddr_t) (kva + off);
	while (npf--) {
		pa = pmap_extract(&p->p_vmspace->vm_pmap, (vm_offset_t)addr);
		if (pa == 0)
			panic("vmapbuf: null page frame");
		pmap_enter(vm_map_pmap(phys_map), kva, trunc_page(pa),
			   VM_PROT_READ|VM_PROT_WRITE, TRUE);
		addr += PAGE_SIZE;
		kva += PAGE_SIZE;
	}
}

/*
 * Free the io map PTEs associated with this IO operation.
 * We also invalidate the TLB entries and restore the original b_addr.
 */
/* EN: Unmaps vmapbuf's phys_map range with kmem_free_wakeup (waking waiters), restores b_un.b_addr from b_saveaddr and clears b_saveaddr; panics when B_PHYS is not set. */
/* 中文：用 kmem_free_wakeup 释放 vmapbuf 建立的 phys_map 区间并唤醒等待者，把 b_un.b_addr 恢复为 b_saveaddr 后将其清空；未置 B_PHYS 时 panic。 */
vunmapbuf(bp)
	register struct buf *bp;
{
	register int npf;
	register caddr_t addr = bp->b_un.b_addr;
	vm_offset_t kva;

	if ((bp->b_flags & B_PHYS) == 0)
		panic("vunmapbuf");
	npf = btoc(round_page(bp->b_bcount + ((int)addr & PGOFSET)));
	kva = (vm_offset_t)((int)addr & ~PGOFSET);
	kmem_free_wakeup(phys_map, kva, ctob(npf));
	bp->b_un.b_addr = bp->b_saveaddr;
	bp->b_saveaddr = NULL;
}
