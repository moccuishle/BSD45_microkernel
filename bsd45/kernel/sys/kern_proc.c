/*
 * Copyright (c) 1982, 1986, 1989, 1991, 1993
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
 *	@(#)kern_proc.c	8.7 (Berkeley) 2/14/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/map.h>
#include <sys/kernel.h>
#include <sys/kprocess.h>
#include <sys/buf.h>
#include <sys/acct.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <ufs/ufs/quota.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/ioctl.h>
#include <sys/tty.h>

/*
 * Structure associated with user cacheing.
 */
struct uidinfo {
	LIST_ENTRY(uidinfo) ui_hash;
	uid_t	ui_uid;
	long	ui_proccnt;
};
#define	UIHASH(uid)	(&uihashtbl[(uid) & uihash])
LIST_HEAD(uihashhead, uidinfo) *uihashtbl;
u_long uihash;		/* size of hash table - 1 */

/*
 * Other process lists
 */
struct pidhashhead *pidhashtbl;
u_long pidhash;
struct pgrphashhead *pgrphashtbl;
u_long pgrphash;
struct proclist allproc;
struct proclist zombproc;

/*
 * Initialize global process hashing structures.
 */
/* 中文：初始化全局进程哈希结构：进程表、僵尸表及 pid/进程组/uid 哈希表 */
/* EN: Initialize global process hashing structures: process lists and pid/pgrp/uid hash tables */
void
procinit()
{

	LIST_INIT(&allproc);
	LIST_INIT(&zombproc);
	pidhashtbl = hashinit(maxproc / 4, M_PROC, &pidhash);
	pgrphashtbl = hashinit(maxproc / 4, M_PROC, &pgrphash);
	uihashtbl = hashinit(maxproc / 16, M_PROC, &uihash);
}

/*
 * Change the count associated with number of processes
 * a given user is using.
 */
/* 中文：按 diff 增减某用户正在使用的进程数，计数归零时释放对应的 uidinfo */
/* EN: Adjust a user's process count by diff, freeing the uidinfo entry when the count reaches zero */
int
chgproccnt(uid, diff)
	uid_t	uid;
	int	diff;
{
	register struct uidinfo *uip;
	register struct uihashhead *uipp;

	uipp = UIHASH(uid);
	for (uip = uipp->lh_first; uip != 0; uip = uip->ui_hash.le_next)
		if (uip->ui_uid == uid)
			break;
	if (uip) {
		uip->ui_proccnt += diff;
		if (uip->ui_proccnt > 0)
			return (uip->ui_proccnt);
		if (uip->ui_proccnt < 0)
			panic("chgproccnt: procs < 0");
		LIST_REMOVE(uip, ui_hash);
		FREE(uip, M_PROC);
		return (0);
	}
	if (diff <= 0) {
		if (diff == 0)
			return(0);
		panic("chgproccnt: lost user");
	}
	MALLOC(uip, struct uidinfo *, sizeof(*uip), M_PROC, M_WAITOK);
	LIST_INSERT_HEAD(uipp, uip, ui_hash);
	uip->ui_uid = uid;
	uip->ui_proccnt = diff;
	return (diff);
}

/*
 * Is p an inferior of the current process?
 */
/* 中文：判断进程 p 是否为当前进程的后代（沿父进程链向上查找） */
/* EN: Determine whether process p is an inferior (descendant) of the current process */
inferior(p)
	register struct process *p;
{

	for (; p != curproc; p = p->p_pptr)
		if (p->p_pid == 0)
			return (0);
	return (1);
}

/*
 * Locate a process by number
 */
/* 中文：按进程号 pid 在哈希表中查找并返回对应进程，未找到返回 NULL */
/* EN: Locate a process by pid in the hash table; returns NULL if not found */
struct process *
pfind(pid)
	register pid_t pid;
{
	register struct process *p;

	for (p = PIDHASH(pid)->lh_first; p != 0; p = p->p_hash.le_next)
		if (p->p_pid == pid)
			return (p);
	return (NULL);
}

/*
 * Locate a process group by number
 */
/* 中文：按进程组号 pgid 在哈希表中查找并返回对应进程组，未找到返回 NULL */
/* EN: Locate a process group by pgid in the hash table; returns NULL if not found */
struct pgrp *
pgfind(pgid)
	register pid_t pgid;
{
	register struct pgrp *pgrp;

	for (pgrp = PGRPHASH(pgid)->lh_first; pgrp != 0;
	     pgrp = pgrp->pg_hash.le_next)
		if (pgrp->pg_id == pgid)
			return (pgrp);
	return (NULL);
}

/*
 * Move p to a new or existing process group (and session)
 */
/* 中文：把进程 p 移入新的或已有的进程组（mksess 时新建会话），并调整作业控制资格 */
/* EN: Move process p into a new or existing process group (creating a session if mksess) and fix job-control eligibility */
int
enterpgrp(p, pgid, mksess)
	register struct process *p;
	pid_t pgid;
	int mksess;
{
	register struct pgrp *pgrp = pgfind(pgid);

#ifdef DIAGNOSTIC
	if (pgrp != NULL && mksess)	/* firewalls */
		panic("enterpgrp: setsid into non-empty pgrp");
	if (SESS_LEADER(p))
		panic("enterpgrp: session leader attempted setpgrp");
#endif
	if (pgrp == NULL) {
		pid_t savepid = p->p_pid;
		struct process *np;
		/*
		 * new process group
		 */
#ifdef DIAGNOSTIC
		if (p->p_pid != pgid)
			panic("enterpgrp: new pgrp and pid != pgid");
#endif
		MALLOC(pgrp, struct pgrp *, sizeof(struct pgrp), M_PGRP,
		    M_WAITOK);
		if ((np = pfind(savepid)) == NULL || np != p)
			return (ESRCH);
		if (mksess) {
			register struct session *sess;

			/*
			 * new session
			 */
			MALLOC(sess, struct session *, sizeof(struct session),
			    M_SESSION, M_WAITOK);
			sess->s_leader = p;
			sess->s_count = 1;
			sess->s_ttyvp = NULL;
			sess->s_ttyp = NULL;
			bcopy(p->p_session->s_login, sess->s_login,
			    sizeof(sess->s_login));
			p->p_flag &= ~P_CONTROLT;
			pgrp->pg_session = sess;
#ifdef DIAGNOSTIC
			if (p != curproc)
				panic("enterpgrp: mksession and p != curproc");
#endif
		} else {
			pgrp->pg_session = p->p_session;
			pgrp->pg_session->s_count++;
		}
		pgrp->pg_id = pgid;
		LIST_INIT(&pgrp->pg_members);
		LIST_INSERT_HEAD(PGRPHASH(pgid), pgrp, pg_hash);
		pgrp->pg_jobc = 0;
	} else if (pgrp == p->p_pgrp)
		return (0);

	/*
	 * Adjust eligibility of affected pgrps to participate in job control.
	 * Increment eligibility counts before decrementing, otherwise we
	 * could reach 0 spuriously during the first call.
	 */
	fixjobc(p, pgrp, 1);
	fixjobc(p, p->p_pgrp, 0);

	LIST_REMOVE(p, p_pglist);
	if (p->p_pgrp->pg_members.lh_first == 0)
		pgdelete(p->p_pgrp);
	p->p_pgrp = pgrp;
	LIST_INSERT_HEAD(&pgrp->pg_members, p, p_pglist);
	return (0);
}

/*
 * remove process from process group
 */
/* 中文：把进程从其进程组中移除，组空时删除该进程组 */
/* EN: Remove a process from its process group, deleting the group when it becomes empty */
int
leavepgrp(p)
	register struct process *p;
{

	LIST_REMOVE(p, p_pglist);
	if (p->p_pgrp->pg_members.lh_first == 0)
		pgdelete(p->p_pgrp);
	p->p_pgrp = 0;
	return (0);
}

/*
 * delete a process group
 */
/* 中文：删除进程组，解除与控制终端的关联并递减会话引用计数 */
/* EN: Delete a process group, detaching its controlling terminal and decrementing the session refcount */
void
pgdelete(pgrp)
	register struct pgrp *pgrp;
{

	if (pgrp->pg_session->s_ttyp != NULL && 
	    pgrp->pg_session->s_ttyp->t_pgrp == pgrp)
		pgrp->pg_session->s_ttyp->t_pgrp = NULL;
	LIST_REMOVE(pgrp, pg_hash);
	if (--pgrp->pg_session->s_count == 0)
		FREE(pgrp->pg_session, M_SESSION);
	FREE(pgrp, M_PGRP);
}

static void orphanpg();

/*
 * Adjust pgrp jobc counters when specified process changes process group.
 * We count the number of processes in each process group that "qualify"
 * the group for terminal job control (those with a parent in a different
 * process group of the same session).  If that count reaches zero, the
 * process group becomes orphaned.  Check both the specified process'
 * process group and that of its children.
 * entering == 0 => p is leaving specified group.
 * entering == 1 => p is entering specified group.
 */
/* 中文：进程进出进程组时调整相关进程组的作业控制计数，计数归零则标记该组为孤儿组 */
/* EN: Adjust job-control counters when a process enters/leaves a group, orphaning groups whose count hits zero */
void
fixjobc(p, pgrp, entering)
	register struct process *p;
	register struct pgrp *pgrp;
	int entering;
{
	register struct pgrp *hispgrp;
	register struct session *mysession = pgrp->pg_session;

	/*
	 * Check p's parent to see whether p qualifies its own process
	 * group; if so, adjust count for p's process group.
	 */
	if ((hispgrp = p->p_pptr->p_pgrp) != pgrp &&
	    hispgrp->pg_session == mysession)
		if (entering)
			pgrp->pg_jobc++;
		else if (--pgrp->pg_jobc == 0)
			orphanpg(pgrp);

	/*
	 * Check this process' children to see whether they qualify
	 * their process groups; if so, adjust counts for children's
	 * process groups.
	 */
	for (p = p->p_children.lh_first; p != 0; p = p->p_sibling.le_next)
		if ((hispgrp = p->p_pgrp) != pgrp &&
		    hispgrp->pg_session == mysession &&
		    p->p_stat != SZOMB)
			if (entering)
				hispgrp->pg_jobc++;
			else if (--hispgrp->pg_jobc == 0)
				orphanpg(hispgrp);
}

/* 
 * A process group has become orphaned;
 * if there are any stopped processes in the group,
 * hang-up all process in that group.
 */
/* 中文：处理变成孤儿的进程组：若有停止的进程则向全组发送 SIGHUP 与 SIGCONT */
/* EN: Handle an orphaned process group: if any member is stopped, send SIGHUP and SIGCONT to all members */
static void
orphanpg(pg)
	struct pgrp *pg;
{
	register struct process *p;

	for (p = pg->pg_members.lh_first; p != 0; p = p->p_pglist.le_next) {
		if (p->p_stat == SSTOP) {
			for (p = pg->pg_members.lh_first; p != 0;
			    p = p->p_pglist.le_next) {
				psignal(p, SIGHUP);
				psignal(p, SIGCONT);
			}
			return;
		}
	}
}

#ifdef DEBUG
/* 中文：调试用，打印所有进程组及其成员进程的信息 */
/* EN: Debug helper that prints all process groups and their member processes */
pgrpdump()
{
	register struct pgrp *pgrp;
	register struct process *p;
	register i;

	for (i = 0; i <= pgrphash; i++) {
		if (pgrp = pgrphashtbl[i].lh_first) {
			printf("\tindx %d\n", i);
			for (; pgrp != 0; pgrp = pgrp->pg_hash.le_next) {
				printf("\tpgrp %x, pgid %d, sess %x, sesscnt %d, mem %x\n",
				    pgrp, pgrp->pg_id, pgrp->pg_session,
				    pgrp->pg_session->s_count,
				    pgrp->pg_members.lh_first);
				for (p = pgrp->pg_members.lh_first; p != 0;
				    p = p->p_pglist.le_next) {
					printf("\t\tpid %d addr %x pgrp %x\n", 
					    p->p_pid, p, p->p_pgrp);
				}
			}
		}
	}
}
#endif /* DEBUG */
