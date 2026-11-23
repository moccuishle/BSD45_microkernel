/*-
 * Copyright (c) 1982, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 *	@(#)subr_prof.c	8.4 (Berkeley) 2/14/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kprocess.h>
#include <sys/user.h>

#include <sys/mount.h>
#include <sys/syscallargs.h>

#include <machine/cpu.h>

#ifdef GPROF
#include <sys/malloc.h>
#include <sys/gmon.h>

/*
 * Froms is actually a bunch of unsigned shorts indexing tos
 */
struct gmonparam _gmonparam = { GMON_PROF_OFF };

extern char etext[];

/* 中文：启动内核性能剖析：计算代码段直方图大小，分配并清零 gmon 缓冲区，设置采样与调用图参数 */
/* EN: Start kernel profiling: compute the text-segment histogram size, allocate and zero the gmon buffers, and set up sampling and call-graph parameters */
void
kmstartup()
{
	char *cp;
	struct gmonparam *p = &_gmonparam;
	/*
	 * Round lowpc and highpc to multiples of the density we're using
	 * so the rest of the scaling (here and in gprof) stays in ints.
	 */
	p->lowpc = ROUNDDOWN(KERNBASE, HISTFRACTION * sizeof(HISTCOUNTER));
	p->highpc = ROUNDUP((u_long)etext, HISTFRACTION * sizeof(HISTCOUNTER));
	p->textsize = p->highpc - p->lowpc;
	printf("Profiling kernel, textsize=%d [%x..%x]\n",
	       p->textsize, p->lowpc, p->highpc);
	p->kcountsize = p->textsize / HISTFRACTION;
	p->hashfraction = HASHFRACTION;
	p->fromssize = p->textsize / HASHFRACTION;
	p->tolimit = p->textsize * ARCDENSITY / 100;
	if (p->tolimit < MINARCS)
		p->tolimit = MINARCS;
	else if (p->tolimit > MAXARCS)
		p->tolimit = MAXARCS;
	p->tossize = p->tolimit * sizeof(struct tostruct);
	cp = (char *)malloc(p->kcountsize + p->fromssize + p->tossize,
	    M_GPROF, M_NOWAIT);
	if (cp == 0) {
		printf("No memory for profiling.\n");
		return;
	}
	bzero(cp, p->kcountsize + p->tossize + p->fromssize);
	p->tos = (struct tostruct *)cp;
	cp += p->tossize;
	p->kcount = (u_short *)cp;
	cp += p->kcountsize;
	p->froms = (u_short *)cp;
}

/*
 * Return kernel profiling information.
 */
/* 中文：读取或设置内核性能剖析的 sysctl 参数：开关状态、直方图、froms、tos 以及 gmonparam 结构 */
/* EN: Get or set kernel profiling sysctl parameters: on/off state, histogram, froms, tos and the gmonparam structure */
int
sysctl_doprof(name, namelen, oldp, oldlenp, newp, newlen, p)
	int *name;
	u_int namelen;
	void *oldp;
	size_t *oldlenp;
	void *newp;
	size_t newlen;
{
	struct gmonparam *gp = &_gmonparam;
	int error;

	/* all sysctl names at this level are terminal */
	if (namelen != 1)
		return (ENOTDIR);		/* overloaded */

	switch (name[0]) {
	case GPROF_STATE:
		error = sysctl_int(oldp, oldlenp, newp, newlen, &gp->state);
		if (error)
			return (error);
		if (gp->state == GMON_PROF_OFF)
			stopprofclock(&proc0);
		else
			startprofclock(&proc0);
		return (0);
	case GPROF_COUNT:
		return (sysctl_struct(oldp, oldlenp, newp, newlen,
		    gp->kcount, gp->kcountsize));
	case GPROF_FROMS:
		return (sysctl_struct(oldp, oldlenp, newp, newlen,
		    gp->froms, gp->fromssize));
	case GPROF_TOS:
		return (sysctl_struct(oldp, oldlenp, newp, newlen,
		    gp->tos, gp->tossize));
	case GPROF_GMONPARAM:
		return (sysctl_rdstruct(oldp, oldlenp, newp, gp, sizeof *gp));
	default:
		return (EOPNOTSUPP);
	}
	/* NOTREACHED */
}
#endif /* GPROF */

/*
 * Profiling system call.
 *
 * The scale factor is a fixed point number with 16 bits of fraction, so that
 * 1.0 is represented as 0x10000.  A scale factor of 0 turns off profiling.
 */
/* 中文：profil 系统调用：按给定缩放因子、偏移和样本缓冲区启动用户态性能剖析，scale 为 0 时关闭剖析 */
/* EN: The profil system call: start user-level profiling with the given scale, offset and sample buffer, or turn profiling off when scale is 0 */
/* ARGSUSED */
int
profil(p, uap, retval)
	struct process *p;
	register struct profil_args /* {
		syscallarg(caddr_t) samples;
		syscallarg(u_int) size;
		syscallarg(u_int) offset;
		syscallarg(u_int) scale;
	} */ *uap;
	register_t *retval;
{
	register struct uprof *upp;
	int s;

	if (SCARG(uap, scale) > (1 << 16))
		return (EINVAL);
	if (SCARG(uap, scale) == 0) {
		stopprofclock(p);
		return (0);
	}
	upp = &p->p_stats->p_prof;

	/* Block profile interrupts while changing state. */
	s = splstatclock();
	upp->pr_off = SCARG(uap, offset);
	upp->pr_scale = SCARG(uap, scale);
	upp->pr_base = SCARG(uap, samples);
	upp->pr_size = SCARG(uap, size);
	startprofclock(p);
	splx(s);

	return (0);
}

/*
 * Scale is a fixed-point number with the binary point 16 bits
 * into the value, and is <= 1.0.  pc is at most 32 bits, so the
 * intermediate result is at most 48 bits.
 */
#define	PC_TO_INDEX(pc, prof) \
	((int)(((u_quad_t)((pc) - (prof)->pr_off) * \
	    (u_quad_t)((prof)->pr_scale)) >> 16) & ~1)

/*
 * Collect user-level profiling statistics; called on a profiling tick,
 * when a process is running in user-mode.  This routine may be called
 * from an interrupt context.  We try to update the user profiling buffers
 * cheaply with fuswintr() and suswintr().  If that fails, we revert to
 * an AST that will vector us to trap() with a context in which copyin
 * and copyout will work.  Trap will then call addupc_task().
 *
 * Note that we may (rarely) not get around to the AST soon enough, and
 * lose profile ticks when the next tick overwrites this one, but in this
 * case the system is overloaded and the profile is probably already
 * inaccurate.
 */
/* 中文：在剖析时钟中断上下文中更新用户态采样计数，若 fuswintr/suswintr 失败则记录 pc 并请求 AST 延后处理 */
/* EN: Update the user-level profile sample count in a profiling clock interrupt; if fuswintr/suswintr fail, record the pc and request an AST for later handling */
void
addupc_intr(p, pc, ticks)
	register struct process *p;
	register u_long pc;
	u_int ticks;
{
	register struct uprof *prof;
	register caddr_t addr;
	register u_int i;
	register int v;

	if (ticks == 0)
		return;
	prof = &p->p_stats->p_prof;
	if (pc < prof->pr_off ||
	    (i = PC_TO_INDEX(pc, prof)) >= prof->pr_size)
		return;			/* out of range; ignore */

	addr = prof->pr_base + i;
	if ((v = fuswintr(addr)) == -1 || suswintr(addr, v + ticks) == -1) {
		prof->pr_addr = pc;
		prof->pr_ticks = ticks;
		need_proftick(p);
	}
}

/*
 * Much like before, but we can afford to take faults here.  If the
 * update fails, we simply turn off profiling.
 */
/* 中文：在允许睡眠的上下文中用 copyin/copyout 更新用户态采样计数，更新失败则关闭该进程的性能剖析 */
/* EN: Update the user-level profile sample count via copyin/copyout in a fault-capable context, turning off profiling for the process if the update fails */
void
addupc_task(p, pc, ticks)
	register struct process *p;
	register u_long pc;
	u_int ticks;
{
	register struct uprof *prof;
	register caddr_t addr;
	register u_int i;
	u_short v;

	/* Testing P_PROFIL may be unnecessary, but is certainly safe. */
	if ((p->p_flag & P_PROFIL) == 0 || ticks == 0)
		return;

	prof = &p->p_stats->p_prof;
	if (pc < prof->pr_off ||
	    (i = PC_TO_INDEX(pc, prof)) >= prof->pr_size)
		return;

	addr = prof->pr_base + i;
	if (copyin(addr, (caddr_t)&v, sizeof(v)) == 0) {
		v += ticks;
		if (copyout((caddr_t)&v, addr, sizeof(v)) == 0)
			return;
	}
	stopprofclock(p);
}
