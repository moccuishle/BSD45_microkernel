/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * timer.c - i386 PIT channel 0 at 100 Hz (formerly kernel/hal/clock.c +
 * arch/i386/hal/clock_arch.c).
 *
 * No IPL layer: IRQ0 enters with IF cleared by the interrupt gate, bumps the
 * tick counter, sets need_resched on quantum expiry, sends EOI, and calls
 * sched_tick() for user-mode contexts.  The old IPL wrapping and the
 * registered IPL_CLOCK action are gone; this is a plain tick handler.
 */
#include <sys/param.h>
#include <sys/systm.h>

#include <machine/segments.h>

#include "arch/i386/io.h"
#include "sys/platform.h"

extern void sched_tick(struct sysregs *r);
/* S413: need_resched is per-CPU now; set it through the scheduler. */
extern void smp_need_resched(void);
extern unsigned sched_switches;
extern void irq0_entry(void);
extern void setidt(int idx, char *func, int typ, int dpl);

#define	CLOCK_HZ	100
#define	PIT_CH0		0x40
#define	PIT_CMD		0x43
#define	PIT_FREQ	1193182
#define	PIC_EOI		0x20
#define	IRQ_VEC_BASE	0x20

static unsigned	clock_ticks;
static unsigned	clock_quantum = 2;	/* ticks per scheduling slice */
static unsigned	lat_last, lat_min = ~0u, lat_max, lat_sum, lat_n;

/* EN: Send end-of-interrupt to the 8259 PIC (IRQ0). */
void
hal_clock_eoi(void)
{

	hal_outb(PIC_EOI, 0x20);
}

/* EN: Print TSC jitter statistics between consecutive clock ticks (min/max/avg cycles). */
void
clock_lat_print(void)
{

	printf("lat: samples %d min %d max %d avg %d cycles\n",
	    lat_n, lat_n ? lat_min : 0, lat_max,
	    lat_n ? lat_sum / lat_n : 0);
}

/* EN: Return the cumulative 100 Hz PIT tick counter. */
unsigned
clock_get_ticks(void)
{

	return (clock_ticks);
}

/* EN: Return the scheduling quantum in ticks (default 2). */
unsigned
clock_quantum_get(void)
{

	return (clock_quantum);
}

/*
 * IRQ0 handler, called from irq0_entry() with a struct sysregs on the stack.
 * No IPL raise/restore: the trap gate already cleared IF, so this handler is
 * non-preemptable by construction.
 */
void
clock_irq(struct sysregs *r)
{

	clock_ticks++;
	{
		unsigned now = hal_rdtsc();

		if (lat_last != 0) {
			unsigned d = now - lat_last;

			if (d < lat_min)
				lat_min = d;
			if (d > lat_max)
				lat_max = d;
			lat_sum += d;
			lat_n++;
		}
		lat_last = now;
	}

	if (clock_quantum == 0 || (clock_ticks % clock_quantum) == 0)
		smp_need_resched();

	/* end of interrupt before we possibly switch address spaces */
	hal_clock_eoi();

	if (r != 0 && (r->cs & 3) == 3)
		sched_tick(r);
}

/* EN: Initialize the 8259 PICs: cascade setup, vectors 0x20-0x2f, all masked. */
static void
pic_init(void)
{

	hal_outb(0x20, 0x11); hal_outb(0x80, 0);
	hal_outb(0xA0, 0x11); hal_outb(0x80, 0);
	hal_outb(0x21, 0x20); hal_outb(0x80, 0);	/* master vectors 0x20 */
	hal_outb(0xA1, 0x28); hal_outb(0x80, 0);	/* slave vectors 0x28 */
	hal_outb(0x21, 0x04); hal_outb(0x80, 0);	/* cascade on IRQ2 */
	hal_outb(0xA1, 0x02); hal_outb(0x80, 0);
	hal_outb(0x21, 0x01); hal_outb(0x80, 0);
	hal_outb(0xA1, 0x01); hal_outb(0x80, 0);
	hal_outb(0x21, 0xff);			/* mask all lines */
	hal_outb(0xA1, 0xff);
}

/* EN: Program PIT channel 0, install the IRQ0 IDT gate and unmask IRQ0. */
void
clock_init(void)
{
	unsigned div;

	pic_init();
	setidt(IRQ_VEC_BASE + 0, (char *)&irq0_entry, SDT_SYS386TGT, SEL_KPL);
	/* enable IRQ0 on the master PIC (currently all lines masked with 0xff) */
	hal_outb(0x21, 0xfe);
	div = PIT_FREQ / CLOCK_HZ;
	hal_outb(PIT_CMD, 0x34);		/* ch0, lo/hi, mode 2 */
	hal_outb(PIT_CH0, div & 0xff);
	hal_outb(PIT_CH0, (div >> 8) & 0xff);
	printf("clock: %d Hz, IRQ0 enabled, quantum %d ticks\n",
	    CLOCK_HZ, clock_quantum);
}

/* EN: Print tick counter, PIT rate, quantum and switch count. */
void
clock_print(void)
{

	printf("clock: ticks %d hz %d quantum %d switches %d\n",
	    clock_ticks, CLOCK_HZ, clock_quantum, sched_switches);
}
