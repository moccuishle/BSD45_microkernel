/*
 * Copyright (c) 1988 University of Utah.
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
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
 *	@(#)cons.c	8.1 (Berkeley) 6/11/93
 */


#include <sys/param.h>
#include <sys/kprocess.h>
#include <sys/user.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/ioctl.h>
#include <sys/tty.h>
#include <sys/file.h>
#include <sys/conf.h>

#include <i386/i386/cons.h>

/* XXX - all this could be autoconfig()ed */
int pccnprobe(), pccninit(), pccngetc(), pccnputc();
#include "com.h"
#if NCOM > 0
int comcnprobe(), comcninit(), comcngetc(), comcnputc();
#endif

struct	consdev constab[] = {
	{ pccnprobe,	pccninit,	pccngetc,	pccnputc },
#if NCOM > 0
	{ comcnprobe,	comcninit,	comcngetc,	comcnputc },
#endif
	{ 0 },
};
/* end XXX */

struct	tty *constty = 0;	/* virtual console output device */
struct	consdev *cn_tab;	/* physical console device info */
struct	tty *cn_tty;		/* XXX: console tty struct for tprintf */

/* EN: Probes every constab entry, selects the highest-priority console above CN_DEAD as cn_tab, records its tty in cn_tty and calls its cn_init; leaves cn_tab NULL if none. */
/* 中文：遍历 constab 逐个调用 cn_probe，选取优先级高于 CN_DEAD 的最高优先级控制台作为 cn_tab，记录 cn_tty 并调用 cn_init；无可用控制台时 cn_tab 为 NULL。 */
cninit()
{
	register struct consdev *cp;

	/*
	 * Collect information about all possible consoles
	 * and find the one with highest priority
	 */
	for (cp = constab; cp->cn_probe; cp++) {
		(*cp->cn_probe)(cp);
		if (cp->cn_pri > CN_DEAD &&
		    (cn_tab == NULL || cp->cn_pri > cn_tab->cn_pri))
			cn_tab = cp;
	}
	/*
	 * No console, we can handle it
	 */
	if ((cp = cn_tab) == NULL)
		return;
	/*
	 * Turn on console
	 */
	cn_tty = cp->cn_tp;
	(*cp->cn_init)(cp);
}

/* EN: Thin wrapper: if no console is selected returns 0, else substitutes cn_tab->cn_dev and calls that device's cdevsw d_open entry. */
/* 中文：薄封装：未选中控制台时返回 0，否则用 cn_tab->cn_dev 替换 dev 并调用该设备 cdevsw 的 d_open。 */
cnopen(dev, flag, mode, p)
	dev_t dev;
	int flag, mode;
	struct process *p;
{
	if (cn_tab == NULL)
		return (0);
	dev = cn_tab->cn_dev;
	return ((*cdevsw[major(dev)].d_open)(dev, flag, mode, p));
}
 
/* EN: Thin wrapper: returns 0 when cn_tab is NULL, otherwise calls the selected console device's cdevsw d_close with cn_tab->cn_dev. */
/* 中文：薄封装：cn_tab 为 NULL 时返回 0，否则以 cn_tab->cn_dev 调用选中控制台设备的 cdevsw d_close。 */
cnclose(dev, flag, mode, p)
	dev_t dev;
	int flag, mode;
	struct process *p;
{
	if (cn_tab == NULL)
		return (0);
	dev = cn_tab->cn_dev;
	return ((*cdevsw[major(dev)].d_close)(dev, flag, mode, p));
}
 
/* EN: Thin wrapper: returns 0 without a console, else forwards the uio to the selected console's cdevsw d_read using cn_tab->cn_dev. */
/* 中文：薄封装：无控制台时返回 0，否则用 cn_tab->cn_dev 把 uio 读请求转给选中控制台的 d_read。 */
cnread(dev, uio, flag)
	dev_t dev;
	struct uio *uio;
{
	if (cn_tab == NULL)
		return (0);
	dev = cn_tab->cn_dev;
	return ((*cdevsw[major(dev)].d_read)(dev, uio, flag));
}
 
/* EN: Thin wrapper: returns 0 without a console, else forwards the uio to the selected console's cdevsw d_write using cn_tab->cn_dev. */
/* 中文：薄封装：无控制台时返回 0，否则用 cn_tab->cn_dev 把 uio 写请求转给选中控制台的 d_write。 */
cnwrite(dev, uio, flag)
	dev_t dev;
	struct uio *uio;
{
	if (cn_tab == NULL)
		return (0);
	dev = cn_tab->cn_dev;
	return ((*cdevsw[major(dev)].d_write)(dev, uio, flag));
}
 
/* EN: Handles TIOCCONS while a virtual console (constty) exists: requires superuser, clears constty and returns 0; all other commands go to the console's d_ioctl. */
/* 中文：存在虚拟控制台 constty 时处理 TIOCCONS：检查超级用户权限后清除 constty 并返回 0；其余命令转交选中控制台的 d_ioctl。 */
cnioctl(dev, cmd, data, flag, p)
	dev_t dev;
	caddr_t data;
	struct process *p;
{
	int error;

	if (cn_tab == NULL)
		return (0);
	/*
	 * Superuser can always use this to wrest control of console
	 * output from the "virtual" console.
	 */
	if (cmd == TIOCCONS && constty) {
		error = suser(p->p_ucred, (u_short *) NULL);
		if (error)
			return (error);
		constty = NULL;
		return (0);
	}
	dev = cn_tab->cn_dev;
	return ((*cdevsw[major(dev)].d_ioctl)(dev, cmd, data, flag, p));
}

/*ARGSUSED*/
/* EN: Returns 1 (always ready) when cn_tab is NULL, otherwise delegates the select request to ttselect on the selected console's cn_dev. */
/* 中文：cn_tab 为 NULL 时返回 1(总就绪)，否则把 select 请求交给 ttselect 在选中控制台的 cn_dev 上处理。 */
cnselect(dev, rw, p)
	dev_t dev;
	int rw;
	struct process *p;
{
	if (cn_tab == NULL)
		return (1);
	return (ttselect(cn_tab->cn_dev, rw, p));
}

/* EN: Polled character input: returns 0 if no console is selected, otherwise calls cn_tab->cn_getc(cn_tab->cn_dev) to read one character in the kernel. */
/* 中文：轮询取字符：未选中控制台时返回 0，否则调用 cn_tab->cn_getc(cn_tab->cn_dev) 在内核态读入一个字符。 */
cngetc()
{
	if (cn_tab == NULL)
		return (0);
	return ((*cn_tab->cn_getc)(cn_tab->cn_dev));
}

/* EN: Puts character c through cn_tab->cn_putc, following a newline with a carriage return and ignoring c == 0; a no-op when no console is selected. */
/* 中文：经 cn_tab->cn_putc 输出字符 c，遇换行符再补发回车，c 为 0 时忽略；未选中控制台时不做任何输出。 */
cnputc(c)
	register int c;
{
	if (cn_tab == NULL)
		return;
	if (c) {
		(*cn_tab->cn_putc)(cn_tab->cn_dev, c);
		if (c == '\n')
			(*cn_tab->cn_putc)(cn_tab->cn_dev, '\r');
	}
}
