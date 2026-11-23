/*
 * Copyright (c 2026 jialiang.  All rights reserved.
 *
 * sync.c - kernel synchronization primitives (S200).
 *
 * Implemented on top of the existing kthread block/wake machinery, without
 * changing the scheduler.  Two execution contexts exist in BSD45:
 *   - kernel threads: current TCB is `curk`, blocked via kthread_block();
 *   - user threads:    current TCB is `cur_thread` (usermode.c), blocked by
 *                      setting t_state=KT_BLOCK (proc_sched skips them).
 * A sleeping primitive must handle both.  We expose helpers that resolve the
 * current TCB and block/wake it in the matching way.
 */
#include <sys/param.h>
#include <sys/systm.h>

#include "sys/smp.h"
#include "sys/sync.h"
#include "thread/kthread.h"
#include "arch/i386/io.h"

/* ---- current-thread resolution ------------------------------------ */
/* S414: curk is a per-CPU macro from kthread.h now, no extern needed. */
extern void kthread_block(void);
extern void kthread_wake(struct kthread *t);
extern void sched_on_wake(struct kthread *t);
/* cur_proc_kthread() (kthread.h) returns usermode.c's static cur_thread */

/*
 * sync_current() - the TCB that should sleep.  Kernel-thread context wins
 * (curk is only set while a kthread runs); otherwise fall back to the user
 * thread selected by proc_sched.
 */
static struct kthread *
sync_current(void)
{

	if (curk != 0)
		return (curk);
	return (cur_proc_kthread());
}

/*
 * sync_block(t, wchan) - put TCB t to sleep on wchan.
 * For a kernel thread use the real kthread_block() stack switch.  For a user
 * thread we only mark KT_BLOCK: the running proc_sched loop will switch away
 * at its next safe point (same mechanism the pipe/wait paths already use).
 */
static void
sync_block(struct kthread *t, void *wchan)
{

	if (t == 0)
		return;
	t->t_wchan = wchan;
	t->t_state = KT_BLOCK;
	if (t == curk)
		kthread_block();		/* kernel thread: real stack switch */
	/* user thread: proc_sched observes KT_BLOCK and deschedules it */
}

/* sync_wake(t) - make a sleeping TCB runnable again. */
static void
sync_wake(struct kthread *t)
{

	if (t == 0 || t->t_state != KT_BLOCK)
		return;
	t->t_sleep_ticks++;
	sched_on_wake(t);
	t->t_state = KT_RUN;
	t->t_wchan = 0;
	if (t != curk)
		kthread_wake(t);		/* kernel thread: back on run queue */
	/* user thread: proc_sched picks it up on its next scan */
}

/* ------------------------------------------------------------------ */
/* wait queue                                                         */
/* ------------------------------------------------------------------ */
void
waitq_init(struct waitq *wq, const char *name)
{

	if (wq == 0)
		return;
	wq->wq_head = 0;
	/* waitq_lock is a struct spinlock; reset via the arch implementation */
	spin_init(&wq->wq_lock);
}

/*
 * waitq_sleep - enqueue current TCB on wq and block.  The spinlock protects
 * the queue; it is released before we actually sleep.
 */
void
waitq_sleep(struct waitq *wq)
{
	struct kthread *t = sync_current();

	if (wq == 0 || t == 0)
		return;
	spin_lock(&wq->wq_lock);
	t->t_wqnext = 0;
	if (wq->wq_head == 0) {
		wq->wq_head = t;
	} else {
		struct kthread *p = wq->wq_head;

		while (p->t_wqnext != 0)
			p = p->t_wqnext;
		p->t_wqnext = t;
	}
	spin_unlock(&wq->wq_lock);
	sync_block(t, wq);
}

/*
 * waitq_timed_sleep - bounded sleep using PIT ticks.  Returns 1 if a wakeup
 * arrived, 0 on timeout.  Requires the 100Hz clock (always present).
 */
int
waitq_timed_sleep(struct waitq *wq, int ticks)
{
	struct kthread *t = sync_current();
	int target;

	if (wq == 0 || t == 0)
		return (0);
	extern unsigned clock_get_ticks(void);
	target = (int)clock_get_ticks() + (ticks > 0 ? ticks : 1);
	waitq_sleep(wq);
	/* If we are here we were woken; proc_sched/tick handles the timeout
	 * case by clearing t_wchan at the next sched_tick (see sync_tick()). */
	return ((int)clock_get_ticks() < target + ticks);
}

/* waitq_dequeue - pull one TCB off wq; caller must hold no lock. */
static struct kthread *
waitq_dequeue(struct waitq *wq)
{
	struct kthread *t;

	if (wq == 0)
		return (0);
	spin_lock(&wq->wq_lock);
	t = wq->wq_head;
	if (t != 0)
		wq->wq_head = t->t_wqnext;
	spin_unlock(&wq->wq_lock);
	if (t != 0)
		t->t_wqnext = 0;
	return (t);
}

void
waitq_wakeone(struct waitq *wq)
{

	sync_wake(waitq_dequeue(wq));
}

void
waitq_wakeall(struct waitq *wq)
{
	struct kthread *t;

	if (wq == 0)
		return;
	for (;;) {
		t = waitq_dequeue(wq);
		if (t == 0)
			break;
		sync_wake(t);
	}
}

int
waitq_waiters(struct waitq *wq)
{
	struct kthread *t;
	int n = 0;

	if (wq == 0)
		return (0);
	spin_lock(&wq->wq_lock);
	for (t = wq->wq_head; t != 0; t = t->t_wqnext)
		n++;
	spin_unlock(&wq->wq_lock);
	return (n);
}

/* ------------------------------------------------------------------ */
/* mutex - sleeping lock with priority inheritance                    */
/* ------------------------------------------------------------------ */
void
mutex_init(struct kmutex *m, const char *name)
{

	if (m == 0)
		return;
	spin_init(&m->m_lock);
	m->m_owner = 0;
	waitq_init(&m->m_waitq, name);
	m->m_inited = 1;
}

void
mutex_enter(struct kmutex *m)
{
	struct kthread *t = sync_current();

	if (m == 0 || t == 0)
		return;
	for (;;) {
		spin_lock(&m->m_lock);
		if (m->m_owner == 0) {
			m->m_owner = t;
			spin_unlock(&m->m_lock);
			return;
		}
		/* record donor for priority inheritance */
		t->t_blocked_on = m->m_owner->t_tid;
		extern void prio_inherit(struct kthread *, struct kthread *);
		prio_inherit(t, m->m_owner);
		spin_unlock(&m->m_lock);
		waitq_sleep(&m->m_waitq);
		/* on wake, re-check ownership under the lock */
	}
}

int
mutex_try(struct kmutex *m)
{
	struct kthread *t = sync_current();
	int got = 0;

	if (m == 0 || t == 0)
		return (0);
	spin_lock(&m->m_lock);
	if (m->m_owner == 0) {
		m->m_owner = t;
		got = 1;
	}
	spin_unlock(&m->m_lock);
	return (got);
}

void
mutex_exit(struct kmutex *m)
{
	struct kthread *t = sync_current();

	if (m == 0)
		return;
	spin_lock(&m->m_lock);
	if (m->m_owner != t) {
		spin_unlock(&m->m_lock);
		return;			/* not owner: ignore (defensive) */
	}
	extern void prio_restore(struct kthread *);
	prio_restore(m->m_owner);
	m->m_owner = 0;
	spin_unlock(&m->m_lock);
	waitq_wakeone(&m->m_waitq);
}

int
mutex_owned(struct kmutex *m)
{

	return (m != 0 && m->m_owner != 0);
}

/* ------------------------------------------------------------------ */
/* semaphore                                                          */
/* ------------------------------------------------------------------ */
void
sem_init(struct ksem *s, int count, const char *name)
{

	if (s == 0)
		return;
	spin_init(&s->s_lock);
	s->s_count = count;
	waitq_init(&s->s_waitq, name);
}

void
sem_wait(struct ksem *s)
{

	if (s == 0)
		return;
	for (;;) {
		spin_lock(&s->s_lock);
		if (s->s_count > 0) {
			s->s_count--;
			spin_unlock(&s->s_lock);
			return;
		}
		spin_unlock(&s->s_lock);
		waitq_sleep(&s->s_waitq);
	}
}

int
sem_trywait(struct ksem *s)
{
	int got = 0;

	if (s == 0)
		return (0);
	spin_lock(&s->s_lock);
	if (s->s_count > 0) {
		s->s_count--;
		got = 1;
	}
	spin_unlock(&s->s_lock);
	return (got);
}

void
sem_post(struct ksem *s)
{

	if (s == 0)
		return;
	spin_lock(&s->s_lock);
	s->s_count++;
	spin_unlock(&s->s_lock);
	waitq_wakeone(&s->s_waitq);
}

int
sem_value(struct ksem *s)
{
	int v;

	if (s == 0)
		return (0);
	spin_lock(&s->s_lock);
	v = s->s_count;
	spin_unlock(&s->s_lock);
	return (v);
}

/* ------------------------------------------------------------------ */
/* condition variable                                                 */
/* ------------------------------------------------------------------ */
void
cv_init(struct kcondvar *cv, const char *name)
{

	if (cv == 0)
		return;
	waitq_init(&cv->cv_waitq, name);
}

/*
 * cv_wait - atomically release m and sleep.  On wake we re-acquire m, so the
 * caller's predicate must be re-checked in a loop (standard condvar usage).
 */
void
cv_wait(struct kcondvar *cv, struct kmutex *m)
{

	if (cv == 0 || m == 0)
		return;
	mutex_exit(m);
	waitq_sleep(&cv->cv_waitq);
	mutex_enter(m);
}

void
cv_signal(struct kcondvar *cv)
{

	waitq_wakeone(&cv->cv_waitq);
}

void
cv_broadcast(struct kcondvar *cv)
{

	waitq_wakeall(&cv->cv_waitq);
}

/* ------------------------------------------------------------------ */
/* selftest - single-threaded functional checks (no concurrency needed  */
/* to validate the data structures and non-blocking paths).             */
/* ------------------------------------------------------------------ */
int
sync_selftest(void)
{
	struct kmutex m;
	struct ksem s;
	struct kcondvar cv;
	struct waitq wq;
	struct kthread fake;
	struct kthread *saved_curk;
	int ok = 1;

	/*
	 * The monitor runs with no current TCB (curk==0 and no user thread
	 * selected).  Install a temporary fake TCB so mutex ownership checks
	 * have a stable identity, then restore curk before returning.
	 */
	bzero(&fake, sizeof(fake));
	fake.t_tid = 0x7fff;
	fake.t_state = KT_RUN;
	saved_curk = curk;
	curk = &fake;

	mutex_init(&m, "test-mtx");
	if (mutex_owned(&m))
		ok = 0;
	if (!mutex_try(&m)) {
		ok = 0;
	} else {
		if (!mutex_owned(&m))
			ok = 0;
		mutex_exit(&m);
	}
	if (mutex_owned(&m))
		ok = 0;

	sem_init(&s, 2, "test-sem");
	if (sem_value(&s) != 2)
		ok = 0;
	if (!sem_trywait(&s) || sem_value(&s) != 1)
		ok = 0;
	if (!sem_trywait(&s) || sem_value(&s) != 0)
		ok = 0;
	if (sem_trywait(&s))
		ok = 0;			/* would block: must fail */
	sem_post(&s);
	if (sem_value(&s) != 1)
		ok = 0;

	cv_init(&cv, "test-cv");
	waitq_init(&wq, "test-wq");
	if (waitq_waiters(&wq) != 0)
		ok = 0;
	waitq_wakeone(&wq);		/* wake empty queue: must be safe */
	waitq_wakeall(&wq);

	curk = saved_curk;
	return (ok);
}
