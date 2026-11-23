/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * mem_plan.h - S411: boot-time memory budget planner.
 *
 * The kernel used to size its thread pool and user address space from
 * compile-time constants.  That made a 128 MB guest over-provisioned and a
 * 4 GB guest under-provisioned.  This module derives one plan from the
 * BIOS-reported physmem and hands it to the thread / VM layers:
 *
 *	total    = detected RAM, clamped to [128 MB, 24 GB]
 *	kheap    = total * 3/8     (1.5 GB at 4 GB, 48 MB at 128 MB)
 *	uheap    = total * 1/2     (2.0 GB at 4 GB, 64 MB at 128 MB)
 *	mmap     = total * 1/8     (0.5 GB at 4 GB, 16 MB at 128 MB)
 *	threads  = total / 4 MB    (1024 at 4 GB, 6144 at 24 GB)
 *
 * Every thread owns a fixed 16 KB kernel stack and a fixed 2 MB user stack;
 * the stacks are demand-paged, so the heap numbers above are *caps*, not
 * physical reservations.  The 3:4:1 split keeps the 4 GB numbers exact and
 * scales the 128 MB case by the same ratio (the "另外算" requirement).
 *
 * Memory is detected at boot (physmem from BIOS/E801), never compiled in,
 * and the policy deliberately scales past 4 GB so a future 6/8/12/16/20/24
 * GB machine needs no policy change.  The current i386 port is non-PAE, so
 * only the first 4 GB is addressable: nthreads_usable is what this port can
 * actually run today, while nthreads is what the policy would allow once
 * PAE (36-bit) or an x86_64 port lands.
 */
#ifndef _VM_MEM_PLAN_H_
#define _VM_MEM_PLAN_H_

#define	MEM_PLAN_MIN_BYTES	(128ULL * 1024 * 1024)
#define	MEM_PLAN_MAX_BYTES	(24ULL * 1024 * 1024 * 1024)

struct mem_plan {
	unsigned	total_kb;	/* detected RAM, clamped to [128 MB, 24 GB] */
	unsigned	addressable_kb;	/* min(total, 4 GB) for this non-PAE port */
	unsigned	kheap_kb;	/* kernel heap budget (3/8) */
	unsigned	uheap_kb;	/* user heap budget   (1/2) */
	unsigned	mmap_kb;	/* mmap budget        (1/8) */
	unsigned	nthreads;	/* policy: total / 4 MB */
	unsigned	nthreads_usable;/* min(policy, current port ceiling) */
	unsigned	kstack_kb;	/* fixed 16 KB */
	unsigned	ustack_kb;	/* fixed 2 MB */
};

extern struct mem_plan	mem_plan;
extern unsigned		mem_phys_limit;	/* highest usable physical address */
extern unsigned long long mem_detected_bytes;	/* raw BIOS/E801 detection */

void	mem_plan_init(void);
void	mem_plan_table(void);	/* monitor: policy table for 128 MB .. 24 GB */

#endif /* _VM_MEM_PLAN_H_ */
