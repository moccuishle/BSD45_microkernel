/*
 * Copyright (c) 1982, 1986, 1989, 1990, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
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
 *	@(#)kern_prot.c	8.9 (Berkeley) 2/14/95
 */

/*
 * System calls related to processes and protection
 */

#include <sys/param.h>
#include <sys/acct.h>
#include <sys/systm.h>
#include <sys/ucred.h>
#include <sys/kprocess.h>
#include <sys/timeb.h>
#include <sys/times.h>
#include <sys/malloc.h>

#include <sys/mount.h>
#include <sys/syscallargs.h>

/* 中文：getpid 系统调用：返回当前进程的进程号（兼容模式下同时返回父进程号） */
/* EN: The getpid system call: return the current process ID (also the parent PID in compatibility modes) */
/* ARGSUSED */
int
getpid(p, uap, retval)
	struct process *p;
	void *uap;
	register_t *retval;
{

	*retval = p->p_pid;
#if defined(COMPAT_43) || defined(COMPAT_SUNOS)
	retval[1] = p->p_pptr->p_pid;
#endif
	return (0);
}

/* 中文：getppid 系统调用：返回当前进程的父进程号 */
/* EN: The getppid system call: return the parent process ID of the current process */
/* ARGSUSED */
int
getppid(p, uap, retval)
	struct process *p;
	void *uap;
	register_t *retval;
{

	*retval = p->p_pptr->p_pid;
	return (0);
}

/* Get process group ID; note that POSIX getpgrp takes no parameter */
/* 中文：getpgrp 系统调用：返回当前进程所在进程组的组号 */
/* EN: The getpgrp system call: return the process group ID of the current process */
int
getpgrp(p, uap, retval)
	struct process *p;
	void *uap;
	register_t *retval;
{

	*retval = p->p_pgrp->pg_id;
	return (0);
}

/* 中文：getuid 系统调用：返回当前进程的真实用户号（兼容模式下同时返回有效用户号） */
/* EN: The getuid system call: return the real user ID of the current process (also the effective UID in compatibility modes) */
/* ARGSUSED */
int
getuid(p, uap, retval)
	struct process *p;
	void *uap;
	register_t *retval;
{

	*retval = p->p_cred->p_ruid;
#if defined(COMPAT_43) || defined(COMPAT_SUNOS)
	retval[1] = p->p_ucred->cr_uid;
#endif
	return (0);
}

/* 中文：geteuid 系统调用：返回当前进程的有效用户号 */
/* EN: The geteuid system call: return the effective user ID of the current process */
/* ARGSUSED */
int
geteuid(p, uap, retval)
	struct process *p;
	void *uap;
	register_t *retval;
{

	*retval = p->p_ucred->cr_uid;
	return (0);
}

/* 中文：getgid 系统调用：返回当前进程的真实组号（兼容模式下同时返回有效组号） */
/* EN: The getgid system call: return the real group ID of the current process (also the effective GID in compatibility modes) */
/* ARGSUSED */
int
getgid(p, uap, retval)
	struct process *p;
	void *uap;
	register_t *retval;
{

	*retval = p->p_cred->p_rgid;
#if defined(COMPAT_43) || defined(COMPAT_SUNOS)
	retval[1] = p->p_ucred->cr_groups[0];
#endif
	return (0);
}

/*
 * Get effective group ID.  The "egid" is groups[0], and could be obtained
 * via getgroups.  This syscall exists because it is somewhat painful to do
 * correctly in a library function.
 */
/* 中文：getegid 系统调用：返回当前进程的有效组号（即 cr_groups[0]） */
/* EN: The getegid system call: return the effective group ID of the current process (cr_groups[0]) */
/* ARGSUSED */
int
getegid(p, uap, retval)
	struct process *p;
	void *uap;
	register_t *retval;
{

	*retval = p->p_ucred->cr_groups[0];
	return (0);
}

/* 中文：getgroups 系统调用：返回当前进程的附属组号集合，gidsetsize 为 0 时只返回组数量 */
/* EN: The getgroups system call: return the supplementary group IDs of the current process; when gidsetsize is 0 only return the group count */
int
getgroups(p, uap, retval)
	struct process *p;
	register struct getgroups_args /* {
		syscallarg(u_int) gidsetsize;
		syscallarg(gid_t *) gidset;
	} */ *uap;
	register_t *retval;
{
	register struct pcred *pc = p->p_cred;
	register u_int ngrp;
	int error;

	if ((ngrp = SCARG(uap, gidsetsize)) == 0) {
		*retval = pc->pc_ucred->cr_ngroups;
		return (0);
	}
	if (ngrp < pc->pc_ucred->cr_ngroups)
		return (EINVAL);
	ngrp = pc->pc_ucred->cr_ngroups;
	if (error = copyout((caddr_t)pc->pc_ucred->cr_groups,
	    (caddr_t)SCARG(uap, gidset), ngrp * sizeof(gid_t)))
		return (error);
	*retval = ngrp;
	return (0);
}

/* 中文：setsid 系统调用：为调用者创建新会话并成为会话首领，若已是进程组长则返回 EPERM */
/* EN: The setsid system call: create a new session with the caller as leader, failing with EPERM if it is already a process group leader */
/* ARGSUSED */
int
setsid(p, uap, retval)
	register struct process *p;
	void *uap;
	register_t *retval;
{

	if (p->p_pgid == p->p_pid || pgfind(p->p_pid)) {
		return (EPERM);
	} else {
		(void)enterpgrp(p, p->p_pid, 1);
		*retval = p->p_pid;
		return (0);
	}
}

/*
 * set process group (setpgid/old setpgrp)
 *
 * caller does setpgid(targpid, targpgid)
 *
 * pid must be caller or child of caller (ESRCH)
 * if a child
 *	pid must be in same session (EPERM)
 *	pid can't have done an exec (EACCES)
 * if pgid != pid
 * 	there must exist some pid in same session having pgid (EPERM)
 * pid must not be session leader (EPERM)
 */
/* 中文：setpgid 系统调用：校验目标进程是自己或同会话的子进程后，用 enterpgrp 设置其进程组号 */
/* EN: The setpgid system call: validate the target is the caller or a child in the same session, then set its process group via enterpgrp */
/* ARGSUSED */
int
setpgid(curp, uap, retval)
	struct process *curp;
	register struct setpgid_args /* {
		syscallarg(int) pid;
		syscallarg(int) pgid;
	} */ *uap;
	register_t *retval;
{
	register struct process *targp;		/* target process */
	register struct pgrp *pgrp;		/* target pgrp */

	if (SCARG(uap, pid) != 0 && SCARG(uap, pid) != curp->p_pid) {
		if ((targp = pfind(SCARG(uap, pid))) == 0 || !inferior(targp))
			return (ESRCH);
		if (targp->p_session != curp->p_session)
			return (EPERM);
		if (targp->p_flag & P_EXEC)
			return (EACCES);
	} else
		targp = curp;
	if (SESS_LEADER(targp))
		return (EPERM);
	if (SCARG(uap, pgid) == 0)
		SCARG(uap, pgid) = targp->p_pid;
	else if (SCARG(uap, pgid) != targp->p_pid)
		if ((pgrp = pgfind(SCARG(uap, pgid))) == 0 ||
	            pgrp->pg_session != curp->p_session)
			return (EPERM);
	return (enterpgrp(targp, SCARG(uap, pgid), 0));
}

/* 中文：setuid 系统调用：设置进程的真实、有效和保存用户号，必要时校验特权并转移进程计数 */
/* EN: The setuid system call: set the real, effective and saved user IDs, checking privilege when needed and transferring the process count */
/* ARGSUSED */
int
setuid(p, uap, retval)
	struct process *p;
	struct setuid_args /* {
		syscallarg(uid_t) uid;
	} */ *uap;
	register_t *retval;
{
	register struct pcred *pc = p->p_cred;
	register uid_t uid;
	int error;

	uid = SCARG(uap, uid);
	if (uid != pc->p_ruid &&
	    (error = suser(pc->pc_ucred, &p->p_acflag)))
		return (error);
	/*
	 * Everything's okay, do it.
	 * Transfer proc count to new user.
	 * Copy credentials so other references do not see our changes.
	 */
	(void)chgproccnt(pc->p_ruid, -1);
	(void)chgproccnt(uid, 1);
	pc->pc_ucred = crcopy(pc->pc_ucred);
	pc->pc_ucred->cr_uid = uid;
	pc->p_ruid = uid;
	pc->p_svuid = uid;
	p->p_flag |= P_SUGID;
	return (0);
}

/* 中文：seteuid 系统调用：设置有效用户号，仅允许在真实/保存用户号之间切换或由特权用户设置 */
/* EN: The seteuid system call: set the effective user ID, allowing switches between real/saved UIDs or setting by a privileged user */
/* ARGSUSED */
int
seteuid(p, uap, retval)
	struct process *p;
	struct seteuid_args /* {
		syscallarg(uid_t) euid;
	} */ *uap;
	register_t *retval;
{
	register struct pcred *pc = p->p_cred;
	register uid_t euid;
	int error;

	euid = SCARG(uap, euid);
	if (euid != pc->p_ruid && euid != pc->p_svuid &&
	    (error = suser(pc->pc_ucred, &p->p_acflag)))
		return (error);
	/*
	 * Everything's okay, do it.  Copy credentials so other references do
	 * not see our changes.
	 */
	pc->pc_ucred = crcopy(pc->pc_ucred);
	pc->pc_ucred->cr_uid = euid;
	p->p_flag |= P_SUGID;
	return (0);
}

/* 中文：setgid 系统调用：设置进程的真实、有效和保存组号，必要时校验超级用户权限 */
/* EN: The setgid system call: set the real, effective and saved group IDs, checking super-user privilege when needed */
/* ARGSUSED */
int
setgid(p, uap, retval)
	struct process *p;
	struct setgid_args /* {
		syscallarg(gid_t) gid;
	} */ *uap;
	register_t *retval;
{
	register struct pcred *pc = p->p_cred;
	register gid_t gid;
	int error;

	gid = SCARG(uap, gid);
	if (gid != pc->p_rgid && (error = suser(pc->pc_ucred, &p->p_acflag)))
		return (error);
	pc->pc_ucred = crcopy(pc->pc_ucred);
	pc->pc_ucred->cr_groups[0] = gid;
	pc->p_rgid = gid;
	pc->p_svgid = gid;		/* ??? */
	p->p_flag |= P_SUGID;
	return (0);
}

/* 中文：setegid 系统调用：设置有效组号，仅允许在真实/保存组号之间切换或由特权用户设置 */
/* EN: The setegid system call: set the effective group ID, allowing switches between real/saved GIDs or setting by a privileged user */
/* ARGSUSED */
int
setegid(p, uap, retval)
	struct process *p;
	struct setegid_args /* {
		syscallarg(gid_t) egid;
	} */ *uap;
	register_t *retval;
{
	register struct pcred *pc = p->p_cred;
	register gid_t egid;
	int error;

	egid = SCARG(uap, egid);
	if (egid != pc->p_rgid && egid != pc->p_svgid &&
	    (error = suser(pc->pc_ucred, &p->p_acflag)))
		return (error);
	pc->pc_ucred = crcopy(pc->pc_ucred);
	pc->pc_ucred->cr_groups[0] = egid;
	p->p_flag |= P_SUGID;
	return (0);
}

/* 中文：setgroups 系统调用：仅超级用户可调用，从用户空间复制组号数组来设置进程的附属组集合 */
/* EN: The setgroups system call: super-user only, copy a group array from user space to set the process's supplementary groups */
/* ARGSUSED */
int
setgroups(p, uap, retval)
	struct process *p;
	struct setgroups_args /* {
		syscallarg(u_int) gidsetsize;
		syscallarg(gid_t *) gidset;
	} */ *uap;
	register_t *retval;
{
	register struct pcred *pc = p->p_cred;
	register u_int ngrp;
	int error;

	if (error = suser(pc->pc_ucred, &p->p_acflag))
		return (error);
	ngrp = SCARG(uap, gidsetsize);
	if (ngrp < 1 || ngrp > NGROUPS)
		return (EINVAL);
	pc->pc_ucred = crcopy(pc->pc_ucred);
	if (error = copyin((caddr_t)SCARG(uap, gidset),
	    (caddr_t)pc->pc_ucred->cr_groups, ngrp * sizeof(gid_t)))
		return (error);
	pc->pc_ucred->cr_ngroups = ngrp;
	p->p_flag |= P_SUGID;
	return (0);
}

#if defined(COMPAT_43) || defined(COMPAT_SUNOS)
/* 中文：4.3BSD 兼容的 setreuid：ruid 与 euid 相同时退化为 setuid，否则校验后仅用 seteuid 设置有效用户号 */
/* EN: 4.3BSD-compatible setreuid: degrade to setuid when ruid equals euid, otherwise validate and set the effective UID via seteuid */
/* ARGSUSED */
int
compat_43_setreuid(p, uap, retval)
	register struct process *p;
	struct compat_43_setreuid_args /* {
		syscallarg(int) ruid;
		syscallarg(int) euid;
	} */ *uap;
	register_t *retval;
{
	register struct pcred *pc = p->p_cred;
	union {
		struct setuid_args sa;
		struct seteuid_args ea;
	} args;

	/*
	 * If ruid == euid then setreuid is being used to emulate setuid,
	 * just do it.
	 */
	if (SCARG(uap, ruid) != -1 && SCARG(uap, ruid) == SCARG(uap, euid)) {
		SCARG(&args.sa, uid) = SCARG(uap, ruid);
		return (setuid(p, &args.sa, retval));
	}
	/*
	 * Otherwise we assume that the intent of setting ruid is to be
	 * able to get back ruid priviledge (i.e. swapping ruid and euid).
	 * So we make sure that we will be able to do so, but do not
	 * actually set the ruid.
	 */
	if (SCARG(uap, ruid) != (uid_t)-1 && SCARG(uap, ruid) != pc->p_ruid &&
	    SCARG(uap, ruid) != pc->p_svuid)
		return (EPERM);
	if (SCARG(uap, euid) == (uid_t)-1)
		return (0);
	SCARG(&args.ea, euid) = SCARG(uap, euid);
	return (seteuid(p, &args.ea, retval));
}

/* 中文：4.3BSD 兼容的 setregid：rgid 与 egid 相同时退化为 setgid，否则校验后仅用 setegid 设置有效组号 */
/* EN: 4.3BSD-compatible setregid: degrade to setgid when rgid equals egid, otherwise validate and set the effective GID via setegid */
/* ARGSUSED */
int
compat_43_setregid(p, uap, retval)
	register struct process *p;
	struct compat_43_setregid_args /* {
		syscallarg(int) rgid;
		syscallarg(int) egid;
	} */ *uap;
	register_t *retval;
{
	register struct pcred *pc = p->p_cred;
	union {
		struct setgid_args sa;
		struct setegid_args ea;
	} args;

	/*
	 * If rgid == egid then setreuid is being used to emulate setgid,
	 * just do it.
	 */
	if (SCARG(uap, rgid) != -1 && SCARG(uap, rgid) == SCARG(uap, egid)) {
		SCARG(&args.sa, gid) = SCARG(uap, rgid);
		return (setgid(p, &args.sa, retval));
	}
	/*
	 * Otherwise we assume that the intent of setting rgid is to be
	 * able to get back rgid priviledge (i.e. swapping rgid and egid).
	 * So we make sure that we will be able to do so, but do not
	 * actually set the rgid.
	 */
	if (SCARG(uap, rgid) != (gid_t)-1 && SCARG(uap, rgid) != pc->p_rgid &&
	    SCARG(uap, rgid) != pc->p_svgid)
		return (EPERM);
	if (SCARG(uap, egid) == (gid_t)-1)
		return (0);
	SCARG(&args.ea, egid) = SCARG(uap, egid);
	return (setegid(p, &args.ea, retval));
}
#endif /* defined(COMPAT_43) || defined(COMPAT_SUNOS) */

/*
 * Check if gid is a member of the group set.
 */
/* 中文：检查给定 gid 是否属于凭证 cred 中的组集合 */
/* EN: Check whether a given gid is a member of the group set held in credentials cred */
int
groupmember(gid, cred)
	gid_t gid;
	register struct ucred *cred;
{
	register gid_t *gp;
	gid_t *egp;

	egp = &(cred->cr_groups[cred->cr_ngroups]);
	for (gp = cred->cr_groups; gp < egp; gp++)
		if (*gp == gid)
			return (1);
	return (0);
}

/*
 * Test whether the specified credentials imply "super-user"
 * privilege; if so, and we have accounting info, set the flag
 * indicating use of super-powers.
 * Returns 0 or error.
 */
/* 中文：检查凭证是否具有超级用户权限（uid 为 0），若有且提供 acflag 则设置 ASU 记账标志 */
/* EN: Test whether credentials imply super-user privilege (uid 0), setting the ASU accounting flag via acflag when provided */
int
suser(cred, acflag)
	struct ucred *cred;
	u_short *acflag;
{
	if (cred->cr_uid == 0) {
		if (acflag)
			*acflag |= ASU;
		return (0);
	}
	return (EPERM);
}

/*
 * Allocate a zeroed cred structure.
 */
/* 中文：分配并清零一个引用计数为 1 的 ucred 凭证结构 */
/* EN: Allocate and zero a ucred credentials structure with reference count 1 */
struct ucred *
crget()
{
	register struct ucred *cr;

	MALLOC(cr, struct ucred *, sizeof(*cr), M_CRED, M_WAITOK);
	bzero((caddr_t)cr, sizeof(*cr));
	cr->cr_ref = 1;
	return (cr);
}

/*
 * Free a cred structure.
 * Throws away space when ref count gets to 0.
 */
/* 中文：释放一个 ucred 凭证结构，引用计数降到 0 时才真正释放其内存 */
/* EN: Free a ucred credentials structure, actually releasing its memory when the reference count drops to zero */
void
crfree(cr)
	struct ucred *cr;
{
	int s;

	s = splimp();				/* ??? */
	if (--cr->cr_ref == 0)
		FREE((caddr_t)cr, M_CRED);
	(void) splx(s);
}

/*
 * Copy cred structure to a new one and free the old one.
 */
/* 中文：为修改准备凭证副本：引用数为 1 时直接返回原结构，否则复制新结构并释放旧结构 */
/* EN: Copy credentials for modification: return the original when refcount is 1, otherwise copy to a new structure and free the old one */
struct ucred *
crcopy(cr)
	struct ucred *cr;
{
	struct ucred *newcr;

	if (cr->cr_ref == 1)
		return (cr);
	newcr = crget();
	*newcr = *cr;
	crfree(cr);
	newcr->cr_ref = 1;
	return (newcr);
}

/*
 * Dup cred struct to a new held one.
 */
/* 中文：把一个 ucred 凭证结构复制为独立持有引用的新结构 */
/* EN: Duplicate a ucred credentials structure into a new independently held copy */
struct ucred *
crdup(cr)
	struct ucred *cr;
{
	struct ucred *newcr;

	newcr = crget();
	*newcr = *cr;
	newcr->cr_ref = 1;
	return (newcr);
}

/*
 * Get login name, if available.
 */
/* 中文：getlogin 系统调用：把当前会话的登录名复制到用户缓冲区，长度按 s_login 大小截断 */
/* EN: The getlogin system call: copy the current session's login name into a user buffer, truncating the length to the s_login size */
/* ARGSUSED */
int
getlogin(p, uap, retval)
	struct process *p;
	struct getlogin_args /* {
		syscallarg(char *) namebuf;
		syscallarg(u_int) namelen;
	} */ *uap;
	register_t *retval;
{

	if (SCARG(uap, namelen) > sizeof (p->p_pgrp->pg_session->s_login))
		SCARG(uap, namelen) = sizeof (p->p_pgrp->pg_session->s_login);
	return (copyout((caddr_t) p->p_pgrp->pg_session->s_login,
	    (caddr_t) SCARG(uap, namebuf), SCARG(uap, namelen)));
}

/*
 * Set login name.
 */
/* 中文：setlogin 系统调用：仅超级用户可调用，从用户空间复制字符串来设置当前会话的登录名 */
/* EN: The setlogin system call: super-user only, copy a string from user space to set the current session's login name */
/* ARGSUSED */
int
setlogin(p, uap, retval)
	struct process *p;
	struct setlogin_args /* {
		syscallarg(char *) namebuf;
	} */ *uap;
	register_t *retval;
{
	int error;

	if (error = suser(p->p_ucred, &p->p_acflag))
		return (error);
	error = copyinstr((caddr_t) SCARG(uap, namebuf),
	    (caddr_t) p->p_pgrp->pg_session->s_login,
	    sizeof (p->p_pgrp->pg_session->s_login) - 1, (u_int *)0);
	if (error == ENAMETOOLONG)
		error = EINVAL;
	return (error);
}
