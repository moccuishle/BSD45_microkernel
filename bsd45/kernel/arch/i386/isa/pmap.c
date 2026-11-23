/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department and William Jolitz of UUNET Technologies Inc.
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
 *	@(#)pmap.c	8.1 (Berkeley) 6/11/93
 */

/*
 * Derived from hp300 version by Mike Hibler, this version by William
 * Jolitz uses a recursive map [a pde points to the page directory] to
 * map the page tables using the pagetables themselves. This is done to
 * reduce the impact on kernel virtual memory for lots of sparse address
 * space, and to reduce the cost of memory to each process.
 *
 *	Derived from: hp300/@(#)pmap.c	7.1 (Berkeley) 12/5/90
 */

/*
 *	Reno i386 version, from Mike Hibler's hp300 version.
 */

/*
 *	Manages physical address maps.
 *
 *	In addition to hardware address maps, this
 *	module is called upon to provide software-use-only
 *	maps which may or may not be stored in the same
 *	form as hardware maps.  These pseudo-maps are
 *	used to store intermediate results from copy
 *	operations to and from address spaces.
 *
 *	Since the information managed by this module is
 *	also stored by the logical address mapping module,
 *	this module may throw away valid virtual-to-physical
 *	mappings at almost any time.  However, invalidations
 *	of virtual-to-physical mappings must be done as
 *	requested.
 *
 *	In order to cope with hardware architectures which
 *	make virtual-to-physical map invalidates expensive,
 *	this module may delay invalidate or reduced protection
 *	operations until such time as they are actually
 *	necessary.  This module is given full information as
 *	to which processors are currently using which maps,
 *	and to when physical maps must be made correct.
 */

#include <sys/param.h>
#include <sys/kprocess.h>
#include <sys/malloc.h>
#include <sys/user.h>

#include <vm/vm.h>
#include <vm/vm_kern.h>
#include <vm/vm_page.h>

#ifdef NOTDEF
include <vm/vm_pageout.h>
include <machine/isa.h>
#endif

/*
 * Allocate various and sundry SYSMAPs used in the days of old VM
 * and not yet converted.  XXX.
 */
#define BSDVM_COMPAT	1

#ifdef DEBUG
struct {
	int kernel;	/* entering kernel mapping */
	int user;	/* entering user mapping */
	int ptpneeded;	/* needed to allocate a PT page */
	int pwchange;	/* no mapping change, just wiring or protection */
	int wchange;	/* no mapping change, just wiring */
	int mchange;	/* was mapped but mapping to different page */
	int managed;	/* a managed page */
	int firstpv;	/* first mapping for this PA */
	int secondpv;	/* second mapping for this PA */
	int ci;		/* cache inhibited */
	int unmanaged;	/* not a managed page */
	int flushes;	/* cache flushes */
} enter_stats;
struct {
	int calls;
	int removes;
	int pvfirst;
	int pvsearch;
	int ptinvalid;
	int uflushes;
	int sflushes;
} remove_stats;

int debugmap = 0;
int pmapdebug = 0;
#define PDB_FOLLOW	0x0001
#define PDB_INIT	0x0002
#define PDB_ENTER	0x0004
#define PDB_REMOVE	0x0008
#define PDB_CREATE	0x0010
#define PDB_PTPAGE	0x0020
#define PDB_CACHE	0x0040
#define PDB_BITS	0x0080
#define PDB_COLLECT	0x0100
#define PDB_PROTECT	0x0200
#define PDB_PDRTAB	0x0400
#define PDB_PARANOIA	0x2000
#define PDB_WIRING	0x4000
#define PDB_PVDUMP	0x8000

int pmapvacflush = 0;
#define	PVF_ENTER	0x01
#define	PVF_REMOVE	0x02
#define	PVF_PROTECT	0x04
#define	PVF_TOTAL	0x80
#endif

/*
 * Get PDEs and PTEs for user/kernel address space
 */
#define	pmap_pde(m, v)	(&((m)->pm_pdir[((vm_offset_t)(v) >> PD_SHIFT)&1023]))

#define pmap_pte_pa(pte)	(*(int *)(pte) & PG_FRAME)

#define pmap_pde_v(pte)		((pte)->pd_v)
#define pmap_pte_w(pte)		((pte)->pg_w)
/* #define pmap_pte_ci(pte)	((pte)->pg_ci) */
#define pmap_pte_m(pte)		((pte)->pg_m)
#define pmap_pte_u(pte)		((pte)->pg_u)
#define pmap_pte_v(pte)		((pte)->pg_v)
#define pmap_pte_set_w(pte, v)		((pte)->pg_w = (v))
#define pmap_pte_set_prot(pte, v)	((pte)->pg_prot = (v))

/*
 * Given a map and a machine independent protection code,
 * convert to a vax protection code.
 */
#define pte_prot(m, p)	(protection_codes[p])
int	protection_codes[8];

struct pmap	kernel_pmap_store;

vm_offset_t    	avail_start;	/* PA of first available physical page */
vm_offset_t	avail_end;	/* PA of last available physical page */
vm_size_t	mem_size;	/* memory size in bytes */
vm_offset_t	virtual_avail;  /* VA of first avail page (after kernel bss)*/
vm_offset_t	virtual_end;	/* VA of last avail page (end of kernel AS) */
vm_offset_t	vm_first_phys;	/* PA of first managed page */
vm_offset_t	vm_last_phys;	/* PA just past last managed page */
int		i386pagesperpage;	/* PAGE_SIZE / I386_PAGE_SIZE */
boolean_t	pmap_initialized = FALSE;	/* Has pmap_init completed? */
char		*pmap_attributes;	/* reference and modify bits */

static boolean_t	pmap_testbit();
void		pmap_clear_modify();

#if BSDVM_COMPAT
#include <sys/msgbuf.h>

/*
 * All those kernel PT submaps that BSD is so fond of
 */
struct pte	*CMAP1, *CMAP2, *mmap;
caddr_t		CADDR1, CADDR2, vmmap;
struct pte	*msgbufmap;
struct msgbuf	*msgbufp;
#endif

void pmap_activate __P((pmap_t, struct pcb *));

/*
 *	Bootstrap the system enough to run with virtual memory.
 *	Map the kernel's code and data, and allocate the system page table.
 *
 *	On the I386 this is called after mapping has already been enabled
 *	and just syncs the pmap module with what has already been done.
 *	[We can't call it easily with mapping off since the kernel is not
 *	mapped with PA == VA, hence we would have to relocate every address
 *	from the linked base (virtual) address 0xFE000000 to the actual
 *	(physical) address starting relative to 0]
 */
struct pte *pmap_pte();

extern vm_offset_t	atdevbase;
/* EN: Early pmap setup: forces avail_start to 0x100000, sets avail_end to maxmem minus msgbuf, calls i386_protection_init, points kernel_pmap->pm_pdir at IdlePTD and pm_count=1. */
/* 中文：早期 pmap 初始化：把 avail_start 强制设为 0x100000，avail_end 取 maxmem 减去 msgbuf，调用 i386_protection_init，令 kernel_pmap->pm_pdir 指向 IdlePTD 并置 pm_count=1。 */
void
pmap_bootstrap(firstaddr, loadaddr)
	vm_offset_t firstaddr;
	vm_offset_t loadaddr;
{
#if BSDVM_COMPAT
	vm_offset_t va;
	struct pte *pte;
#endif
	extern vm_offset_t maxmem, physmem;
extern int IdlePTD;


/* disable pageing in basemem for all machines until this cryptic comment
 * can be explained
 */
#if 1 ||	defined(ODYSSEUS) || defined(ARGO) || defined(CIRCE)
firstaddr=0x100000;	/* for some reason, basemem screws up on this machine */
#endif
printf("ps %x pe %x ", firstaddr, maxmem <<PG_SHIFT);
	avail_start = firstaddr;
	avail_end = maxmem << PG_SHIFT;

	/* XXX: allow for msgbuf */
	avail_end -= i386_round_page(sizeof(struct msgbuf));

	mem_size = physmem << PG_SHIFT;
	virtual_avail = atdevbase + 0x100000 - 0xa0000 + 10*NBPG;
	virtual_end = VM_MAX_KERNEL_ADDRESS;
	i386pagesperpage = PAGE_SIZE / I386_PAGE_SIZE;

	/*
	 * Initialize protection array.
	 */
	i386_protection_init();

#ifdef notdef
	/*
	 * Create Kernel page directory table and page maps.
	 * [ currently done in locore. i have wild and crazy ideas -wfj ]
	 */
	bzero(firstaddr, 4*NBPG);
	kernel_pmap->pm_pdir = firstaddr + VM_MIN_KERNEL_ADDRESS;
	kernel_pmap->pm_ptab = firstaddr + VM_MIN_KERNEL_ADDRESS + NBPG;

	firstaddr += NBPG;
	for (x = i386_btod(VM_MIN_KERNEL_ADDRESS);
		x < i386_btod(VM_MIN_KERNEL_ADDRESS)+3; x++) {
			struct pde *pde;
		pde = kernel_pmap->pm_pdir + x;
		*(int *)pde = firstaddr + x*NBPG | PG_V | PG_KW;
	}
#else
	kernel_pmap->pm_pdir = (pd_entry_t *)(0xfe000000 + IdlePTD);
#endif


	simple_lock_init(&kernel_pmap->pm_lock);
	kernel_pmap->pm_count = 1;

#if BSDVM_COMPAT
	/*
	 * Allocate all the submaps we need
	 */
#define	SYSMAP(c, p, v, n)	\
	v = (c)va; va += ((n)*I386_PAGE_SIZE); p = pte; pte += (n);

	va = virtual_avail;
	pte = pmap_pte(kernel_pmap, va);

	SYSMAP(caddr_t		,CMAP1		,CADDR1	   ,1		)
	SYSMAP(caddr_t		,CMAP2		,CADDR2	   ,1		)
	SYSMAP(caddr_t		,mmap		,vmmap	   ,1		)
	SYSMAP(struct msgbuf *	,msgbufmap	,msgbufp   ,1		)
	virtual_avail = va;
#endif

	/**(int *)PTD = 0;
	load_cr3(rcr3());*/

}

pmap_isvalidphys(addr) {
	if (addr < 0xa0000) return (1);
	if (addr >= 0x100000) return (1);
	return(0);
}

/*
 * Bootstrap memory allocator. This function allows for early dynamic
 * memory allocation until the virtual memory system has been bootstrapped.
 * After that point, either kmem_alloc or malloc should be used. This
 * function works by stealing pages from the (to be) managed page pool,
 * stealing virtual address space, then mapping the pages and zeroing them.
 *
 * It should be used from pmap_bootstrap till vm_page_startup, afterwards
 * it cannot be used, and will generate a panic if tried. Note that this
 * memory will never be freed, and in essence it is wired down.
 */
void *
pmap_bootstrap_alloc(size) {
	vm_offset_t val;
	int i;
	extern boolean_t vm_page_startup_initialized;
	
	if (vm_page_startup_initialized)
		panic("pmap_bootstrap_alloc: called after startup initialized");
	size = round_page(size);
	val = virtual_avail;

	/* deal with "hole incursion" */
	for (i = 0; i < size; i += PAGE_SIZE) {

		while (!pmap_isvalidphys(avail_start))
				avail_start += PAGE_SIZE;
		
		virtual_avail = pmap_map(virtual_avail, avail_start,
			avail_start + PAGE_SIZE, VM_PROT_READ|VM_PROT_WRITE);
		avail_start += PAGE_SIZE;
	}

	blkclr ((caddr_t) val, size);
	return ((void *) val);
}

/*
 *	Initialize the pmap module.
 *	Called by vm_init, to initialize any structures that the pmap
 *	system needs to map virtual memory.
 */
/* EN: Reserves the ISA hole and kernel PT area in kernel_map, kmem_alloc's pv_table and pmap_attributes, then sets vm_first_phys/vm_last_phys and pmap_initialized=TRUE. */
/* 中文：在 kernel_map 中标记 ISA 空洞和内核页表区已占用，用 kmem_alloc 分配 pv_table 与 pmap_attributes，最后设置 vm_first_phys/vm_last_phys 且把 pmap_initialized 置 TRUE。 */
void
pmap_init(phys_start, phys_end)
	vm_offset_t	phys_start, phys_end;
{
	vm_offset_t	addr, addr2;
	vm_size_t	npg, s;
	int		rv;
	extern int KPTphys;

#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_init(%x, %x)\n", phys_start, phys_end);
#endif
	/*
	 * Now that kernel map has been allocated, we can mark as
	 * unavailable regions which we have mapped in locore.
	 */
	addr = atdevbase;
	(void) vm_map_find(kernel_map, NULL, (vm_offset_t) 0,
			   &addr, (0x100000-0xa0000), FALSE);

	addr = (vm_offset_t) 0xfe000000+KPTphys/* *NBPG */;
	vm_object_reference(kernel_object);
	(void) vm_map_find(kernel_map, kernel_object, addr,
			   &addr, 2*NBPG, FALSE);

	/*
	 * Allocate memory for random pmap data structures.  Includes the
	 * pv_head_table and pmap_attributes.
	 */
	npg = atop(phys_end - phys_start);
	s = (vm_size_t) (sizeof(struct pv_entry) * npg + npg);
	s = round_page(s);
	addr = (vm_offset_t) kmem_alloc(kernel_map, s);
	pv_table = (pv_entry_t) addr;
	addr += sizeof(struct pv_entry) * npg;
	pmap_attributes = (char *) addr;
#ifdef DEBUG
	if (pmapdebug & PDB_INIT)
		printf("pmap_init: %x bytes (%x pgs): tbl %x attr %x\n",
		       s, npg, pv_table, pmap_attributes);
#endif

	/*
	 * Now it is safe to enable pv_table recording.
	 */
	vm_first_phys = phys_start;
	vm_last_phys = phys_end;
	pmap_initialized = TRUE;
}

/*
 *	Used to map a range of physical addresses into kernel
 *	virtual address space.
 *
 *	For now, VM is already on, we only need to map the
 *	specified memory.
 */
/* EN: Maps the [start,end) physical range into kernel virtual space page by page via pmap_enter on kernel_pmap and returns the address just past the range. */
/* 中文：把物理区间 [start,end) 逐页通过 pmap_enter 映射到 kernel_pmap 的 virt 处，返回该区间之后的下一个虚拟地址。 */
vm_offset_t
pmap_map(virt, start, end, prot)
	vm_offset_t	virt;
	vm_offset_t	start;
	vm_offset_t	end;
	int		prot;
{
#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_map(%x, %x, %x, %x)\n", virt, start, end, prot);
#endif
	while (start < end) {
		pmap_enter(kernel_pmap, virt, start, prot, FALSE);
		virt += PAGE_SIZE;
		start += PAGE_SIZE;
	}
	return(virt);
}

/*
 *	Create and return a physical map.
 *
 *	If the size specified for the map
 *	is zero, the map is an actual physical
 *	map, and may be referenced by the
 *	hardware.
 *
 *	If the size specified is non-zero,
 *	the map will be used in software only, and
 *	is bounded by that size.
 *
 * [ just allocate a ptd and mark it uninitialize -- should we track
 *   with a table which process has which ptd? -wfj ]
 */

pmap_t
/* EN: Allocates a zeroed struct pmap with malloc(M_VMPMAP, M_WAITOK) and initializes it with pmap_pinit; a non-zero size (software map) returns NULL. */
/* 中文：用 malloc(M_VMPMAP, M_WAITOK) 分配 struct pmap 并清零，再交给 pmap_pinit 初始化后返回；size 非 0（纯软件 map）时直接返回 NULL。 */
pmap_create(size)
	vm_size_t	size;
{
	register pmap_t pmap;

#ifdef DEBUG
	if (pmapdebug & (PDB_FOLLOW|PDB_CREATE))
		printf("pmap_create(%x)\n", size);
#endif
	/*
	 * Software use map does not need a pmap
	 */
	if (size)
		return(NULL);

	/* XXX: is it ok to wait here? */
	pmap = (pmap_t) malloc(sizeof *pmap, M_VMPMAP, M_WAITOK);
#ifdef notifwewait
	if (pmap == NULL)
		panic("pmap_create: cannot allocate a pmap");
#endif
	bzero(pmap, sizeof(*pmap));
	pmap_pinit(pmap);
	return (pmap);
}

/*
 * Initialize a preallocated and zeroed pmap structure,
 * such as one in a vmspace structure.
 */
/* EN: Gives pmap a page directory from kmem_alloc, copies the kernel global PDE entries, installs the self-referential PTDPTDI entry as PG_V|PG_URKW, sets pm_count=1 and inits pm_lock. */
/* 中文：用 kmem_alloc 为 pmap 取得一页页目录，复制内核全局 PDE 项，在 PTDPTDI 处写入指向自身的 PG_V|PG_URKW 表项，并置 pm_count=1、初始化 pm_lock。 */
void
pmap_pinit(pmap)
	register struct pmap *pmap;
{

#ifdef DEBUG
	if (pmapdebug & (PDB_FOLLOW|PDB_CREATE))
		pg("pmap_pinit(%x)\n", pmap);
#endif

	/*
	 * No need to allocate page table space yet but we do need a
	 * valid page directory table.
	 */
	pmap->pm_pdir = (pd_entry_t *) kmem_alloc(kernel_map, NBPG);

	/* wire in kernel global address entries */
	bcopy(PTD+KPTDI_FIRST, pmap->pm_pdir+KPTDI_FIRST,
		(KPTDI_LAST-KPTDI_FIRST+1)*4);

	/* install self-referential address mapping entry */
	*(int *)(pmap->pm_pdir+PTDPTDI) =
		(int)pmap_extract(kernel_pmap, (vm_offset_t)pmap->pm_pdir) | PG_V | PG_URKW;

	pmap->pm_count = 1;
	simple_lock_init(&pmap->pm_lock);
}

/*
 *	Retire the given physical map from service.
 *	Should only be called if the map contains
 *	no valid mappings.
 */
/* EN: Drops pmap->pm_count under simple_lock; on reaching zero it calls pmap_release and frees the struct to M_VMPMAP; a NULL pmap is ignored. */
/* 中文：在 simple_lock 保护下递减 pmap->pm_count，减到 0 时调用 pmap_release 并把结构体 free 归还 M_VMPMAP；pmap 为 NULL 时直接返回。 */
void
pmap_destroy(pmap)
	register pmap_t pmap;
{
	int count;

#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_destroy(%x)\n", pmap);
#endif
	if (pmap == NULL)
		return;

	simple_lock(&pmap->pm_lock);
	count = --pmap->pm_count;
	simple_unlock(&pmap->pm_lock);
	if (count == 0) {
		pmap_release(pmap);
		free((caddr_t)pmap, M_VMPMAP);
	}
}

/*
 * Release any resources held by the given physical map.
 * Called when a pmap initialized by pmap_pinit is being released.
 * Should only be called if the map contains no valid mappings.
 */
/* EN: Frees just the page directory page pm_pdir of pmap with kmem_free; every mapping must already have been removed by the caller. */
/* 中文：只用 kmem_free 释放 pmap 的页目录页 pm_pdir；调用前该 pmap 的所有映射必须已由调用者清除。 */
void
pmap_release(pmap)
	register struct pmap *pmap;
{

#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		pg("pmap_release(%x)\n", pmap);
#endif
#ifdef notdef /* DIAGNOSTIC */
	/* count would be 0 from pmap_destroy... */
	simple_lock(&pmap->pm_lock);
	if (pmap->pm_count != 1)
		panic("pmap_release count");
#endif
	kmem_free(kernel_map, (vm_offset_t)pmap->pm_pdir, NBPG);
}

/*
 *	Add a reference to the specified pmap.
 */
/* EN: Increments pmap->pm_count under simple_lock; NULL is ignored and no other field is touched. */
/* 中文：在 simple_lock 保护下把 pmap->pm_count 加一；pmap 为 NULL 时忽略，不修改其他任何字段。 */
void
pmap_reference(pmap)
	pmap_t	pmap;
{
#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		pg("pmap_reference(%x)", pmap);
#endif
	if (pmap != NULL) {
		simple_lock(&pmap->pm_lock);
		pmap->pm_count++;
		simple_unlock(&pmap->pm_lock);
	}
}

/*
 *	Remove the given range of addresses from the specified map.
 *
 *	It is assumed that the start and end are properly
 *	rounded to the page size.
 */
/* EN: Unmaps [sva,eva): zeroes each valid PTE run, adjusts wired/resident counts, tlbflush()es, unlinks the pv_table entry under splimp and saves PG_U/PG_M bits in pmap_attributes. */
/* 中文：解除 [sva,eva) 的映射：清零有效 PTE 串，调整 wired/resident 计数，调用 tlbflush()，在 splimp 下摘除 pv_table 表项，并把 PG_U/PG_M 位存入 pmap_attributes。 */
void
pmap_remove(pmap, sva, eva)
	register struct pmap *pmap;
	vm_offset_t sva, eva;
{
	register vm_offset_t pa, va;
	register pt_entry_t *pte;
	register pv_entry_t pv, npv;
	register int ix;
	pmap_t ptpmap;
	int *pde, s, bits;
	boolean_t firstpage = TRUE;
	boolean_t flushcache = FALSE;
#ifdef DEBUG
	pt_entry_t opte;

	if (pmapdebug & (PDB_FOLLOW|PDB_REMOVE|PDB_PROTECT))
		printf("pmap_remove(%x, %x, %x)", pmap, sva, eva);
	if (eva >= USRSTACK && eva <= UPT_MAX_ADDRESS)
		nullop();
#endif

	if (pmap == NULL)
		return;

#ifdef DEBUG
	remove_stats.calls++;
#endif
	for (va = sva; va < eva; va += PAGE_SIZE) {
		/*
		 * Weed out invalid mappings.
		 * Note: we assume that the page directory table is
	 	 * always allocated, and in kernel virtual.
		 */
		if (!pmap_pde_v(pmap_pde(pmap, va)))
			continue;

		pte = pmap_pte(pmap, va);
		if (pte == 0)
			continue;
		pa = pmap_pte_pa(pte);
		if (pa == 0)
			continue;
#ifdef DEBUG
		opte = *pte;
		remove_stats.removes++;
#endif
		/*
		 * Update statistics
		 */
		if (pmap_pte_w(pte))
			pmap->pm_stats.wired_count--;
		pmap->pm_stats.resident_count--;

		/*
		 * Invalidate the PTEs.
		 * XXX: should cluster them up and invalidate as many
		 * as possible at once.
		 */
#ifdef DEBUG
		if (pmapdebug & PDB_REMOVE)
			printf("remove: inv %x ptes at %x(%x) ",
			       i386pagesperpage, pte, *(int *)pte);
#endif
		bits = ix = 0;
		do {
			bits |= *(int *)pte & (PG_U|PG_M);
			*(int *)pte++ = 0;
			/*TBIS(va + ix * I386_PAGE_SIZE);*/
		} while (++ix != i386pagesperpage);
		if (pmap == &curproc->p_vmspace->vm_pmap)
			pmap_activate(pmap, (struct pcb *)curproc->p_addr);
		/* are we current address space or kernel? */
		/*if (pmap->pm_pdir[PTDPTDI].pd_pfnum == PTDpde.pd_pfnum
			|| pmap == kernel_pmap)
		load_cr3(curpcb->pcb_ptd);*/
		tlbflush();

#ifdef needednotdone
reduce wiring count on page table pages as references drop
#endif

		/*
		 * Remove from the PV table (raise IPL since we
		 * may be called at interrupt time).
		 */
		if (pa < vm_first_phys || pa >= vm_last_phys)
			continue;
		pv = pa_to_pvh(pa);
		s = splimp();
		/*
		 * If it is the first entry on the list, it is actually
		 * in the header and we must copy the following entry up
		 * to the header.  Otherwise we must search the list for
		 * the entry.  In either case we free the now unused entry.
		 */
		if (pmap == pv->pv_pmap && va == pv->pv_va) {
			npv = pv->pv_next;
			if (npv) {
				*pv = *npv;
				free((caddr_t)npv, M_VMPVENT);
			} else
				pv->pv_pmap = NULL;
#ifdef DEBUG
			remove_stats.pvfirst++;
#endif
		} else {
			for (npv = pv->pv_next; npv; npv = npv->pv_next) {
#ifdef DEBUG
				remove_stats.pvsearch++;
#endif
				if (pmap == npv->pv_pmap && va == npv->pv_va)
					break;
				pv = npv;
			}
#ifdef DEBUG
			if (npv == NULL)
				panic("pmap_remove: PA not in pv_tab");
#endif
			pv->pv_next = npv->pv_next;
			free((caddr_t)npv, M_VMPVENT);
			pv = pa_to_pvh(pa);
		}

#ifdef notdef
[tally number of pagetable pages, if sharing of ptpages adjust here]
#endif
		/*
		 * Update saved attributes for managed page
		 */
		pmap_attributes[pa_index(pa)] |= bits;
		splx(s);
	}
#ifdef notdef
[cache and tlb flushing, if needed]
#endif
}

/*
 *	Routine:	pmap_remove_all
 *	Function:
 *		Removes this physical page from
 *		all physical maps in which it resides.
 *		Reflects back modify bits to the pager.
 */
/* EN: Deletes every mapping of managed page pa by calling pmap_remove in a loop until its pv_table entry is empty, with splimp held; unmanaged pa returns at once. */
/* 中文：在 splimp 保护下循环调用 pmap_remove，直到物理页 pa 的 pv_table 表项为空，从而删除其全部映射；pa 不在受管范围时立即返回。 */
void
pmap_remove_all(pa)
	vm_offset_t pa;
{
	register pv_entry_t pv;
	int s;

#ifdef DEBUG
	if (pmapdebug & (PDB_FOLLOW|PDB_REMOVE|PDB_PROTECT))
		printf("pmap_remove_all(%x)", pa);
	/*pmap_pvdump(pa);*/
#endif
	/*
	 * Not one of ours
	 */
	if (pa < vm_first_phys || pa >= vm_last_phys)
		return;

	pv = pa_to_pvh(pa);
	s = splimp();
	/*
	 * Do it the easy way for now
	 */
	while (pv->pv_pmap != NULL) {
#ifdef DEBUG
		if (!pmap_pde_v(pmap_pde(pv->pv_pmap, pv->pv_va)) ||
		    pmap_pte_pa(pmap_pte(pv->pv_pmap, pv->pv_va)) != pa)
			panic("pmap_remove_all: bad mapping");
#endif
		pmap_remove(pv->pv_pmap, pv->pv_va, pv->pv_va + PAGE_SIZE);
	}
	splx(s);
}

/*
 *	Routine:	pmap_copy_on_write
 *	Function:
 *		Remove write privileges from all
 *		physical maps for this physical page.
 */
/* EN: Wrapper for pmap_changebit(pa, PG_RO, TRUE): clears write permission on all mappings of page pa so the next write takes a copy-on-write fault. */
/* 中文：对 pmap_changebit(pa, PG_RO, TRUE) 的包装：清除物理页 pa 所有映射的写权限，使其下次写入触发写时复制缺页。 */
void
pmap_copy_on_write(pa)
	vm_offset_t pa;
{
#ifdef DEBUG
	if (pmapdebug & (PDB_FOLLOW|PDB_PROTECT))
		printf("pmap_copy_on_write(%x)", pa);
#endif
	pmap_changebit(pa, PG_RO, TRUE);
}

/*
 *	Set the physical protection on the
 *	specified range of this map as requested.
 */
/* EN: Applies prot to [sva,eva): no-read calls pmap_remove, write needs nothing, else rewrites each valid PTE's protection (adding PG_u) and re-activates pmap if current. */
/* 中文：按 prot 设置 [sva,eva) 的页保护：无读权限则转调 pmap_remove，带写权限无需处理，否则重写有效 PTE 的保护位（加 PG_u），并在当前 pmap 上调用 pmap_activate。 */
void
pmap_protect(pmap, sva, eva, prot)
	register pmap_t	pmap;
	vm_offset_t	sva, eva;
	vm_prot_t	prot;
{
	register pt_entry_t *pte;
	register vm_offset_t va;
	register int ix;
	int i386prot;
	boolean_t firstpage = TRUE;

#ifdef DEBUG
	if (pmapdebug & (PDB_FOLLOW|PDB_PROTECT))
		printf("pmap_protect(%x, %x, %x, %x)", pmap, sva, eva, prot);
#endif
	if (pmap == NULL)
		return;

	if ((prot & VM_PROT_READ) == VM_PROT_NONE) {
		pmap_remove(pmap, sva, eva);
		return;
	}
	if (prot & VM_PROT_WRITE)
		return;

	for (va = sva; va < eva; va += PAGE_SIZE) {
		/*
		 * Page table page is not allocated.
		 * Skip it, we don't want to force allocation
		 * of unnecessary PTE pages just to set the protection.
		 */
		if (!pmap_pde_v(pmap_pde(pmap, va))) {
			/* XXX: avoid address wrap around */
			if (va >= i386_trunc_pdr((vm_offset_t)-1))
				break;
			va = i386_round_pdr(va + PAGE_SIZE) - PAGE_SIZE;
			continue;
		} else	pte = pmap_pte(pmap, va);

		/*
		 * Page not valid.  Again, skip it.
		 * Should we do this?  Or set protection anyway?
		 */
		if (!pmap_pte_v(pte))
			continue;

		ix = 0;
		i386prot = pte_prot(pmap, prot);
		if(va < UPT_MAX_ADDRESS)
			i386prot |= 2 /*PG_u*/;
		do {
			/* clear VAC here if PG_RO? */
			pmap_pte_set_prot(pte++, i386prot);
			/*TBIS(va + ix * I386_PAGE_SIZE);*/
		} while (++ix != i386pagesperpage);
	}
out:
	if (pmap == &curproc->p_vmspace->vm_pmap)
		pmap_activate(pmap, (struct pcb *)curproc->p_addr);
}

/*
 *	Insert the given physical page (p) at
 *	the specified virtual address (v) in the
 *	target physical map with the protection requested.
 *
 *	If specified, the page will be wired down, meaning
 *	that the related pte can not be reclaimed.
 *
 *	NB:  This is the only routine which MAY NOT lazy-evaluate
 *	or lose information.  That is, this routine must actually
 *	insert this page into the given map NOW.
 */
/* EN: Installs pa at va in pmap: removes the old mapping, links a pv_table entry under splimp, updates resident/wired counts, writes the PTE run with pte_prot|PG_V and calls tlbflush(). */
/* 中文：在 pmap 的 va 处装入物理页 pa：先删除旧映射，在 splimp 下挂入 pv_table 表项，更新 resident/wired 计数，写入 pte_prot|PG_V 组成的 PTE 串并调用 tlbflush()。 */
void
pmap_enter(pmap, va, pa, prot, wired)
	register pmap_t pmap;
	vm_offset_t va;
	register vm_offset_t pa;
	vm_prot_t prot;
	boolean_t wired;
{
	register pt_entry_t *pte;
	register int npte, ix;
	vm_offset_t opa;
	boolean_t cacheable = TRUE;
	boolean_t checkpv = TRUE;

#ifdef DEBUG
	if (pmapdebug & (PDB_FOLLOW|PDB_ENTER))
		printf("pmap_enter(%x, %x, %x, %x, %x)",
		       pmap, va, pa, prot, wired);
	if(!pmap_isvalidphys(pa)) panic("invalid phys");
#endif
	if (pmap == NULL)
		return;

	if(va > VM_MAX_KERNEL_ADDRESS)panic("pmap_enter: toobig");
	/* also, should not muck with PTD va! */

#ifdef DEBUG
	if (pmap == kernel_pmap)
		enter_stats.kernel++;
	else
		enter_stats.user++;
#endif

	/*
	 * Page Directory table entry not valid, we need a new PT page
	 */
	if (!pmap_pde_v(pmap_pde(pmap, va))) {
		pg("ptdi %x", pmap->pm_pdir[PTDPTDI]);
	}

	pte = pmap_pte(pmap, va);
	opa = pmap_pte_pa(pte);
#ifdef DEBUG
	if (pmapdebug & PDB_ENTER)
		printf("enter: pte %x, *pte %x ", pte, *(int *)pte);
#endif

	/*
	 * Mapping has not changed, must be protection or wiring change.
	 */
	if (opa == pa) {
#ifdef DEBUG
		enter_stats.pwchange++;
#endif
		/*
		 * Wiring change, just update stats.
		 * We don't worry about wiring PT pages as they remain
		 * resident as long as there are valid mappings in them.
		 * Hence, if a user page is wired, the PT page will be also.
		 */
		if (wired && !pmap_pte_w(pte) || !wired && pmap_pte_w(pte)) {
#ifdef DEBUG
			if (pmapdebug & PDB_ENTER)
				pg("enter: wiring change -> %x ", wired);
#endif
			if (wired)
				pmap->pm_stats.wired_count++;
			else
				pmap->pm_stats.wired_count--;
#ifdef DEBUG
			enter_stats.wchange++;
#endif
		}
		goto validate;
	}

	/*
	 * Mapping has changed, invalidate old range and fall through to
	 * handle validating new mapping.
	 */
	if (opa) {
#ifdef DEBUG
		if (pmapdebug & PDB_ENTER)
			printf("enter: removing old mapping %x pa %x ", va, opa);
#endif
		pmap_remove(pmap, va, va + PAGE_SIZE);
#ifdef DEBUG
		enter_stats.mchange++;
#endif
	}

	/*
	 * Enter on the PV list if part of our managed memory
	 * Note that we raise IPL while manipulating pv_table
	 * since pmap_enter can be called at interrupt time.
	 */
	if (pa >= vm_first_phys && pa < vm_last_phys) {
		register pv_entry_t pv, npv;
		int s;

#ifdef DEBUG
		enter_stats.managed++;
#endif
		pv = pa_to_pvh(pa);
		s = splimp();
#ifdef DEBUG
		if (pmapdebug & PDB_ENTER)
			printf("enter: pv at %x: %x/%x/%x ",
			       pv, pv->pv_va, pv->pv_pmap, pv->pv_next);
#endif
		/*
		 * No entries yet, use header as the first entry
		 */
		if (pv->pv_pmap == NULL) {
#ifdef DEBUG
			enter_stats.firstpv++;
#endif
			pv->pv_va = va;
			pv->pv_pmap = pmap;
			pv->pv_next = NULL;
			pv->pv_flags = 0;
		}
		/*
		 * There is at least one other VA mapping this page.
		 * Place this entry after the header.
		 */
		else {
			/*printf("second time: ");*/
#ifdef DEBUG
			for (npv = pv; npv; npv = npv->pv_next)
				if (pmap == npv->pv_pmap && va == npv->pv_va)
					panic("pmap_enter: already in pv_tab");
#endif
			npv = (pv_entry_t)
				malloc(sizeof *npv, M_VMPVENT, M_NOWAIT);
			npv->pv_va = va;
			npv->pv_pmap = pmap;
			npv->pv_next = pv->pv_next;
			pv->pv_next = npv;
#ifdef DEBUG
			if (!npv->pv_next)
				enter_stats.secondpv++;
#endif
		splx(s);
		}
	}
	/*
	 * Assumption: if it is not part of our managed memory
	 * then it must be device memory which may be volitile.
	 */
	if (pmap_initialized) {
		checkpv = cacheable = FALSE;
#ifdef DEBUG
		enter_stats.unmanaged++;
#endif
	}

	/*
	 * Increment counters
	 */
	pmap->pm_stats.resident_count++;
	if (wired)
		pmap->pm_stats.wired_count++;

validate:
	/*
	 * Now validate mapping with desired protection/wiring.
	 * Assume uniform modified and referenced status for all
	 * I386 pages in a MACH page.
	 */
	npte = (pa & PG_FRAME) | pte_prot(pmap, prot) | PG_V;
	npte |= (*(int *)pte & (PG_M|PG_U));
	if (wired)
		npte |= PG_W;
	if(va < UPT_MIN_ADDRESS)
		npte |= PG_u;
	else if(va < UPT_MAX_ADDRESS)
		npte |= PG_u | PG_RW;
#ifdef DEBUG
	if (pmapdebug & PDB_ENTER)
		printf("enter: new pte value %x ", npte);
#endif
	ix = 0;
	do {
		*(int *)pte++ = npte;
		/*TBIS(va);*/
		npte += I386_PAGE_SIZE;
		va += I386_PAGE_SIZE;
	} while (++ix != i386pagesperpage);
	pte--;
#ifdef DEBUGx
cache, tlb flushes
#endif
/*pads(pmap);*/
	/*load_cr3(((struct pcb *)curproc->p_addr)->pcb_ptd);*/
	tlbflush();
}

/*
 *      pmap_page_protect:
 *
 *      Lower the permission for all mappings to a given page.
 */
/* EN: Lowers permissions on all mappings of phys: read-only or read+exec calls pmap_copy_on_write, VM_PROT_ALL does nothing, anything else calls pmap_remove_all. */
/* 中文：降低物理页 phys 所有映射的权限：只读或只读加执行调用 pmap_copy_on_write，VM_PROT_ALL 不做处理，其余（如无访问权）调用 pmap_remove_all。 */
void
pmap_page_protect(phys, prot)
        vm_offset_t     phys;
        vm_prot_t       prot;
{
        switch (prot) {
        case VM_PROT_READ:
        case VM_PROT_READ|VM_PROT_EXECUTE:
                pmap_copy_on_write(phys);
                break;
        case VM_PROT_ALL:
                break;
        default:
                pmap_remove_all(phys);
                break;
        }
}

/*
 *	Routine:	pmap_change_wiring
 *	Function:	Change the wiring attribute for a map/virtual-address
 *			pair.
 *	In/out conditions:
 *			The mapping must already exist in the pmap.
 */
/* EN: Sets or clears PG_W on the whole PTE run for va and adjusts pm_stats.wired_count; wiring is software-only so no TLB flush is done. */
/* 中文：为 va 所在的整串 PTE 设置或清除 PG_W 位，并相应增减 pm_stats.wired_count；wiring 只是软件属性，因此不刷新 TLB。 */
void
pmap_change_wiring(pmap, va, wired)
	register pmap_t	pmap;
	vm_offset_t	va;
	boolean_t	wired;
{
	register pt_entry_t *pte;
	register int ix;

#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_change_wiring(%x, %x, %x)", pmap, va, wired);
#endif
	if (pmap == NULL)
		return;

	pte = pmap_pte(pmap, va);
#ifdef DEBUG
	/*
	 * Page table page is not allocated.
	 * Should this ever happen?  Ignore it for now,
	 * we don't want to force allocation of unnecessary PTE pages.
	 */
	if (!pmap_pde_v(pmap_pde(pmap, va))) {
		if (pmapdebug & PDB_PARANOIA)
			pg("pmap_change_wiring: invalid PDE for %x ", va);
		return;
	}
	/*
	 * Page not valid.  Should this ever happen?
	 * Just continue and change wiring anyway.
	 */
	if (!pmap_pte_v(pte)) {
		if (pmapdebug & PDB_PARANOIA)
			pg("pmap_change_wiring: invalid PTE for %x ", va);
	}
#endif
	if (wired && !pmap_pte_w(pte) || !wired && pmap_pte_w(pte)) {
		if (wired)
			pmap->pm_stats.wired_count++;
		else
			pmap->pm_stats.wired_count--;
	}
	/*
	 * Wiring is not a hardware characteristic so there is no need
	 * to invalidate TLB.
	 */
	ix = 0;
	do {
		pmap_pte_set_w(pte++, wired);
	} while (++ix != i386pagesperpage);
}

/*
 *	Routine:	pmap_pte
 *	Function:
 *		Extract the page table entry associated
 *		with the given map/virtual_address pair.
 * [ what about induced faults -wfj]
 */

/* EN: Returns the PTE pointer for va: vtopte() for kernel/current pmap, else loads APTDpde from pmap's PD (tlbflush if changed) and uses avtopte(); NULL if PDE invalid. */
/* 中文：返回 va 的 PTE 指针：内核或当前 pmap 用 vtopte()，否则把 APTDpde 切到该 pmap 的页目录（有变化就 tlbflush）再用 avtopte()；PDE 无效时返回 NULL。 */
struct pte *pmap_pte(pmap, va)
	register pmap_t	pmap;
	vm_offset_t va;
{

#ifdef DEBUGx
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_pte(%x, %x) ->\n", pmap, va);
#endif
	if (pmap && pmap_pde_v(pmap_pde(pmap, va))) {

		/* are we current address space or kernel? */
		if (pmap->pm_pdir[PTDPTDI].pd_pfnum == PTDpde.pd_pfnum
			|| pmap == kernel_pmap)
			return ((struct pte *) vtopte(va));

		/* otherwise, we are alternate address space */
		else {
			if (pmap->pm_pdir[PTDPTDI].pd_pfnum
				!= APTDpde.pd_pfnum) {
				APTDpde = pmap->pm_pdir[PTDPTDI];
				tlbflush();
			}
			return((struct pte *) avtopte(va));
		}
	}
	return(0);
}

/*
 *	Routine:	pmap_extract
 *	Function:
 *		Extract the physical page address associated
 *		with the given map/virtual_address pair.
 */

/* EN: Returns the physical address mapped at va: takes the PTE page frame (PG_FRAME) and ORs in va's page offset; returns 0 when the PDE or PTE is invalid. */
/* 中文：返回 va 映射到的物理地址：取 PTE 中的页帧（PG_FRAME）再或上 va 的页内偏移；PDE 或 PTE 无效时返回 0。 */
vm_offset_t
pmap_extract(pmap, va)
	register pmap_t	pmap;
	vm_offset_t va;
{
	register vm_offset_t pa;

#ifdef DEBUGx
	if (pmapdebug & PDB_FOLLOW)
		pg("pmap_extract(%x, %x) -> ", pmap, va);
#endif
	pa = 0;
	if (pmap && pmap_pde_v(pmap_pde(pmap, va))) {
		pa = *(int *) pmap_pte(pmap, va);
	}
	if (pa)
		pa = (pa & PG_FRAME) | (va & ~PG_FRAME);
#ifdef DEBUGx
	if (pmapdebug & PDB_FOLLOW)
		printf("%x\n", pa);
#endif
	return(pa);
}

/*
 *	Copy the range specified by src_addr/len
 *	from the source map to the range dst_addr/len
 *	in the destination map.
 *
 *	This routine is only advisory and need not do anything.
 */
/* EN: Advisory no-op: the i386 pmap does no page-table pre-copying, so beyond an optional DEBUG printf the arguments are ignored. */
/* 中文：仅作建议的空实现：i386 pmap 不做页表预复制，除 DEBUG 下的打印外完全忽略参数。 */
void pmap_copy(dst_pmap, src_pmap, dst_addr, len, src_addr)
	pmap_t		dst_pmap;
	pmap_t		src_pmap;
	vm_offset_t	dst_addr;
	vm_size_t	len;
	vm_offset_t	src_addr;
{
#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_copy(%x, %x, %x, %x, %x)",
		       dst_pmap, src_pmap, dst_addr, len, src_addr);
#endif
}

/*
 *	Require that all active physical maps contain no
 *	incorrect entries NOW.  [This update includes
 *	forcing updates of any address map caching.]
 *
 *	Generally used to insure that a thread about
 *	to run will see a semantically correct world.
 */
/* EN: Flush the TLB via tlbflush() so pending page-table changes take effect; the body is only an optional DEBUG trace plus that flush. */
/* 中文：调用 tlbflush() 刷新 TLB，使挂起的页表修改生效；函数体除可选的 DEBUG 打印外只有这次刷新。 */
void pmap_update()
{
#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_update()");
#endif
	tlbflush();
}

/*
 *	Routine:	pmap_collect
 *	Function:
 *		Garbage collects the physical map system for
 *		pages which are no longer used.
 *		Success need not be guaranteed -- that is, there
 *		may well be pages which are not referenced, but
 *		others may be collected.
 *	Usage:
 *		Called by the pageout daemon when pages are scarce.
 * [ needs to be written -wfj ]
 */
/* EN: Pageout-daemon hook meant to reclaim unused pv/PT pages; it returns unless pmap == kernel_pmap, and the rest of the body is empty, so it collects nothing. */
/* 中文：供页回收守护进程调用的收集钩子；仅当 pmap == kernel_pmap 时继续，之后函数体为空，因此当前不回收任何页表页。 */
void
pmap_collect(pmap)
	pmap_t		pmap;
{
	register vm_offset_t pa;
	register pv_entry_t pv;
	register int *pte;
	vm_offset_t kpa;
	int s;

#ifdef DEBUG
	int *pde;
	int opmapdebug;
	printf("pmap_collect(%x) ", pmap);
#endif
	if (pmap != kernel_pmap)
		return;

}

/* [ macro again?, should I force kstack into user map here? -wfj ] */
/* EN: Activate pmap for pcbp: PMAP_ACTIVATE stores the page directory's physical address in pcb_cr3, loads CR3 if pmap is curproc's, and clears pm_pdchanged. */
/* 中文：为 pcbp 激活 pmap：PMAP_ACTIVATE 把页目录物理地址写入 pcb_cr3，若该 pmap 属于 curproc 则装入 CR3，并清除 pm_pdchanged。 */
void
pmap_activate(pmap, pcbp)
	register pmap_t pmap;
	struct pcb *pcbp;
{
int x;
#ifdef DEBUG
	if (pmapdebug & (PDB_FOLLOW|PDB_PDRTAB))
		pg("pmap_activate(%x, %x) ", pmap, pcbp);
#endif
	PMAP_ACTIVATE(pmap, pcbp);
/*printf("pde ");
for(x=0x3f6; x < 0x3fA; x++)
	printf("%x ", pmap->pm_pdir[x]);*/
/*pads(pmap);*/
/*pg(" pcb_cr3 %x", pcbp->pcb_cr3);*/
}

/*
 *	pmap_zero_page zeros the specified (machine independent)
 *	page by mapping the page into virtual memory and using
 *	bzero to clear its contents, one machine dependent page
 *	at a time.
 */
/* EN: Zero one machine-independent page: shift phys to a page index and call clearseg for each of i386pagesperpage hardware pages; no locking or TLB flush. */
/* 中文：清零一个机器无关页：把 phys 右移为页号，再对 i386pagesperpage 个硬件页逐个调用 clearseg；不加锁也不刷新 TLB。 */
void
pmap_zero_page(phys)
	register vm_offset_t	phys;
{
	register int ix;

#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_zero_page(%x)", phys);
#endif
	phys >>= PG_SHIFT;
	ix = 0;
	do {
		clearseg(phys++);
	} while (++ix != i386pagesperpage);
}

/*
 *	pmap_copy_page copies the specified (machine independent)
 *	page by mapping the page into virtual memory and using
 *	bcopy to copy the page, one machine dependent page at a
 *	time.
 */
/* EN: Copy page src to dst: shift both physical addresses to page indexes and call physcopyseg once per hardware page; no locking or TLB flush. */
/* 中文：复制 src 页到 dst 页：把两个物理地址右移为页号后，对每个硬件页调用一次 physcopyseg；不加锁也不刷新 TLB。 */
void
pmap_copy_page(src, dst)
	register vm_offset_t	src, dst;
{
	register int ix;

#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_copy_page(%x, %x)", src, dst);
#endif
	src >>= PG_SHIFT;
	dst >>= PG_SHIFT;
	ix = 0;
	do {
		physcopyseg(src++, dst++);
	} while (++ix != i386pagesperpage);
}


/*
 *	Routine:	pmap_pageable
 *	Function:
 *		Make the specified pages (by pmap, offset)
 *		pageable (or not) as requested.
 *
 *		A page which is not pageable may not take
 *		a fault; therefore, its page table entry
 *		must remain valid for the duration.
 *
 *		This routine is merely advisory; pmap_enter
 *		will specify that these pages are to be wired
 *		down (or not) as appropriate.
 */
/* EN: Advisory only: for a single kernel PT page being made pageable, look up its pv entry and clear the modify bit so pageout won't reclaim it; other cases ignored. */
/* 中文：仅作提示：当单个内核页表页被置为可分页时，查其 pv 表项并清除修改位以免被换出；其他情况直接忽略。 */
void
pmap_pageable(pmap, sva, eva, pageable)
	pmap_t		pmap;
	vm_offset_t	sva, eva;
	boolean_t	pageable;
{
#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_pageable(%x, %x, %x, %x)",
		       pmap, sva, eva, pageable);
#endif
	/*
	 * If we are making a PT page pageable then all valid
	 * mappings must be gone from that page.  Hence it should
	 * be all zeros and there is no need to clean it.
	 * Assumptions:
	 *	- we are called with only one page at a time
	 *	- PT pages have only one pv_table entry
	 */
	if (pmap == kernel_pmap && pageable && sva + PAGE_SIZE == eva) {
		register pv_entry_t pv;
		register vm_offset_t pa;

#ifdef DEBUG
		if ((pmapdebug & (PDB_FOLLOW|PDB_PTPAGE)) == PDB_PTPAGE)
			printf("pmap_pageable(%x, %x, %x, %x)",
			       pmap, sva, eva, pageable);
#endif
		/*if (!pmap_pde_v(pmap_pde(pmap, sva)))
			return;*/
		if(pmap_pte(pmap, sva) == 0)
			return;
		pa = pmap_pte_pa(pmap_pte(pmap, sva));
		if (pa < vm_first_phys || pa >= vm_last_phys)
			return;
		pv = pa_to_pvh(pa);
		/*if (!ispt(pv->pv_va))
			return;*/
#ifdef DEBUG
		if (pv->pv_va != sva || pv->pv_next) {
			pg("pmap_pageable: bad PT page va %x next %x\n",
			       pv->pv_va, pv->pv_next);
			return;
		}
#endif
		/*
		 * Mark it unmodified to avoid pageout
		 */
		pmap_clear_modify(pa);
#ifdef needsomethinglikethis
		if (pmapdebug & PDB_PTPAGE)
			pg("pmap_pageable: PT page %x(%x) unmodified\n",
			       sva, *(int *)pmap_pte(pmap, sva));
		if (pmapdebug & PDB_WIRING)
			pmap_check_wiring("pageable", sva);
#endif
	}
}

/*
 *	Clear the modify bits on the specified physical page.
 */

/* EN: Thin wrapper over pmap_changebit(pa, PG_M, FALSE) that clears the modify bit on every mapping of physical page pa. */
/* 中文：对 pmap_changebit(pa, PG_M, FALSE) 的薄封装，清除物理页 pa 所有映射上的修改位。 */
void
pmap_clear_modify(pa)
	vm_offset_t	pa;
{
#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_clear_modify(%x)", pa);
#endif
	pmap_changebit(pa, PG_M, FALSE);
}

/*
 *	pmap_clear_reference:
 *
 *	Clear the reference bit on the specified physical page.
 */

/* EN: Thin wrapper over pmap_changebit(pa, PG_U, FALSE) that clears the reference (used) bit in every PTE mapping physical page pa. */
/* 中文：对 pmap_changebit(pa, PG_U, FALSE) 的薄封装，清除映射物理页 pa 的每个 PTE 中的引用（已使用）位。 */
void pmap_clear_reference(pa)
	vm_offset_t	pa;
{
#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW)
		printf("pmap_clear_reference(%x)", pa);
#endif
	pmap_changebit(pa, PG_U, FALSE);
}

/*
 *	pmap_is_referenced:
 *
 *	Return whether or not the specified physical page is referenced
 *	by any physical maps.
 */

/* EN: Return pmap_testbit(pa, PG_U): TRUE if any mapping of pa has the reference bit set; DEBUG builds print the result and return it early. */
/* 中文：返回 pmap_testbit(pa, PG_U)：pa 的任一映射置有引用位则为 TRUE；DEBUG 版本会先打印结果再提前返回。 */
boolean_t
pmap_is_referenced(pa)
	vm_offset_t	pa;
{
#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW) {
		boolean_t rv = pmap_testbit(pa, PG_U);
		printf("pmap_is_referenced(%x) -> %c", pa, "FT"[rv]);
		return(rv);
	}
#endif
	return(pmap_testbit(pa, PG_U));
}

/*
 *	pmap_is_modified:
 *
 *	Return whether or not the specified physical page is modified
 *	by any physical maps.
 */

/* EN: Return pmap_testbit(pa, PG_M): TRUE if any PTE mapping physical page pa has the modify bit set; DEBUG builds print the result and return early. */
/* 中文：返回 pmap_testbit(pa, PG_M)：映射物理页 pa 的任一 PTE 置有修改位则为 TRUE；DEBUG 版本会打印结果后提前返回。 */
boolean_t
pmap_is_modified(pa)
	vm_offset_t	pa;
{
#ifdef DEBUG
	if (pmapdebug & PDB_FOLLOW) {
		boolean_t rv = pmap_testbit(pa, PG_M);
		printf("pmap_is_modified(%x) -> %c", pa, "FT"[rv]);
		return(rv);
	}
#endif
	return(pmap_testbit(pa, PG_M));
}

/* EN: Convert a page number to a byte offset with i386_ptob(ppn); a pure arithmetic helper with no side effects. */
/* 中文：用 i386_ptob(ppn) 把页号换算为字节地址，纯算术辅助函数，无副作用。 */
vm_offset_t
pmap_phys_address(ppn)
	int ppn;
{
	return(i386_ptob(ppn));
}

/*
 * Miscellaneous support routines follow
 */

/* EN: Fill protection_codes[0..7], mapping each 3-bit VM_PROT combination to 0, PG_RO or PG_RW so that pmap_enter can look it up by protection. */
/* 中文：填充 protection_codes[0..7]，把 3 位 VM_PROT 组合映射为 0、PG_RO 或 PG_RW，供 pmap_enter 按保护位查表。 */
i386_protection_init()
{
	register int *kp, prot;

	kp = protection_codes;
	for (prot = 0; prot < 8; prot++) {
		switch (prot) {
		case VM_PROT_NONE | VM_PROT_NONE | VM_PROT_NONE:
			*kp++ = 0;
			break;
		case VM_PROT_READ | VM_PROT_NONE | VM_PROT_NONE:
		case VM_PROT_READ | VM_PROT_NONE | VM_PROT_EXECUTE:
		case VM_PROT_NONE | VM_PROT_NONE | VM_PROT_EXECUTE:
			*kp++ = PG_RO;
			break;
		case VM_PROT_NONE | VM_PROT_WRITE | VM_PROT_NONE:
		case VM_PROT_NONE | VM_PROT_WRITE | VM_PROT_EXECUTE:
		case VM_PROT_READ | VM_PROT_WRITE | VM_PROT_NONE:
		case VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE:
			*kp++ = PG_RW;
			break;
		}
	}
}

/* EN: Return TRUE if the saved pmap_attributes word or any pv-listed PTE for pa has bit set; a pa outside vm_first_phys/vm_last_phys gives FALSE; runs at splimp. */
/* 中文：若保存的 pmap_attributes 或 pa 的任一 pv 链 PTE 中该位已置则返回 TRUE；pa 越界返回 FALSE，扫描期间用 splimp 屏蔽中断。 */
static
boolean_t
pmap_testbit(pa, bit)
	register vm_offset_t pa;
	int bit;
{
	register pv_entry_t pv;
	register int *pte, ix;
	int s;

	if (pa < vm_first_phys || pa >= vm_last_phys)
		return(FALSE);

	pv = pa_to_pvh(pa);
	s = splimp();
	/*
	 * Check saved info first
	 */
	if (pmap_attributes[pa_index(pa)] & bit) {
		splx(s);
		return(TRUE);
	}
	/*
	 * Not found, check current mappings returning
	 * immediately if found.
	 */
	if (pv->pv_pmap != NULL) {
		for (; pv; pv = pv->pv_next) {
			pte = (int *) pmap_pte(pv->pv_pmap, pv->pv_va);
			ix = 0;
			do {
				if (*pte++ & bit) {
					splx(s);
					return(TRUE);
				}
			} while (++ix != i386pagesperpage);
		}
	}
	splx(s);
	return(FALSE);
}

/* EN: Set or clear bit in every PTE mapping pa and in pmap_attributes; skips pager mappings when setting PG_RO, re-activates the current pmap; runs at splimp. */
/* 中文：在映射 pa 的所有 PTE 及 pmap_attributes 中置位或清位；置 PG_RO 时跳过 pager 映射，必要时重新激活当前 pmap；全程 splimp。 */
pmap_changebit(pa, bit, setem)
	register vm_offset_t pa;
	int bit;
	boolean_t setem;
{
	register pv_entry_t pv;
	register int *pte, npte, ix;
	vm_offset_t va;
	int s;
	boolean_t firstpage = TRUE;

#ifdef DEBUG
	if (pmapdebug & PDB_BITS)
		printf("pmap_changebit(%x, %x, %s)",
		       pa, bit, setem ? "set" : "clear");
#endif
	if (pa < vm_first_phys || pa >= vm_last_phys)
		return;

	pv = pa_to_pvh(pa);
	s = splimp();
	/*
	 * Clear saved attributes (modify, reference)
	 */
	if (!setem)
		pmap_attributes[pa_index(pa)] &= ~bit;
	/*
	 * Loop over all current mappings setting/clearing as appropos
	 * If setting RO do we need to clear the VAC?
	 */
	if (pv->pv_pmap != NULL) {
#ifdef DEBUG
		int toflush = 0;
#endif
		for (; pv; pv = pv->pv_next) {
#ifdef DEBUG
			toflush |= (pv->pv_pmap == kernel_pmap) ? 2 : 1;
#endif
			va = pv->pv_va;

                        /*
                         * XXX don't write protect pager mappings
                         */
                        if (bit == PG_RO) {
                                extern vm_offset_t pager_sva, pager_eva;

                                if (va >= pager_sva && va < pager_eva)
                                        continue;
                        }

			pte = (int *) pmap_pte(pv->pv_pmap, va);
			ix = 0;
			do {
				if (setem)
					npte = *pte | bit;
				else
					npte = *pte & ~bit;
				if (*pte != npte) {
					*pte = npte;
					/*TBIS(va);*/
				}
				va += I386_PAGE_SIZE;
				pte++;
			} while (++ix != i386pagesperpage);

			if (pv->pv_pmap == &curproc->p_vmspace->vm_pmap)
				pmap_activate(pv->pv_pmap, (struct pcb *)curproc->p_addr);
		}
#ifdef somethinglikethis
		if (setem && bit == PG_RO && (pmapvacflush & PVF_PROTECT)) {
			if ((pmapvacflush & PVF_TOTAL) || toflush == 3)
				DCIA();
			else if (toflush == 2)
				DCIS();
			else
				DCIU();
		}
#endif
	}
	splx(s);
}

#ifdef DEBUG
/* EN: DEBUG helper that printf's pa then walks the pv chain printing each pv_pmap, pv_va and pv_flags, calling pads() to dump that pmap's address space. */
/* 中文：DEBUG 辅助函数：先打印 pa，再遍历 pv 链表输出每项的 pv_pmap、pv_va、pv_flags，并调用 pads() 转储对应 pmap 的地址空间。 */
pmap_pvdump(pa)
	vm_offset_t pa;
{
	register pv_entry_t pv;

	printf("pa %x", pa);
	for (pv = pa_to_pvh(pa); pv; pv = pv->pv_next) {
		printf(" -> pmap %x, va %x, flags %x",
		       pv->pv_pmap, pv->pv_va, pv->pv_flags);
		pads(pv->pv_pmap);
	}
	printf(" ");
}

#ifdef notyet
/* EN: DEBUG consistency check: truncate va to a page, return if unmapped, find the pt_map entry and pg() a complaint if its wired_count differs from the valid PTE count. */
/* 中文：DEBUG 一致性检查：把 va 截到页边界，未映射则返回；查到 pt_map 表项后，若其 wired_count 与该页有效 PTE 数不符就用 pg() 报错。 */
pmap_check_wiring(str, va)
	char *str;
	vm_offset_t va;
{
	vm_map_entry_t entry;
	register int count, *pte;

	va = trunc_page(va);
	if (!pmap_pde_v(pmap_pde(kernel_pmap, va)) ||
	    !pmap_pte_v(pmap_pte(kernel_pmap, va)))
		return;

	if (!vm_map_lookup_entry(pt_map, va, &entry)) {
		pg("wired_check: entry for %x not found\n", va);
		return;
	}
	count = 0;
	for (pte = (int *)va; pte < (int *)(va+PAGE_SIZE); pte++)
		if (*pte)
			count++;
	if (entry->wired_count != count)
		pg("*%s*: %x: w%d/a%d\n",
		       str, va, entry->wired_count, count);
}
#endif

/* print address space of pmap*/
pads(pm) pmap_t pm; {
	unsigned va, i, j;
	struct pte *ptep;

	if(pm == kernel_pmap) return;
	for (i = 0; i < 1024; i++) 
		if(pm->pm_pdir[i].pd_v)
			for (j = 0; j < 1024 ; j++) {
				va = (i<<22)+(j<<12);
				if (pm == kernel_pmap && va < 0xfe000000)
						continue;
				if (pm != kernel_pmap && va > UPT_MAX_ADDRESS)
						continue;
				ptep = pmap_pte(pm, va);
				if(pmap_pte_v(ptep)) 
					printf("%x:%x ", va, *(int *)ptep); 
			} ;
				
}
#endif
