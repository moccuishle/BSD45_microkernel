/*
 * stubs.c - minimal kernel services that the stand-alone bring-up
 * kernel needs in order to link the genuine 4.4BSD machine-dependent
 * code (locore.s, pmap.c, machdep.c, cons.c, icu.s).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/kprocess.h>
#include <sys/malloc.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>

#include <machine/cpu.h>
#include <machine/pmap.h>

int	bootdev;
int	page_shift = 12;
vm_size_t page_mask = 0xfff;
boolean_t vm_page_startup_initialized;

int	astpending;
int	want_resched;
int	whichqs;
int	netisr;

/*
 * pmap.c's debug printf macro (PMAP_DEBUG is off in a normal build).
 */
/* 中文：pmap 调试 printf 宏的空桩实现（正常构建下 PMAP_DEBUG 关闭），恒返回 0 */
/* EN: No-op stub for pmap's debug printf macro (PMAP_DEBUG is off in normal builds), always returning 0 */
int
pg(const char *fmt, ...)
{

	return (0);
}

/*
 * Classify interrupts / timer callbacks.  The bring-up kernel keeps the
 * PIC masked and polls, so these are never entered.
 */
/* 中文：原始中断入口的空桩：引导内核屏蔽 PIC 并轮询，故永不进入，返回 0 */
/* EN: No-op stub for the raw interrupt entry: the bring-up kernel masks the PIC and polls, so it is never entered; returns 0 */
int
rawintr(int unit)
{

	return (0);
}

/* 中文：ISA 杂散中断处理桩：仅打印一条杂散中断提示 */
/* EN: Stub ISA stray-interrupt handler that only prints a stray-interrupt message */
void
isa_strayintr(int d)
{

	printf("stray ISA interrupt %d\n", d);
}

/* 中文：软时钟处理的空桩，引导内核中不使用 */
/* EN: No-op stub for softclock processing, unused in the bring-up kernel */
void
softclock(void)
{
}

/*
 * kernel malloc: a bump allocator over a static arena.  Only pmap's pv
 * entry allocation uses it, and the monitor never grows it without bound.
 */
#define	HEAP_SIZE	(256 * 1024)
static char	heap[HEAP_SIZE] __attribute__((aligned(16)));
static unsigned	heap_used;

/* 中文：内核 malloc：在静态堆上做按 16 字节对齐的 bump 分配，堆耗尽时 panic */
/* EN: Kernel malloc: a 16-byte-aligned bump allocator over a static heap, panicking when the heap is exhausted */
void *
malloc(unsigned long size, int type, int flags)
{
	void *p;

	size = (size + 15) & ~15;
	if (heap_used + size > HEAP_SIZE)
		panic("malloc: out of kernel heap");
	p = heap + heap_used;
	heap_used += size;
	return (p);
}

/* 中文：内核 free 的空桩：bump 分配器不回收内存 */
/* EN: No-op kernel free stub: the bump allocator does not reclaim memory */
void
free(void *addr, int type)
{
	/* bump allocator: no-op */
}

/* 中文：blkclr 桩：对指定内存块调用 bzero 清零 */
/* EN: blkclr stub that zero-fills a memory block via bzero */
void
blkclr(void *p, unsigned int len)
{

	bzero(p, len);
}

/* mingw/gcc inserts a call to __main() at the start of main(). */
/* 中文：__main 空桩：满足 mingw/gcc 在 main() 开头插入的 __main() 调用 */
/* EN: No-op __main stub to satisfy the __main() call mingw/gcc inserts at the start of main */
void
__main(void)
{
}
