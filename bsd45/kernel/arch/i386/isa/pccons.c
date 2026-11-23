/*-
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * William Jolitz and Don Ahn.
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
 *	@(#)pccons.c	8.1 (Berkeley) 6/11/93
 */

/*
 * code to work keyboard & display for PC-style console
 */
#include <sys/param.h>
#include <sys/conf.h>
#include <sys/ioctl.h>
#include <sys/kprocess.h>
#include <sys/user.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <sys/callout.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/syslog.h>

#include <i386/isa/isa_device.h>
#include <i386/isa/icu.h>
#include <i386/i386/cons.h>

struct	tty pccons;

struct	pcconsoftc {
	char	cs_flags;
#define	CSF_ACTIVE	0x1	/* timeout active */
#define	CSF_POLLING	0x2	/* polling for input */
	char	cs_lastc;	/* last char sent */
	int	cs_timo;	/* timeouts since interrupt */
	u_long	cs_wedgecnt;	/* times restarted */
} pcconsoftc;

int pcprobe(), pcattach();

struct	isa_driver pcdriver = {
	pcprobe, pcattach, "pc",
};

#define	COL		80
#define	ROW		25
#define	CHR		2
#define MONO_BASE	0x3B4
#define MONO_BUF	0xfe0B0000
#define CGA_BASE	0x3D4
#define CGA_BUF		0xfe0B8000
#define IOPHYSMEM	0xA0000

u_char	color = 0xe ;
static unsigned int addr_6845 = MONO_BASE;
u_short *Crtat = (u_short *)MONO_BUF;
static openf;

/*
 * We check the console periodically to make sure
 * that it hasn't wedged.  Unfortunately, if an XOFF
 * is typed on the console, that can't be distinguished
 * from more catastrophic failure.
 */
#define	CN_TIMERVAL	(hz)		/* frequency at which to check cons */
#define	CN_TIMO		(2*60)		/* intervals to allow for output char */

int	pcstart();
int	pcparam();
int	ttrstrt();
char	partab[];

/*
 * Wait for CP to accept last CP command sent
 * before setting up next command.
 */
#define	waitforlast(timo) { \
	if (pclast) { \
		(timo) = 10000; \
		do \
			uncache((char *)&pclast->cp_unit); \
		while ((pclast->cp_unit&CPTAKE) == 0 && --(timo)); \
	} \
}

u_char inb();

/* EN: Resets the keyboard controller via ports 0x64/0x60 (command 0xFF), retries on 0xFE/0xFF errors and waits for the 0xAA self-test reply; always returns 1. */
/* 中文：经端口 0x64/0x60 发送 0xFF RESET 复位键盘控制器，遇到 0xFE/0xFF 重试并等待 0xAA 自检回码；总是返回 1。 */
pcprobe(dev)
struct isa_device *dev;
{
	u_char c;
	int again = 0;

	/* Enable interrupts and keyboard controller */
	while (inb(0x64)&2); outb(0x64,0x60);
	while (inb(0x64)&2); outb(0x60,0x4D);

	/* Start keyboard stuff RESET */
	while (inb(0x64)&2);	/* wait input ready */
	outb(0x60,0xFF);	/* RESET */
	while((c=inb(0x60))!=0xFA) {
		if ((c == 0xFE) ||  (c == 0xFF)) {
			if(!again)printf("KEYBOARD disconnected: RECONNECT \n");
			while (inb(0x64)&2);	/* wait input ready */
			outb(0x60,0xFF);	/* RESET */
			again = 1;
		}
	}
	/* pick up keyboard reset return code */
	while((c=inb(0x60))!=0xAA);
	return 1;
}

/* EN: Distinguishes mono from color by writing 0xA55A through Crtat and printing <mono> or <color>, restores the saved word, then calls cursor(). */
/* 中文：向 Crtat 写入 0xA55A 判断单色或彩色显示并打印 <mono>/<color>，恢复原来的字值，然后调用 cursor()。 */
pcattach(dev)
struct isa_device *dev;
{
	u_short *cp = Crtat + (CGA_BUF-MONO_BUF)/CHR;
	u_short was;

	/* Crtat initialized to point to MONO buffer   */
	/* if not present change to CGA_BUF offset     */
	/* ONLY ADD the difference since locore.s adds */
	/* in the remapped offset at the right time    */

	was = *Crtat;
	*Crtat = (u_short) 0xA55A;
	if (*Crtat != 0xA55A)
		printf("<mono>");
	else	printf("<color>");
	*Crtat = was;
	cursor();
}

/* ARGSUSED */
#ifdef __STDC__
pcopen(dev_t dev, int flag, int mode, struct process *p)
#else
/* EN: Opens console tty: sets t_oproc=pcstart and t_param=pcparam, initializes termios defaults/watermarks on first open, returns EBUSY for non-root exclusive open, then calls l_open. */
/* 中文：打开控制台 tty：设 t_oproc=pcstart、t_param=pcparam，首次打开初始化默认 termios 与水位，非 root 独占打开返回 EBUSY，最后调用 l_open。 */
pcopen(dev, flag, mode, p)
	dev_t dev;
	int flag, mode;
	struct process *p;
#endif
{
	register struct tty *tp;

	tp = &pccons;
	tp->t_oproc = pcstart;
	tp->t_param = pcparam;
	tp->t_dev = dev;
	openf++;
	if ((tp->t_state & TS_ISOPEN) == 0) {
		tp->t_state |= TS_WOPEN;
		ttychars(tp);
		tp->t_iflag = TTYDEF_IFLAG;
		tp->t_oflag = TTYDEF_OFLAG;
		tp->t_cflag = TTYDEF_CFLAG;
		tp->t_lflag = TTYDEF_LFLAG;
		tp->t_ispeed = tp->t_ospeed = TTYDEF_SPEED;
		pcparam(tp, &tp->t_termios);
		ttsetwater(tp);
	} else if (tp->t_state&TS_XCLUDE && p->p_ucred->cr_uid != 0)
		return (EBUSY);
	tp->t_state |= TS_CARR_ON;
	return ((*linesw[tp->t_line].l_open)(dev, tp));
}

/* EN: Closes the console: invokes the current line discipline's l_close then ttyclose(&pccons); unconditionally returns 0. */
/* 中文：关闭控制台：先调用当前线路规程的 l_close，再调用 ttyclose(&pccons)，无条件返回 0。 */
pcclose(dev, flag, mode, p)
	dev_t dev;
	int flag, mode;
	struct process *p;
{
	(*linesw[pccons.t_line].l_close)(&pccons, flag);
	ttyclose(&pccons);
	return(0);
}

/*ARGSUSED*/
/* EN: Thin wrapper: returns linesw[pccons.t_line].l_read(&pccons, uio, flag), delegating the whole read to the currently attached line discipline. */
/* 中文：薄封装：直接返回 linesw[pccons.t_line].l_read(&pccons, uio, flag)，把读取完全交给当前线路规程。 */
pcread(dev, uio, flag)
	dev_t dev;
	struct uio *uio;
{
	return ((*linesw[pccons.t_line].l_read)(&pccons, uio, flag));
}

/*ARGSUSED*/
/* EN: Thin wrapper: returns linesw[pccons.t_line].l_write(&pccons, uio, flag), handing the user output buffer to the current line discipline. */
/* 中文：薄封装：返回 linesw[pccons.t_line].l_write(&pccons, uio, flag)，把用户输出缓冲区交给当前线路规程。 */
pcwrite(dev, uio, flag)
	dev_t dev;
	struct uio *uio;
{
	return ((*linesw[pccons.t_line].l_write)(&pccons, uio, flag));
}

/*
 * Got a console receive interrupt -
 * the console processor wants to give us a character.
 * Catch the character, and see who it goes to.
 */
/* EN: Console receive interrupt: takes a char with sgetc(1), drops it on error, when CSF_POLLING is set or when KDB consumes it, else feeds it to linesw l_rint if openf is set. */
/* 中文：控制台接收中断：用 sgetc(1) 取字符，出错、置 CSF_POLLING 或被 KDB 接管时丢弃，否则在 openf 置位时交给线路规程 l_rint。 */
pcrint(dev, irq, cpl)
	dev_t dev;
{
	int c;

	c = sgetc(1);
	if (c&0x100) return;
	if (pcconsoftc.cs_flags&CSF_POLLING)
		return;
#ifdef KDB
	if (kdbrintr(c, &pccons))
		return;
#endif
	if (openf) /* 386bsd */
		(*linesw[pccons.t_line].l_rint)(c&0xff, &pccons);
}

/* EN: Tries the line discipline's l_ioctl first, then ttioctl, returning the first non-negative error value and ENOTTY when neither handles cmd. */
/* 中文：先调用线路规程 l_ioctl，再调用 ttioctl，返回第一个非负结果；两者都不处理该 cmd 时返回 ENOTTY。 */
pcioctl(dev, cmd, data, flag, p)
	dev_t dev;
	int cmd, flag;
	caddr_t data;
	struct process *p;
{
	register struct tty *tp = &pccons;
	register error;
 
	error = (*linesw[tp->t_line].l_ioctl)(tp, cmd, data, flag, p);
	if (error >= 0)
		return (error);
	error = ttioctl(tp, cmd, data, flag);
	if (error >= 0)
		return (error);
	return (ENOTTY);
}

int	pcconsintr = 1;
/*
 * Got a console transmission interrupt -
 * the console processor wants another character.
 */
/* EN: Console transmit interrupt: returns immediately when pcconsintr is 0, otherwise clears TS_BUSY and the timeout and restarts output via the line discipline l_start or pcstart. */
/* 中文：控制台发送中断：pcconsintr 为 0 时立即返回，否则清 TS_BUSY 与超时标志，经线路规程 l_start 或 pcstart 继续输出。 */
pcxint(dev)
	dev_t dev;
{
	register struct tty *tp;
	register int unit;

	if (!pcconsintr)
		return;
	pccons.t_state &= ~TS_BUSY;
	pcconsoftc.cs_timo = 0;
	if (pccons.t_line)
		(*linesw[pccons.t_line].l_start)(&pccons);
	else
		pcstart(&pccons);
}

/* EN: tty output start routine running under spltty: bails out on TS_TIMEOUT/TS_BUSY/TS_TTSTOP, wakes writers and selectors at low water, and drains t_outq with sput(c, 0x7). */
/* 中文：在 spltty 下运行的 tty 输出启动例程：遇 TS_TIMEOUT/TS_BUSY/TS_TTSTOP 直接返回，低水位时唤醒写者与 select，并用 sput(c,0x7) 排空 t_outq。 */
pcstart(tp)
	register struct tty *tp;
{
	register c, s;

	s = spltty();
	if (tp->t_state & (TS_TIMEOUT|TS_BUSY|TS_TTSTOP))
		goto out;
	do {
		if (tp->t_outq.c_cc <= tp->t_lowat) {
			if (tp->t_state&TS_ASLEEP) {
				tp->t_state &= ~TS_ASLEEP;
				wakeup((caddr_t)&tp->t_outq);
			}
			selwakeup(&tp->t_wsel);
		}
		if (tp->t_outq.c_cc == 0)
			goto out;
		c = getc(&tp->t_outq);
		splx(s);
		sput(c, 0x7);
		s = spltty();
	} while(1);
out:
	splx(s);
}

/* EN: Fills in the consdev: finds the major number whose cdevsw d_open is pcopen, then sets cn_dev=makedev(maj,0), cn_tp=&pccons and cn_pri=CN_INTERNAL. */
/* 中文：填写 consdev：扫描 cdevsw 找出 d_open 等于 pcopen 的主设备号，设置 cn_dev=makedev(maj,0)、cn_tp=&pccons、cn_pri=CN_INTERNAL。 */
pccnprobe(cp)
	struct consdev *cp;
{
	int maj;
	extern int pcopen();

	/* locate the major number */
	for (maj = 0; maj < nchrdev; maj++)
		if (cdevsw[maj].d_open == pcopen)
			break;

	/* initialize required fields */
	cp->cn_dev = makedev(maj, 0);
	cp->cn_tp = &pccons;
	cp->cn_pri = CN_INTERNAL;
}

/* ARGSUSED */
/* EN: Console initialization hook that intentionally does nothing on this port (the crtat reset is commented out); takes the consdev pointer and returns void. */
/* 中文：控制台初始化钩子，本移植版刻意留空（crtat 复位语句被注释掉），接收 consdev 指针且无返回值。 */
pccninit(cp)
	struct consdev *cp;
{
	/*
	 * For now, don't screw with it.
	 */
	/* crtat = 0; */
}

static __color;

/* ARGSUSED */
/* EN: Console putc: turns global __color into an attribute (0 becomes 0x30, else OR 0x60), prepends CR when c is a newline, then sput(c,clr); dev is unused. */
/* 中文：控制台字符输出：把全局 __color 换算成属性（0 取 0x30，否则再或上 0x60），c 为换行时先补发 CR，随后调用 sput(c,clr) 输出；dev 未使用。 */
pccnputc(dev, c)
	dev_t dev;
	char c;
{
	int clr = __color;

	if (clr == 0)
		clr = 0x30;
	else
		clr |= 0x60;
	if (c == '\n')
		sput('\r', clr);
	sput(c, clr);
}

/*
 * Print a character on console.
 */
/* EN: Thin console write wrapper: sput(c,0x2) with a fixed attribute, and after a newline calls getchar() so output blocks until a key is pressed; tp is unused. */
/* 中文：底层控制台输出包装：以固定属性 0x2 调用 sput(c,0x2)，遇到换行后再调 getchar() 阻塞等待按键，起分屏暂停作用；tp 未使用。 */
pcputchar(c, tp)
	char c;
	register struct tty *tp;
{
	sput(c,0x2);
	if (c=='\n') getchar();
}


/* ARGSUSED */
/* EN: Polled keyboard getc: raises spltty to block pcrint, takes a char from sgetc(0), maps CR to newline, restores the old spl with splx and returns it; dev unused. */
/* 中文：轮询取键盘字符：先用 spltty 屏蔽 pcrint，再调 sgetc(0) 取字符并把 CR 转成换行，最后用 splx 恢复原中断级别并返回该字符；dev 未使用。 */
pccngetc(dev)
	dev_t dev;
{
	register int c, s;

	s = spltty();		/* block pcrint while we poll */
	c = sgetc(0);
	if (c == '\r') c = '\n';
	splx(s);
	return (c);
}

/* EN: Returns the next keyboard character from sgetc(0) masked to the low 8 bits; takes no lock and ignores tp. */
/* 中文：调用 sgetc(0) 取下一个键盘字符，只保留低 8 位后返回；函数不加锁，tp 参数未使用。 */
pcgetchar(tp)
	register struct tty *tp;
{
	int c;

	c = sgetc(0);
	return (c&0xff);
}

/*
 * Set line parameters
 */
/* EN: Copies t->c_ispeed, c_ospeed and c_cflag into tp->t_ispeed/t_ospeed/t_cflag and returns 0; no hardware line settings are programmed. */
/* 中文：把 termios 的 c_ispeed、c_ospeed、c_cflag 复制到 tp 的 t_ispeed/t_ospeed/t_cflag 后返回 0，不编程任何硬件线路参数。 */
pcparam(tp, t)
	register struct tty *tp;
	register struct termios *t;
{
	register int cflag = t->c_cflag;
        /* and copy to tty */
        tp->t_ispeed = t->c_ispeed;
        tp->t_ospeed = t->c_ospeed;
        tp->t_cflag = cflag;

	return(0);
}

#ifdef KDB
/*
 * Turn input polling on/off (used by debugger).
 */
/* EN: KDB-only stub with an empty body: accepts the polling on/off argument and changes nothing. */
/* 中文：仅在 KDB 下编译的空函数：接收轮询开关参数 onoff，但函数体为空，不改变任何轮询状态。 */
pcpoll(onoff)
	int onoff;
{
}
#endif

extern int hz;

static beeping;
/* EN: Silences the speaker: clears bits 0-1 of port 0x61 to disable timer channel 2 and sets beeping to 0. */
/* 中文：关闭扬声器：把端口 0x61 的第 0、1 位清零以停用定时器通道 2，并把 beeping 标志置 0。 */
sysbeepstop()
{
	/* disable counter 2 */
	outb(0x61,inb(0x61)&0xFC);
	beeping = 0;
}

/* EN: Starts the beep: enables timer channel 2 via 0x61 and programs 0x43/0x42 for 750 Hz; if not already beeping, arms timeout(sysbeepstop,0,hz/4) and sets beeping=1. */
/* 中文：启动蜂鸣：通过端口 0x61 使能定时器通道 2，并向 0x43/0x42 写入 0xB6 和 0x637 得到 750Hz；若尚未鸣叫则挂 timeout(sysbeepstop,0,hz/4) 并置 beeping=1。 */
sysbeep()
{

	/* enable counter 2 */
	outb(0x61,inb(0x61)|3);
	/* set command for counter 2, 2 byte write */
	outb(0x43,0xB6);
	/* send 0x637 for 750 HZ */
	outb(0x42,0x37);
	outb(0x42,0x06);
	if(!beeping)timeout(sysbeepstop,0,hz/4);
	beeping = 1;
}

/* cursor() sets an offset (0-1999) into the 80x25 text area    */

static u_short *crtat = 0;
char bg_at = 0x0f;
char so_at = 0x70;

cursor()
{ 	int pos = crtat - Crtat;

	outb(addr_6845,14);
	outb(addr_6845+1,pos >> 8);
	outb(addr_6845,15);
	outb(addr_6845+1,pos&0xff);
	timeout(cursor,0,hz/10);
}

u_char shfts, ctls, alts, caps, num, stp, scroll;

/*
 * Compensate for abysmally stupid frame buffer aribitration with macro
 */
#define	wrtchar(c) { do *crtat = (c); while ((c) != *crtat); crtat++; row++; }

/* sput has support for emulation of the 'ibmpc' termcap entry. */
/* This is a bare-bones implementation of a bare-bones entry    */
/* One modification: Change li#24 to li#25 to reflect 25 lines  */

/* EN: 80x25 text core: first call probes mono/CGA buffer and reads cursor from 6845 ports; then handles tab/BS/CR/LF/ESC[ escapes, bell via sysbeep, and scrolls with bcopy/fillw. */
/* 中文：80x25 文本输出核心：首次调用时探测单色/CGA 显存并从 6845 端口读取光标位置；之后处理 tab/退格/CR/LF/ESC[ 转义序列、经 sysbeep 响铃，越界时用 bcopy/fillw 滚屏。 */
sput(c, ca)
u_char c, ca;
{

	static int esc,ebrac,eparm,cx,cy,row,so;

	if (crtat == 0) {
		u_short *cp = Crtat + (CGA_BUF-MONO_BUF)/CHR, was;
		unsigned cursorat;

		/* Crtat initialized to point to MONO buffer   */
		/* if not present change to CGA_BUF offset     */
		/* ONLY ADD the difference since locore.s adds */
		/* in the remapped offset at the right time    */

		was = *cp;
		*cp = (u_short) 0xA55A;
		if (*cp != 0xA55A) {
			addr_6845 = MONO_BASE;
		} else {
			*cp = was;
			addr_6845 = CGA_BASE;
			Crtat = Crtat + (CGA_BUF-MONO_BUF)/CHR;
		}
		/* Extract cursor location */
		outb(addr_6845,14);
		cursorat = inb(addr_6845+1)<<8 ;
		outb(addr_6845,15);
		cursorat |= inb(addr_6845+1);

		crtat = Crtat + cursorat;
		fillw((bg_at<<8)|' ', crtat, COL*ROW-cursorat);
	}
	switch(c) {
	case 0x1B:
		esc = 1; ebrac = 0; eparm = 0;
		break;

	case '\t':
		do {
			wrtchar((ca<<8)| ' ');
		} while (row % 8);
		break;

	case '\010':
		crtat--; row--;
		if (row < 0) row += COL;  /* non-destructive backspace */
		break;

	case '\r':
		crtat -= row ; row = 0;
		break;

	case '\n':
		crtat += COL ;
		break;

	default:
		if (esc) {
			if (ebrac) {
				switch(c) {
				case 'm': /* no support for standout */
					if (!cx) so = 0;
					else so = 1;
					esc = 0; ebrac = 0; eparm = 0;
					break;
				case 'A': /* back one row */
					crtat -= COL;
					esc = 0; ebrac = 0; eparm = 0;
					break;
				case 'B': /* down one row */
					crtat += COL;
					esc = 0; ebrac = 0; eparm = 0;
					break;
				case 'C': /* right cursor */
					crtat++; row++;
					esc = 0; ebrac = 0; eparm = 0;
					break;
				case 'J': /* Clear to end of display */
					fillw((bg_at<<8)+' ', crtat,
						Crtat+COL*ROW-crtat);
					esc = 0; ebrac = 0; eparm = 0;
					break;
				case 'K': /* Clear to EOL */
					fillw((bg_at<<8)+' ', crtat,
						COL-(crtat-Crtat)%COL);
					esc = 0; ebrac = 0; eparm = 0;
					break;
				case 'H': /* Cursor move */
					if ((!cx)||(!cy)) {
						crtat = Crtat;
						row = 0;
					} else {
						crtat = Crtat+(cx-1)*COL+cy-1;
						row = cy-1;
					}
					esc = 0; ebrac = 0; eparm = 0;
					break;
				case ';': /* Switch params in cursor def */
					eparm = 1;
					return;
				default: /* Only numbers valid here */
					if ((c >= '0')&&(c <= '9')) {
						if (eparm) {
							cy *= 10;
							cy += c - '0';
						} else {
							cx *= 10;
							cx += c - '0';
						}
					} else {
						esc = 0; ebrac = 0; eparm = 0;
					}
					return;
				}
				break;
			} else if (c == 'c') { /* Clear screen & home */
				fillw((bg_at<<8)+' ', Crtat,COL*ROW);
				crtat = Crtat; row = 0;
				esc = 0; ebrac = 0; eparm = 0;
			} else if (c == '[') { /* Start ESC [ sequence */
				ebrac = 1; cx = 0; cy = 0; eparm = 0;
			} else { /* Invalid, clear state */
				 esc = 0; ebrac = 0; eparm = 0;
			}
		} else {
			if (c == 7)
				sysbeep();
			/* Print only printables */
			else /*if (c >= ' ') */ {
				if (so) {
					wrtchar((so_at<<8)| c); 
				} else {
					wrtchar((ca<<8)| c); 
				}
				if (row >= COL) row = 0;
				break ;
			}
		}
	}
	if (crtat >= Crtat+COL*(ROW)) { /* scroll check */
		if (openf) do sgetc(1); while (scroll);
		bcopy(Crtat+COL,Crtat,COL*(ROW-1)*CHR);
		fillw ((bg_at<<8) + ' ', Crtat+COL*(ROW-1),COL) ;
		crtat -= COL ;
	}
}

#define	L		0x0001	/* locking function */
#define	SHF		0x0002	/* keyboard shift */
#define	ALT		0x0004	/* alternate shift -- alternate chars */
#define	NUM		0x0008	/* numeric shift  cursors vs. numeric */
#define	CTL		0x0010	/* control shift  -- allows ctl function */
#define	CPS		0x0020	/* caps shift -- swaps case of letter */
#define	ASCII		0x0040	/* ascii code for this key */
#define	STP		0x0080	/* stop output */
#define	FUNC		0x0100	/* function key */
#define	SCROLL		0x0200	/* scroll lock key */

unsigned	__debug = 0; /*0xffe */;
u_short action[] = {
0,     ASCII, ASCII, ASCII, ASCII, ASCII, ASCII, ASCII,		/* scan  0- 7 */
ASCII, ASCII, ASCII, ASCII, ASCII, ASCII, ASCII, ASCII,		/* scan  8-15 */
ASCII, ASCII, ASCII, ASCII, ASCII, ASCII, ASCII, ASCII,		/* scan 16-23 */
ASCII, ASCII, ASCII, ASCII, ASCII,   CTL, ASCII, ASCII,		/* scan 24-31 */
ASCII, ASCII, ASCII, ASCII, ASCII, ASCII, ASCII, ASCII,		/* scan 32-39 */
ASCII, ASCII, SHF  , ASCII, ASCII, ASCII, ASCII, ASCII,		/* scan 40-47 */
ASCII, ASCII, ASCII, ASCII, ASCII, ASCII,  SHF,  ASCII,		/* scan 48-55 */
  ALT, ASCII, CPS  , FUNC , FUNC , FUNC , FUNC , FUNC ,		/* scan 56-63 */
FUNC , FUNC , FUNC , FUNC , FUNC , NUM, SCROLL, ASCII,		/* scan 64-71 */
ASCII, ASCII, ASCII, ASCII, ASCII, ASCII, ASCII, ASCII,		/* scan 72-79 */
ASCII, ASCII, ASCII, ASCII,     0,     0,     0,     0,		/* scan 80-87 */
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,	} ;

u_char unshift[] = {	/* no shift */
0,     033  , '1'  , '2'  , '3'  , '4'  , '5'  , '6'  ,		/* scan  0- 7 */
'7'  , '8'  , '9'  , '0'  , '-'  , '='  , 0177 ,'\t'  ,		/* scan  8-15 */

'q'  , 'w'  , 'e'  , 'r'  , 't'  , 'y'  , 'u'  , 'i'  ,		/* scan 16-23 */
'o'  , 'p'  , '['  , ']'  , '\r' , CTL  , 'a'  , 's'  ,		/* scan 24-31 */

'd'  , 'f'  , 'g'  , 'h'  , 'j'  , 'k'  , 'l'  , ';'  ,		/* scan 32-39 */
'\'' , '`'  , SHF  , '\\' , 'z'  , 'x'  , 'c'  , 'v'  ,		/* scan 40-47 */

'b'  , 'n'  , 'm'  , ','  , '.'  , '/'  , SHF  ,   '*',		/* scan 48-55 */
ALT  , ' '  , CPS,     1,     2,    3 ,     4,     5,		/* scan 56-63 */

    6,     7,     8,     9,    10, NUM, STP,   '7',		/* scan 64-71 */
  '8',   '9',   '-',   '4',   '5',   '6',   '+',   '1',		/* scan 72-79 */

  '2',   '3',   '0',   '.',     0,     0,     0,     0,		/* scan 80-87 */
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,	} ;

u_char shift[] = {	/* shift shift */
0,     033  , '!'  , '@'  , '#'  , '$'  , '%'  , '^'  ,		/* scan  0- 7 */
'&'  , '*'  , '('  , ')'  , '_'  , '+'  , 0177 ,'\t'  ,		/* scan  8-15 */
'Q'  , 'W'  , 'E'  , 'R'  , 'T'  , 'Y'  , 'U'  , 'I'  ,		/* scan 16-23 */
'O'  , 'P'  , '{'  , '}'  , '\r' , CTL  , 'A'  , 'S'  ,		/* scan 24-31 */
'D'  , 'F'  , 'G'  , 'H'  , 'J'  , 'K'  , 'L'  , ':'  ,		/* scan 32-39 */
'"'  , '~'  , SHF  , '|'  , 'Z'  , 'X'  , 'C'  , 'V'  ,		/* scan 40-47 */
'B'  , 'N'  , 'M'  , '<'  , '>'  , '?'  , SHF  ,   '*',		/* scan 48-55 */
ALT  , ' '  , CPS,     0,     0, ' '  ,     0,     0,		/* scan 56-63 */
    0,     0,     0,     0,     0, NUM, STP,   '7',		/* scan 64-71 */
  '8',   '9',   '-',   '4',   '5',   '6',   '+',   '1',		/* scan 72-79 */
  '2',   '3',   '0',   '.',     0,     0,     0,     0,		/* scan 80-87 */
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,	} ;

u_char ctl[] = {	/* CTL shift */
0,     033  , '!'  , 000  , '#'  , '$'  , '%'  , 036  ,		/* scan  0- 7 */
'&'  , '*'  , '('  , ')'  , 037  , '+'  , 034  ,'\177',		/* scan  8-15 */
021  , 027  , 005  , 022  , 024  , 031  , 025  , 011  ,		/* scan 16-23 */
017  , 020  , 033  , 035  , '\r' , CTL  , 001  , 023  ,		/* scan 24-31 */
004  , 006  , 007  , 010  , 012  , 013  , 014  , ';'  ,		/* scan 32-39 */
'\'' , '`'  , SHF  , 034  , 032  , 030  , 003  , 026  ,		/* scan 40-47 */
002  , 016  , 015  , '<'  , '>'  , '?'  , SHF  ,   '*',		/* scan 48-55 */
ALT  , ' '  , CPS,     0,     0, ' '  ,     0,     0,		/* scan 56-63 */
CPS,     0,     0,     0,     0,     0,     0,     0,		/* scan 64-71 */
    0,     0,     0,     0,     0,     0,     0,     0,		/* scan 72-79 */
    0,     0,     0,     0,     0,     0,     0,     0,		/* scan 80-87 */
    0,     0,   033, '7'  , '4'  , '1'  ,     0, NUM,		/* scan 88-95 */
'8'  , '5'  , '2'  ,     0, STP, '9'  , '6'  , '3'  ,		/*scan  96-103*/
'.'  ,     0, '*'  , '-'  , '+'  ,     0,     0,     0,		/*scan 104-111*/
0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,	} ;

#ifdef notdef
struct key {
	u_short action;		/* how this key functions */
	char	ascii[8];	/* ascii result character indexed by shifts */
};
#endif


#define	KBSTAT		0x64	/* kbd status port */
#define	KBS_INP_BUF_FUL	0x02	/* kbd char ready */
#define	KBDATA		0x60	/* kbd data port */
#define	KBSTATUSPORT	0x61	/* kbd status */

/* EN: Programs the keyboard LEDs: waits for the port 0x64 input buffer to clear, sends command 0xED to 0x60, then the mask scroll|2*num|4*caps. */
/* 中文：更新键盘指示灯：轮询端口 0x64 等待输入缓冲空闲，先向 0x60 发 0xED 命令，再发由 scroll|2*num|4*caps 组成的灯位掩码。 */
update_led()
{
	while (inb(0x64)&2);	/* wait input ready */
	outb(0x60,0xED);	/* LED Command */
	while (inb(0x64)&2);	/* wait input ready */
	outb(0x60,scroll | 2*num | 4*caps);
}

/* 中文：通过向键盘控制器发送 0xFE 命令重启 CPU（随后无限循环等待复位）。 */
/* EN: Reboot the CPU by sending command 0xFE to the keyboard controller (then spins forever awaiting reset). */
reset_cpu() {
	while(1) {
		while (inb(0x64)&2);	/* wait input ready */
		outb(0x64,0xFE);	/* Reset Command */
		DELAY(4000000);
		while (inb(0x64)&2);	/* wait input ready */
		outb(0x64,0xFF);	/* Keyboard Reset Command */
	}
	/* NOTREACHED */
}

/*
sgetc(noblock) : get a character from the keyboard. If noblock = 0 wait until
a key is gotten.  Otherwise return a 0x100 (256).
*/
/* EN: Keyboard decode: polls 0x64/0x60, ctrl-alt-del calls _exit(), maps scancodes via unshift/shift/ctl tables, toggles locks with update_led; returns 0x100 if noblock and no key. */
/* 中文：键盘扫描码解码：轮询 0x64/0x60 端口，Ctrl-Alt-Del 调用 _exit()，按 unshift/shift/ctl 表解释通断码并翻转锁定键后调 update_led；noblock 且无键时返回 0x100。 */
int sgetc(noblock)
{
	u_char dt; unsigned key;
loop:
	/* First see if there is something in the keyboard port */
	if (inb(KBSTAT)&1) dt = inb(KBDATA);
	else { if (noblock) return (0x100); else goto loop; }

	/* Check for cntl-alt-del */
	if ((dt == 83)&&ctls&&alts) _exit();

	/* Check for make/break */
	if (dt & 0x80) {
		/* break */
		dt = dt & 0x7f ;
		switch (action[dt]) {
		case SHF: shfts = 0; break;
		case ALT: alts = 0; break;
		case CTL: ctls = 0; break;
		case FUNC:
			/* Toggle debug flags */
			key = unshift[dt];
			if(__debug & (1<<key)) __debug &= ~(1<<key) ;
			else __debug |= (1<<key) ;
			break;
		}
	} else {
		/* make */
		dt = dt & 0x7f ;
		switch (action[dt]) {
		/* LOCKING KEYS */
		case NUM: num ^= 1; update_led(); break;
		case CPS: caps ^= 1; update_led(); break;
		case SCROLL: scroll ^= 1; update_led(); break;
		case STP: stp ^= 1; if(stp) goto loop; break;

		/* NON-LOCKING KEYS */
		case SHF: shfts = 1; break;
		case ALT: alts = 1; break;
		case CTL: ctls = 1; break;
		case ASCII:
			if (shfts) dt = shift[dt];
			else if (ctls) dt = ctl[dt];
			else dt = unshift[dt];
			if (caps && (dt >= 'a' && dt <= 'z')) dt -= 'a' - 'A';
			return(dt);
		}
	}
	if (noblock) return (0x100); else goto loop;
}

pg(p,q,r,s,t,u,v,w,x,y,z) char *p; {
	printf(p,q,r,s,t,u,v,w,x,y,z);
	printf("\n");
	return(getchar());
}

/* special characters */
#define bs	8
#define lf	10	
#define cr	13	
#define cntlc	3	
#define del	0177	
#define cntld	4

/* EN: Polled debugger console input: sets CSF_POLLING under splhigh, prompts '>', blocks in sgetc(0), clears the flag, echoes the char (CR/LF, BS/DEL) and returns it, 0 on ^D. */
/* 中文：调试用轮询控制台输入：在 splhigh 下设 CSF_POLLING 并输出 '>' 提示，阻塞于 sgetc(0)，取到后清标志并回显（处理 CR/LF、BS/DEL），遇 ^D 返回 0。 */
getchar()
{
	register char thechar;
	register delay;
	int x;

	pcconsoftc.cs_flags |= CSF_POLLING;
	x=splhigh();
	sput('>',0x6);
	/*while (1) {*/
		thechar = (char) sgetc(0);
		pcconsoftc.cs_flags &= ~CSF_POLLING;
		splx(x);
		switch (thechar) {
		    default: if (thechar >= ' ')
			     	sput(thechar,0x6);
			     return(thechar);
		    case cr:
		    case lf: sput(cr,0x6);
			     sput(lf,0x6);
			     return(lf);
		    case bs:
		    case del:
			     sput(bs,0x6);
			     sput(' ',0x6);
			     sput(bs,0x6);
			     return(thechar);
		    /*case cntlc:
			     sput('^',0xe) ; sput('C',0xe) ; sput('\r',0xe) ; sput('\n',0xe) ;
			     _exit(-2) ; */
		    case cntld:
			     sput('^',0x6) ; sput('D',0x6) ; sput('\r',0x6) ; sput('\n',0x6) ;
			     return(0);
		}
	/*}*/
}

#include <machine/dbg.h>
#include <machine/stdarg.h>
static nrow;

void
#ifdef __STDC__
dprintf(unsigned flgs, const char *fmt, ...)
#else
dprintf(flgs, fmt /*, va_alist */)
        char *fmt;
	unsigned flgs;
#endif
{	extern unsigned __debug;
	va_list ap;

	if((flgs&__debug) > DPAUSE) {
		__color = ffs(flgs&__debug)+1;
		va_start(ap,fmt);
		kprintf(fmt, 1, (struct tty *)0, ap);
		va_end(ap);
	if (flgs&DPAUSE || nrow%24 == 23) { 
		int x;
		x = splhigh();
		if (nrow%24 == 23) nrow = 0;
		sgetc(0);
		splx(x);
	}
	}
	__color = 0;
}

consinit() {}
