/*
 * Copyright (c) 1982, 1986, 1990, 1993
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
 *	@(#)sys_socket.c	8.3 (Berkeley) 2/14/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kprocess.h>
#include <sys/file.h>
#include <sys/mbuf.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <net/if.h>
#include <net/route.h>

struct	fileops socketops =
    { soo_read, soo_write, soo_ioctl, soo_select, soo_close };

/* 中文：套接字文件的读操作，转调 soreceive 从套接字接收数据 */
/* EN: Read operation for a socket file that calls soreceive to receive data from the socket */
/* ARGSUSED */
int
soo_read(fp, uio, cred)
	struct file *fp;
	struct uio *uio;
	struct ucred *cred;
{

	return (soreceive((struct socket *)fp->f_data, (struct mbuf **)0,
		uio, (struct mbuf **)0, (struct mbuf **)0, (int *)0));
}

/* 中文：套接字文件的写操作，转调 sosend 向套接字发送数据 */
/* EN: Write operation for a socket file that calls sosend to send data to the socket */
/* ARGSUSED */
int
soo_write(fp, uio, cred)
	struct file *fp;
	struct uio *uio;
	struct ucred *cred;
{

	return (sosend((struct socket *)fp->f_data, (struct mbuf *)0,
		uio, (struct mbuf *)0, (struct mbuf *)0, 0));
}

/* 中文：套接字 ioctl：处理 FIONBIO/FIOASYNC/FIONREAD 等命令，并把接口、路由与协议控制命令分发出去 */
/* EN: Socket ioctl handler for FIONBIO/FIOASYNC/FIONREAD and similar commands, dispatching interface, routing and protocol control commands */
int
soo_ioctl(fp, cmd, data, p)
	struct file *fp;
	u_long cmd;
	register caddr_t data;
	struct process *p;
{
	register struct socket *so = (struct socket *)fp->f_data;

	switch (cmd) {

	case FIONBIO:
		if (*(int *)data)
			so->so_state |= SS_NBIO;
		else
			so->so_state &= ~SS_NBIO;
		return (0);

	case FIOASYNC:
		if (*(int *)data) {
			so->so_state |= SS_ASYNC;
			so->so_rcv.sb_flags |= SB_ASYNC;
			so->so_snd.sb_flags |= SB_ASYNC;
		} else {
			so->so_state &= ~SS_ASYNC;
			so->so_rcv.sb_flags &= ~SB_ASYNC;
			so->so_snd.sb_flags &= ~SB_ASYNC;
		}
		return (0);

	case FIONREAD:
		*(int *)data = so->so_rcv.sb_cc;
		return (0);

	case SIOCSPGRP:
		so->so_pgid = *(int *)data;
		return (0);

	case SIOCGPGRP:
		*(int *)data = so->so_pgid;
		return (0);

	case SIOCATMARK:
		*(int *)data = (so->so_state&SS_RCVATMARK) != 0;
		return (0);
	}
	/*
	 * Interface/routing/protocol specific ioctls:
	 * interface and routing ioctls should have a
	 * different entry since a socket's unnecessary
	 */
	if (IOCGROUP(cmd) == 'i')
		return (ifioctl(so, cmd, data, p));
	if (IOCGROUP(cmd) == 'r')
		return (rtioctl(cmd, data, p));
	return ((*so->so_proto->pr_usrreq)(so, PRU_CONTROL, 
	    (struct mbuf *)cmd, (struct mbuf *)data, (struct mbuf *)0));
}

/* 中文：套接字文件的 select 操作，按 FREAD/FWRITE/OOB 检查可读性可写性，未就绪时用 selrecord 登记等待者 */
/* EN: Select operation for a socket file that checks readability/writability/OOB for FREAD/FWRITE and uses selrecord to register waiters when not ready */
int
soo_select(fp, which, p)
	struct file *fp;
	int which;
	struct process *p;
{
	register struct socket *so = (struct socket *)fp->f_data;
	register int s = splnet();

	switch (which) {

	case FREAD:
		if (soreadable(so)) {
			splx(s);
			return (1);
		}
		selrecord(p, &so->so_rcv.sb_sel);
		so->so_rcv.sb_flags |= SB_SEL;
		break;

	case FWRITE:
		if (sowriteable(so)) {
			splx(s);
			return (1);
		}
		selrecord(p, &so->so_snd.sb_sel);
		so->so_snd.sb_flags |= SB_SEL;
		break;

	case 0:
		if (so->so_oobmark || (so->so_state & SS_RCVATMARK)) {
			splx(s);
			return (1);
		}
		selrecord(p, &so->so_rcv.sb_sel);
		so->so_rcv.sb_flags |= SB_SEL;
		break;
	}
	splx(s);
	return (0);
}

/* 中文：套接字文件的 stat 操作，填入 S_IFSOCK 文件类型并通过 PRU_SENSE 获取其余属性 */
/* EN: Stat operation for a socket file that fills in the S_IFSOCK file type and obtains remaining attributes via PRU_SENSE */
int
soo_stat(so, ub)
	register struct socket *so;
	register struct stat *ub;
{

	bzero((caddr_t)ub, sizeof (*ub));
	ub->st_mode = S_IFSOCK;
	return ((*so->so_proto->pr_usrreq)(so, PRU_SENSE,
	    (struct mbuf *)ub, (struct mbuf *)0, 
	    (struct mbuf *)0));
}

/* 中文：套接字文件的关闭操作，若有数据则调用 soclose 关闭套接字并清空 f_data */
/* EN: Close operation for a socket file that calls soclose if data exists and clears f_data */
/* ARGSUSED */
int
soo_close(fp, p)
	struct file *fp;
	struct process *p;
{
	int error = 0;

	if (fp->f_data)
		error = soclose((struct socket *)fp->f_data);
	fp->f_data = 0;
	return (error);
}
