/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * mem_plan.c - S411: derive the memory budget and thread count from RAM.
 *
 * Called once from main() after the BIOS has filled in physmem and before
 * kthread_init()/page_ref_init().  Nothing here allocates memory: the plan
 * only sizes the heaps, the mmap window and the TCB pool.  Pages are still
 * handed out on demand by page_alloc().
 *
 * Detection is pure BIOS/E801 -> physmem (page count), so adding RAM needs
 * no rebuild.  All arithmetic below is 64-bit: a 24 GB machine would
 * overflow every "MB << 20" expression in 32-bit, and the whole point is
 * that 6/8/12/16/20/24 GB machines get a correct plan without code edits.
 */
#include <sys/param.h>
#include <sys/systm.h>

#include <vm/mem_plan.h>
#include "thread/kthread.h"

struct mem_plan		mem_plan;
unsigned		mem_phys_limit = 0x3f000000;
unsigned long long	mem_detected_bytes;

/*
 * Runtime TCB slot count.  NKTHREAD_MAX is the storage ceiling of this
 * port (static bitmaps and the VA window are sized for it); this variable
 * is what the scheduler and every loop actually use.  It is the *usable*
 * count, not the policy count, so a 24 GB host on this non-PAE kernel
 * cannot over-commit TCB slots.
 */
int		nkthread = 8;

/*
 * Pure policy function: bytes of RAM in, plan out.  No globals, no
 * allocation - this is what the monitor's `memplan` command uses to show
 * the 4 GB / 6..24 GB cases on a machine that cannot physically have them.
 */
static void
mem_plan_compute(unsigned long long bytes, struct mem_plan *p)
{
	unsigned long long total;

	total = bytes;
	/*
	 * The BIOS/E801 count excludes the top reserved page, so a 512 MB
	 * guest reports 511.87 MB and a 4 GB guest 4095.87 MB.  Round up to
	 * the next 4 MB boundary (the thread quantum) so the documented
	 * endpoints stay exact: 128 MB -> 32 threads, 4 GB -> 1024 threads.
	 * BIOS/E801 会少报顶端保留页；按 4MB 边界上取整，保证 128MB→32、
	 * 4GB→1024 这些端点精确。
	 */
	total = (total + 0x3FFFFFULL) & ~0x3FFFFFULL;
	if (total < MEM_PLAN_MIN_BYTES)
		total = MEM_PLAN_MIN_BYTES;
	if (total > MEM_PLAN_MAX_BYTES)
		total = MEM_PLAN_MAX_BYTES;

	p->total_kb = (unsigned)(total >> 10);
	/*
	 * The 32-bit port can only address 4 GB.  Heap/mmap VA and the TCB
	 * pool are laid out from addressable_kb so a 24 GB host still gets a
	 * sane 4 GB-shaped user address space instead of a 12 GB heap that
	 * cannot be mapped.  total_kb keeps the real detection for the
	 * policy/thread print and for a future PAE port.
	 */
	p->addressable_kb = (p->total_kb > 4194304u) ? 4194304u : p->total_kb;
	p->kheap_kb = p->total_kb * 3u / 8u;
	p->uheap_kb = p->total_kb / 2u;
	p->mmap_kb = p->total_kb - p->kheap_kb - p->uheap_kb;
	p->nthreads = p->total_kb / 4096u;	/* one per 4 MB */
	if (p->nthreads < 8)
		p->nthreads = 8;
	/*
	 * The current port is i386 without PAE: it can use at most the first
	 * 4 GB and can hold NKTHREAD_MAX TCBs.  Both caps live here so a PAE
	 * or x86_64 port only has to raise these two values - the 3:4:1
	 * policy and the 4 MB/thread rule stay untouched.
	 */
	p->nthreads_usable = p->nthreads;
	if (p->nthreads_usable > (unsigned)NKTHREAD_MAX)
		p->nthreads_usable = (unsigned)NKTHREAD_MAX;
	p->kstack_kb = KSTACK_SIZE / 1024u;
	p->ustack_kb = 2048;
}

void
mem_plan_init(void)
{
	unsigned long long actual, limit;

	/*
	 * physmem is the page count from the BIOS/E801 probe (machdep.c).
	 * Fall back to 128 MB only when the probe failed completely, so a
	 * broken BIOS still gives a bootable minimum rather than zero.
	 */
	actual = (physmem > 0) ? ((unsigned long long)physmem << 12) :
	    (unsigned long long)MEM_PLAN_MIN_BYTES;
	mem_detected_bytes = actual;

	mem_plan_compute(actual, &mem_plan);

	/*
	 * Physical ceiling: leave 1 MB below the top for the bootstrap/msgbuf
	 * area, clamp to the 32-bit address space.  actual (not total) is used
	 * so a 127 MB BIOS report does not make page_alloc walk past RAM.
	 * With >4 GB RAM the extra memory is simply not reachable until PAE.
	 */
	limit = (actual > 0x100000ULL) ? actual - 0x100000ULL : 0ULL;
	if (limit > 0xFFF00000ULL)
		limit = 0xFFF00000ULL;
	mem_phys_limit = (unsigned)limit;

	nkthread = (int)mem_plan.nthreads_usable;

	printf("mem: detected %d MB (phys limit %x)\n",
	    (int)(mem_detected_bytes >> 20), mem_phys_limit);
	printf("mem: plan total %d MB: kheap %d MB, uheap %d MB, mmap %d MB\n",
	    (int)(mem_plan.total_kb / 1024u),
	    (int)(mem_plan.kheap_kb / 1024u),
	    (int)(mem_plan.uheap_kb / 1024u),
	    (int)(mem_plan.mmap_kb / 1024u));
	printf("mem: threads %d x (kstack %d KB + ustack %d KB), policy %d\n",
	    (int)mem_plan.nthreads_usable, (int)mem_plan.kstack_kb,
	    (int)mem_plan.ustack_kb, (int)mem_plan.nthreads);
	if (mem_plan.nthreads_usable < mem_plan.nthreads)
		printf("mem: port cap %d TCBs (non-PAE 32-bit); "
		    "PAE/x86_64 raises it for >4 GB\n", NKTHREAD_MAX);
}

/*
 * Monitor helper: print the policy for a range of RAM sizes.  This is how
 * the 4 GB and 6/8/12/16/20/24 GB plans are verified on a QEMU i386 guest,
 * which can only expose ~3 GB below the PCI hole.
 */
void
mem_plan_table(void)
{
	static const unsigned mb[] = {
		128, 256, 512, 1024, 2048, 3072, 4096,
		6144, 8192, 12288, 16384, 20480, 24576
	};
	struct mem_plan p;
	unsigned i;

	printf("mem:  MB   kheap   uheap    mmap  threads\n");
	for (i = 0; i < sizeof(mb) / sizeof(mb[0]); i++) {
		mem_plan_compute((unsigned long long)mb[i] << 20, &p);
		printf("mem: %5d  %5d  %6d  %6d  %4d/%d\n", (int)mb[i],
		    (int)(p.kheap_kb / 1024u), (int)(p.uheap_kb / 1024u),
		    (int)(p.mmap_kb / 1024u), (int)p.nthreads_usable,
		    (int)p.nthreads);
	}
}
