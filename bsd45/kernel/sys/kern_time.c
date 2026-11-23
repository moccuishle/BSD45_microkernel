/*
 * Copyright (c) 1982, 1986, 1989, 1993
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
 *	@(#)kern_time.c	8.4 (Berkeley) 5/26/95
 */

#include <sys/param.h>
#include <sys/resourcevar.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/kprocess.h>
#include <sys/vnode.h>

#include <sys/mount.h>
#include <sys/syscallargs.h>

#include <machine/cpu.h>

/* 
 * Time of day and interval timer support.
 *
 * These routines provide the kernel entry points to get and set
 * the time-of-day and per-process interval timers.  Subroutines
 * here provide support for adding and subtracting timeval structures
 * and decrementing interval timers, optionally reloading the interval
 * timers when they expire.
 */

/* ARGSUSED */
/* 中文：实现 gettimeofday 系统调用：把当前时间（及调用者请求的时区）拷贝回用户空间 */
/* EN: Implement the gettimeofday system call: copy the current time (and timezone when requested) back to user space */
int
gettimeofday(p, uap, retval)
	struct process *p;
	register struct gettimeofday_args /* {
		syscallarg(struct timeval *) tp;
		syscallarg(struct timezone *) tzp;
	} */ *uap;
	register_t *retval;
{
	struct timeval atv;
	int error = 0;

	if (SCARG(uap, tp)) {
		microtime(&atv);
		if (error = copyout((caddr_t)&atv, (caddr_t)SCARG(uap, tp),
		    sizeof (atv)))
			return (error);
	}
	if (SCARG(uap, tzp))
		error = copyout((caddr_t)&tz, (caddr_t)SCARG(uap, tzp),
		    sizeof (tz));
	return (error);
}

/* ARGSUSED */
/* 中文：实现 settimeofday 系统调用：仅特权进程可用，先拷贝并校验所有参数，再更新系统时间与时区 */
/* EN: Implement the settimeofday system call: restricted to the superuser, copy in and validate all parameters before updating system time and timezone */
int
settimeofday(p, uap, retval)
	struct process *p;
	struct settimeofday_args /* {
		syscallarg(struct timeval *) tv;
		syscallarg(struct timezone *) tzp;
	} */ *uap;
	register_t *retval;
{
	struct timeval atv, delta;
	struct timezone atz;
	int error, s;

	if (error = suser(p->p_ucred, &p->p_acflag))
		return (error);
	/* Verify all parameters before changing time. */
	if (SCARG(uap, tv) && (error = copyin((caddr_t)SCARG(uap, tv),
	    (caddr_t)&atv, sizeof(atv))))
		return (error);
	if (SCARG(uap, tzp) && (error = copyin((caddr_t)SCARG(uap, tzp),
	    (caddr_t)&atz, sizeof(atz))))
		return (error);
	if (SCARG(uap, tv)) {
		/*
		 * If the system is secure, we do not allow the time to be 
		 * set to an earlier value (it may be slowed using adjtime,
		 * but not set back). This feature prevent interlopers from
		 * setting arbitrary time stamps on files.
		 */
		if (securelevel > 0 && timercmp(&atv, &time, <))
			return (EPERM);
		/* WHAT DO WE DO ABOUT PENDING REAL-TIME TIMEOUTS??? */
		s = splclock();
		/* nb. delta.tv_usec may be < 0, but this is OK here */
		delta.tv_sec = atv.tv_sec - time.tv_sec;
		delta.tv_usec = atv.tv_usec - time.tv_usec;
		time = atv;
		(void) splsoftclock();
		timevaladd(&boottime, &delta);
		timevalfix(&boottime);
		timevaladd(&runtime, &delta);
		timevalfix(&runtime);
#		ifdef NFS
			lease_updatetime(delta.tv_sec);
#		endif
		splx(s);
		resettodr();
	}
	if (SCARG(uap, tzp))
		tz = atz;
	return (0);
}

extern	int tickadj;			/* "standard" clock skew, us./tick */
int	tickdelta;			/* current clock skew, us. per tick */
long	timedelta;			/* unapplied time correction, us. */
long	bigadj = 1000000;		/* use 10x skew above bigadj us. */

/* ARGSUSED */
/* 中文：实现 adjtime 系统调用：以平滑方式逐步校正系统时间，并返回上一次尚未完成的校正量 */
/* EN: Implement the adjtime system call: slew the system time gradually and return the previous unfinished adjustment */
int
adjtime(p, uap, retval)
	struct process *p;
	register struct adjtime_args /* {
		syscallarg(struct timeval *) delta;
		syscallarg(struct timeval *) olddelta;
	} */ *uap;
	register_t *retval;
{
	struct timeval atv;
	register long ndelta, ntickdelta, odelta;
	int s, error;

	if (error = suser(p->p_ucred, &p->p_acflag))
		return (error);
	if (error = copyin((caddr_t)SCARG(uap, delta), (caddr_t)&atv,
	    sizeof(struct timeval)))
		return (error);

	/*
	 * Compute the total correction and the rate at which to apply it.
	 * Round the adjustment down to a whole multiple of the per-tick
	 * delta, so that after some number of incremental changes in
	 * hardclock(), tickdelta will become zero, lest the correction
	 * overshoot and start taking us away from the desired final time.
	 */
	ndelta = atv.tv_sec * 1000000 + atv.tv_usec;
	if (ndelta > bigadj)
		ntickdelta = 10 * tickadj;
	else
		ntickdelta = tickadj;
	if (ndelta % ntickdelta)
		ndelta = ndelta / ntickdelta * ntickdelta;

	/*
	 * To make hardclock()'s job easier, make the per-tick delta negative
	 * if we want time to run slower; then hardclock can simply compute
	 * tick + tickdelta, and subtract tickdelta from timedelta.
	 */
	if (ndelta < 0)
		ntickdelta = -ntickdelta;
	s = splclock();
	odelta = timedelta;
	timedelta = ndelta;
	tickdelta = ntickdelta;
	splx(s);

	if (SCARG(uap, olddelta)) {
		atv.tv_sec = odelta / 1000000;
		atv.tv_usec = odelta % 1000000;
		(void) copyout((caddr_t)&atv, (caddr_t)SCARG(uap, olddelta),
		    sizeof(struct timeval));
	}
	return (0);
}

/*
 * Get value of an interval timer.  The process virtual and
 * profiling virtual time timers are kept in the p_stats area, since
 * they can be swapped out.  These are kept internally in the
 * way they are specified externally: in time until they expire.
 *
 * The real time interval timer is kept in the process table slot
 * for the process, and its value (it_value) is kept as an
 * absolute time rather than as a delta, so that it is easy to keep
 * periodic real-time signals from drifting.
 *
 * Virtual time timers are processed in the hardclock() routine of
 * kern_clock.c.  The real time timer is processed by a timeout
 * routine, called from the softclock() routine.  Since a callout
 * may be delayed in real time due to interrupt processing in the system,
 * it is possible for the real time timeout routine (realitexpire, given below),
 * to be delayed in real time past when it is supposed to occur.  It
 * does not suffice, therefore, to reload the real timer .it_value from the
 * real time timers .it_interval.  Rather, we compute the next time in
 * absolute time the timer should go off.
 */
/* ARGSUSED */
/* 中文：实现 getitimer 系统调用：读取指定间隔定时器（实时/虚拟/性能）的当前值并返回给用户 */
/* EN: Implement the getitimer system call: read the current value of the selected interval timer (real/virtual/profiling) and return it to userland */
int
getitimer(p, uap, retval)
	struct process *p;
	register struct getitimer_args /* {
		syscallarg(u_int) which;
		syscallarg(struct itimerval *) itv;
	} */ *uap;
	register_t *retval;
{
	struct itimerval aitv;
	int s;

	if (SCARG(uap, which) > ITIMER_PROF)
		return (EINVAL);
	s = splclock();
	if (SCARG(uap, which) == ITIMER_REAL) {
		/*
		 * Convert from absolute to relative time in .it_value
		 * part of real time timer.  If time for real time timer
		 * has passed return 0, else return difference between
		 * current time and time for the timer to go off.
		 */
		aitv = p->p_realtimer;
		if (timerisset(&aitv.it_value))
			if (timercmp(&aitv.it_value, &time, <))
				timerclear(&aitv.it_value);
			else
				timevalsub(&aitv.it_value,
				    (struct timeval *)&time);
	} else
		aitv = p->p_stats->p_timer[SCARG(uap, which)];
	splx(s);
	return (copyout((caddr_t)&aitv, (caddr_t)SCARG(uap, itv),
	    sizeof (struct itimerval)));
}

/* ARGSUSED */
/* 中文：实现 setitimer 系统调用：设置指定间隔定时器的当前值与周期，并返回原值 */
/* EN: Implement the setitimer system call: set the current value and interval of the selected timer and return the old value */
int
setitimer(p, uap, retval)
	struct process *p;
	register struct setitimer_args /* {
		syscallarg(u_int) which;
		syscallarg(struct itimerval *) itv;
		syscallarg(struct itimerval *) oitv;
	} */ *uap;
	register_t *retval;
{
	struct itimerval aitv;
	register struct itimerval *itvp;
	int s, error;

	if (SCARG(uap, which) > ITIMER_PROF)
		return (EINVAL);
	itvp = SCARG(uap, itv);
	if (itvp && (error = copyin((caddr_t)itvp, (caddr_t)&aitv,
	    sizeof(struct itimerval))))
		return (error);
	if ((SCARG(uap, itv) = SCARG(uap, oitv)) &&
	    (error = getitimer(p, uap, retval)))
		return (error);
	if (itvp == 0)
		return (0);
	if (itimerfix(&aitv.it_value) || itimerfix(&aitv.it_interval))
		return (EINVAL);
	s = splclock();
	if (SCARG(uap, which) == ITIMER_REAL) {
		untimeout(realitexpire, (caddr_t)p);
		if (timerisset(&aitv.it_value)) {
			timevaladd(&aitv.it_value, (struct timeval *)&time);
			timeout(realitexpire, (caddr_t)p, hzto(&aitv.it_value));
		}
		p->p_realtimer = aitv;
	} else
		p->p_stats->p_timer[SCARG(uap, which)] = aitv;
	splx(s);
	return (0);
}

/*
 * Real interval timer expired:
 * send process whose timer expired an alarm signal.
 * If time is not set up to reload, then just return.
 * Else compute next time timer should go off which is > current time.
 * This is where delay in processing this timeout causes multiple
 * SIGALRM calls to be compressed into one.
 */
/* 中文：真实间隔定时器到期处理：向进程发送 SIGALRM，若设置了周期则重新装载定时器并对处理延迟进行补偿 */
/* EN: Real interval timer expiry handler: send SIGALRM to the process and, if periodic, reload the timer compensating for processing delay */
void
realitexpire(arg)
	void *arg;
{
	register struct process *p;
	int s;

	p = (struct process *)arg;
	psignal(p, SIGALRM);
	if (!timerisset(&p->p_realtimer.it_interval)) {
		timerclear(&p->p_realtimer.it_value);
		return;
	}
	for (;;) {
		s = splclock();
		timevaladd(&p->p_realtimer.it_value,
		    &p->p_realtimer.it_interval);
		if (timercmp(&p->p_realtimer.it_value, &time, >)) {
			timeout(realitexpire, (caddr_t)p,
			    hzto(&p->p_realtimer.it_value));
			splx(s);
			return;
		}
		splx(s);
	}
}

/*
 * Check that a proposed value to load into the .it_value or
 * .it_interval part of an interval timer is acceptable, and
 * fix it to have at least minimal value (i.e. if it is less
 * than the resolution of the clock, round it up.)
 */
/* 中文：校验并修正待装入间隔定时器的时间值：范围检查，并把小于时钟分辨率的正值向上取整 */
/* EN: Validate and fix a timeval to be loaded into an interval timer: range-check it and round sub-resolution positive values up to one tick */
int
itimerfix(tv)
	struct timeval *tv;
{

	if (tv->tv_sec < 0 || tv->tv_sec > 100000000 ||
	    tv->tv_usec < 0 || tv->tv_usec >= 1000000)
		return (EINVAL);
	if (tv->tv_sec == 0 && tv->tv_usec != 0 && tv->tv_usec < tick)
		tv->tv_usec = tick;
	return (0);
}

/*
 * Decrement an interval timer by a specified number
 * of microseconds, which must be less than a second,
 * i.e. < 1000000.  If the timer expires, then reload
 * it.  In this case, carry over (usec - old value) to
 * reduce the value reloaded into the timer so that
 * the timer does not drift.  This routine assumes
 * that it is called in a context where the timers
 * on which it is operating cannot change in value.
 */
/* 中文：按给定的微秒数递减间隔定时器，到期时按周期重装并把余量结转到新值以避免漂移 */
/* EN: Decrement an interval timer by the given microseconds, reloading it at expiry and carrying over the remainder to avoid drift */
int
itimerdecr(itp, usec)
	register struct itimerval *itp;
	int usec;
{

	if (itp->it_value.tv_usec < usec) {
		if (itp->it_value.tv_sec == 0) {
			/* expired, and already in next interval */
			usec -= itp->it_value.tv_usec;
			goto expire;
		}
		itp->it_value.tv_usec += 1000000;
		itp->it_value.tv_sec--;
	}
	itp->it_value.tv_usec -= usec;
	usec = 0;
	if (timerisset(&itp->it_value))
		return (1);
	/* expired, exactly at end of interval */
expire:
	if (timerisset(&itp->it_interval)) {
		itp->it_value = itp->it_interval;
		itp->it_value.tv_usec -= usec;
		if (itp->it_value.tv_usec < 0) {
			itp->it_value.tv_usec += 1000000;
			itp->it_value.tv_sec--;
		}
	} else
		itp->it_value.tv_usec = 0;		/* sec is already 0 */
	return (0);
}

/*
 * Add and subtract routines for timevals.
 * N.B.: subtract routine doesn't deal with
 * results which are before the beginning,
 * it just gets very confused in this case.
 * Caveat emptor.
 */
/* 中文：把 t2 加到 t1 上（秒与微秒分别相加后规整进位） */
/* EN: Add t2 to t1 (add seconds and microseconds separately, then normalize any carry) */
timevaladd(t1, t2)
	struct timeval *t1, *t2;
{

	t1->tv_sec += t2->tv_sec;
	t1->tv_usec += t2->tv_usec;
	timevalfix(t1);
}

/* 中文：从 t1 中减去 t2（秒与微秒分别相减后规整借位） */
/* EN: Subtract t2 from t1 (subtract seconds and microseconds separately, then normalize any borrow) */
timevalsub(t1, t2)
	struct timeval *t1, *t2;
{

	t1->tv_sec -= t2->tv_sec;
	t1->tv_usec -= t2->tv_usec;
	timevalfix(t1);
}

/* 中文：规整 timeval：微秒为负时向秒借位，微秒达到或超过 1000000 时向秒进位 */
/* EN: Normalize a timeval: borrow a second when microseconds are negative and carry when they reach 1000000 */
timevalfix(t1)
	struct timeval *t1;
{

	if (t1->tv_usec < 0) {
		t1->tv_sec--;
		t1->tv_usec += 1000000;
	}
	if (t1->tv_usec >= 1000000) {
		t1->tv_sec++;
		t1->tv_usec -= 1000000;
	}
}
