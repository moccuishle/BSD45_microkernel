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
 *	@(#)kern_xxx.c	8.3 (Berkeley) 2/14/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kprocess.h>
#include <sys/reboot.h>
#include <vm/vm.h>
#include <sys/sysctl.h>

#include <sys/mount.h>
#include <sys/syscallargs.h>

/* ARGSUSED */
/* 中文：reboot 系统调用：经 suser 校验权限后，用 opt 参数调用 boot 重启系统 */
/* EN: reboot system call: after suser privilege check, call boot with the opt argument to reboot the system */
int
reboot(p, uap, retval)
	struct process *p;
	struct reboot_args /* {
		syscallarg(int) opt;
	} */ *uap;
	register_t *retval;
{
	int error;

	if (error = suser(p->p_ucred, &p->p_acflag))
		return (error);
	boot(SCARG(uap, opt));
	return (0);
}

#if defined(COMPAT_43) || defined(COMPAT_SUNOS)

/* ARGSUSED */
/* 中文：4.3BSD 兼容的 gethostname 系统调用：通过 kern_sysctl 读取 KERN_HOSTNAME 返回主机名 */
/* EN: 4.3BSD-compatible gethostname call: read KERN_HOSTNAME via kern_sysctl to return the host name */
int
compat_43_gethostname(p, uap, retval)
	struct process *p;
	struct compat_43_gethostname_args /* {
		syscallarg(char *) hostname;
		syscallarg(u_int) len;
	} */ *uap;
	register_t *retval;
{
	int name;

	name = KERN_HOSTNAME;
	return (kern_sysctl(&name, 1, SCARG(uap, hostname), &SCARG(uap, len),
	    0, 0));
}

/* ARGSUSED */
/* 中文：4.3BSD 兼容的 sethostname 系统调用：经 suser 校验权限后通过 kern_sysctl 写入 KERN_HOSTNAME 设置主机名 */
/* EN: 4.3BSD-compatible sethostname call: after suser privilege check, set the host name by writing KERN_HOSTNAME via kern_sysctl */
int
compat_43_sethostname(p, uap, retval)
	struct process *p;
	register struct compat_43_sethostname_args /* {
		syscallarg(char *) hostname;
		syscallarg(u_int) len;
	} */ *uap;
	register_t *retval;
{
	int name;
	int error;

	if (error = suser(p->p_ucred, &p->p_acflag))
		return (error);
	name = KERN_HOSTNAME;
	return (kern_sysctl(&name, 1, 0, 0, SCARG(uap, hostname),
	    SCARG(uap, len)));
}

/* ARGSUSED */
/* 中文：4.3BSD 兼容的 gethostid 系统调用：把全局 hostid 写入 retval 返回 */
/* EN: 4.3BSD-compatible gethostid call: write the global hostid into retval to return it */
int
compat_43_gethostid(p, uap, retval)
	struct process *p;
	void *uap;
	register_t *retval;
{

	*(int32_t *)retval = hostid;
	return (0);
}
#endif /* COMPAT_43 || COMPAT_SUNOS */

#ifdef COMPAT_43
/* ARGSUSED */
/* 中文：4.3BSD 兼容的 sethostid 系统调用：经 suser 校验权限后把参数 hostid 存入全局变量 hostid */
/* EN: 4.3BSD-compatible sethostid call: after suser privilege check, store the argument hostid into the global hostid */
int
compat_43_sethostid(p, uap, retval)
	struct process *p;
	struct compat_43_sethostid_args /* {
		syscallarg(int32_t) hostid;
	} */ *uap;
	register_t *retval;
{
	int error;

	if (error = suser(p->p_ucred, &p->p_acflag))
		return (error);
	hostid = SCARG(uap, hostid);
	return (0);
}

/* 中文：4.3BSD 兼容的 quota 系统调用占位：函数体已删除，返回 ENOSYS 表示未实现 */
/* EN: 4.3BSD-compatible quota system call placeholder: body removed, returns ENOSYS to indicate it is unimplemented */
int
compat_43_quota(p, uap, retval)
	struct process *p;
	void *uap;
	register_t *retval;
{

	return (ENOSYS);
}
#endif /* COMPAT_43 */
