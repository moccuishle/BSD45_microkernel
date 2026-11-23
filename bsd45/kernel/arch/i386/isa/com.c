/*-
 * Copyright (c) 1991, 1993
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
 *	@(#)com.c	8.1 (Berkeley) 6/11/93
 */

#include "com.h"
#if NCOM > 0
/*
 * COM driver, based on HP dca driver
 * uses National Semiconductor NS16450/NS16550AF UART
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/ioctl.h>
#include <sys/tty.h>
#include <sys/kprocess.h>
#include <sys/user.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <sys/kernel.h>
#include <sys/syslog.h>

#include <i386/isa/isa_device.h>
#include <i386/isa/comreg.h>
#include <i386/isa/ic/ns16550.h>

void	comstart();
int 	comprobe(), comattach(), comintr(), comparam();

struct	isa_driver comdriver = {
	comprobe, comattach, "com"
};

int	comsoftCAR;
int	com_active;
int	com_hasfifo;
int	ncom = NCOM;
#ifdef COMCONSOLE
int	comconsole = COMCONSOLE;
#else
int	comconsole = -1;
#endif
int	comconsinit;
int	comdefaultrate = TTYDEF_SPEED;
int	commajor;
short com_addr[NCOM];
struct	tty com_tty[NCOM];

struct speedtab comspeedtab[] = {
	0,	0,
	50,	COMBRD(50),
	75,	COMBRD(75),
	110,	COMBRD(110),
	134,	COMBRD(134),
	150,	COMBRD(150),
	200,	COMBRD(200),
	300,	COMBRD(300),
	600,	COMBRD(600),
	1200,	COMBRD(1200),
	1800,	COMBRD(1800),
	2400,	COMBRD(2400),
	4800,	COMBRD(4800),
	9600,	COMBRD(9600),
	19200,	COMBRD(19200),
	38400,	COMBRD(38400),
	57600,	COMBRD(57600),
	-1,	-1
};

extern	struct tty *constty;
#ifdef KGDB
#include <machine/remote-sl.h>

extern int kgdb_dev;
extern int kgdb_rate;
extern int kgdb_debug_init;
#endif

#define	UNIT(x)		minor(x)

/* EN: Probes a UART by clearing the CFCR DLAB bit and writing then reading IIR; both branches return 1, so it always reports the device as present. */
/* 中文：探测 UART：先清除 CFCR 的 DLAB 位以访问标识寄存器，向 IIR 写入再回读；两个分支都返回 1，因此总是报告设备存在。 */
comprobe(dev)
struct isa_device *dev;
{
	/* force access to id reg */
	outb(dev->id_iobase+com_cfcr, inb (dev->id_iobase+com_cfcr) & ~0x80);
	outb(dev->id_iobase+com_iir, 0);
	if ((inb(dev->id_iobase+com_iir) & 0x38) == 0)
		return(1);
	return(1);

}


/* EN: Records isdp's iobase in com_addr, marks the unit active, enables and detects 16550 FIFOs via IIR, sets MCR_IENABLE, clears IER and handles console/KGDB setup; returns 1. */
/* 中文：把 isdp 的 iobase 记入 com_addr 并将该单元标记为活动，经 IIR 探测并启用 16550 FIFO，置 MCR_IENABLE、清 IER，处理控制台/KGDB 场景后返回 1。 */
int
comattach(isdp)
struct isa_device *isdp;
{
	struct	tty	*tp;
	u_char		unit;
	int		port = isdp->id_iobase;

	unit = isdp->id_unit;
	if (unit == comconsole)
		DELAY(100000);
	com_addr[unit] = port;
	com_active |= 1 << unit;
	comsoftCAR |= 1 << unit;	/* XXX */

	/* look for a NS 16550AF UART with FIFOs */
	outb(port+com_fifo, FIFO_ENABLE|FIFO_RCV_RST|FIFO_XMT_RST|FIFO_TRIGGER_14);
	DELAY(100);
	if ((inb(port+com_iir) & IIR_FIFO_MASK) == IIR_FIFO_MASK)
		com_hasfifo |= 1 << unit;

	outb(port+com_ier, 0);
	outb(port+com_mcr, 0 | MCR_IENABLE);
#ifdef KGDB
	if (kgdb_dev == makedev(commajor, unit)) {
		if (comconsole == unit)
			kgdb_dev = -1;	/* can't debug over console port */
		else {
			(void) cominit(unit, kgdb_rate);
			if (kgdb_debug_init) {
				/*
				 * Print prefix of device name,
				 * let kgdb_connect print the rest.
				 */
				printf("com%d: ", unit);
				kgdb_connect(1);
			} else
				printf("com%d: kgdb enabled\n", unit);
		}
	}
#endif
	/*
	 * Need to reset baud rate, etc. of next print so reset comconsinit.
	 * Also make sure console is always "hardwired"
	 */
	if (unit == comconsole) {
		comconsinit = 0;
		comsoftCAR |= (1 << unit);
	}
	return (1);
}

/* ARGSUSED */
#ifdef __STDC__
comopen(dev_t dev, int flag, int mode, struct process *p)
#else
/* EN: Opens a com tty: ENXIO for inactive units, installs line-discipline hooks and defaults, raises DTR/RTS, sleeps for carrier unless CLOCAL/O_NONBLOCK, then calls l_open. */
/* 中文：打开 com tty：无活动单元返回 ENXIO，安装线路规程钩子与默认参数，拉起 DTR/RTS，在非 CLOCAL/O_NONBLOCK 时睡眠等待载波，最后调用 l_open。 */
comopen(dev, flag, mode, p)
	dev_t dev;
	int flag, mode;
	struct process *p;
#endif
{
	register struct tty *tp;
	register int unit;
	int error = 0;
 
	unit = UNIT(dev);
	if (unit >= NCOM || (com_active & (1 << unit)) == 0)
		return (ENXIO);
	tp = &com_tty[unit];
	tp->t_oproc = comstart;
	tp->t_param = comparam;
	tp->t_dev = dev;
	if ((tp->t_state & TS_ISOPEN) == 0) {
		tp->t_state |= TS_WOPEN;
		ttychars(tp);
		if (tp->t_ispeed == 0) {
			tp->t_iflag = TTYDEF_IFLAG;
			tp->t_oflag = TTYDEF_OFLAG;
			tp->t_cflag = TTYDEF_CFLAG;
			tp->t_lflag = TTYDEF_LFLAG;
			tp->t_ispeed = tp->t_ospeed = comdefaultrate;
		}
		comparam(tp, &tp->t_termios);
		ttsetwater(tp);
	} else if (tp->t_state&TS_XCLUDE && p->p_ucred->cr_uid != 0)
		return (EBUSY);
	(void) spltty();
	(void) commctl(dev, MCR_DTR | MCR_RTS, DMSET);
	if ((comsoftCAR & (1 << unit)) || (commctl(dev, 0, DMGET) & MSR_DCD))
		tp->t_state |= TS_CARR_ON;
	while ((flag&O_NONBLOCK) == 0 && (tp->t_cflag&CLOCAL) == 0 &&
	       (tp->t_state & TS_CARR_ON) == 0) {
		tp->t_state |= TS_WOPEN;
		if (error = ttysleep(tp, (caddr_t)&tp->t_rawq, TTIPRI | PCATCH,
		    ttopen, 0))
			break;
	}
	(void) spl0();
	if (error == 0)
		error = (*linesw[tp->t_line].l_open)(dev, tp);
	return (error);
}
 
/*ARGSUSED*/
/* EN: Closes a com tty: calls l_close, clears CFCR_SBREAK, clears IER unless this is the KGDB port, drops DTR/RTS when HUPCL or half-open, then ttyclose; returns 0. */
/* 中文：关闭 com tty：调用 l_close，清除 CFCR_SBREAK，非 KGDB 端口则清 IER，在 HUPCL 或半开状态下撤销 DTR/RTS，最后 ttyclose 并返回 0。 */
comclose(dev, flag, mode, p)
	dev_t dev;
	int flag, mode;
	struct process *p;
{
	register struct tty *tp;
	register com;
	register int unit;
 
	unit = UNIT(dev);
	com = com_addr[unit];
	tp = &com_tty[unit];
	(*linesw[tp->t_line].l_close)(tp, flag);
	outb(com+com_cfcr, inb(com+com_cfcr) & ~CFCR_SBREAK);
#ifdef KGDB
	/* do not disable interrupts if debugging */
	if (kgdb_dev != makedev(commajor, unit))
#endif
	outb(com+com_ier, 0);
	if (tp->t_cflag&HUPCL || tp->t_state&TS_WOPEN || 
	    (tp->t_state&TS_ISOPEN) == 0)
		(void) commctl(dev, 0, DMSET);
	ttyclose(tp);
	return(0);
}
 
/* EN: Thin wrapper: passes the read straight to linesw[tp->t_line].l_read for the tty selected by UNIT(dev). */
/* 中文：薄封装：把读请求直接转交给 UNIT(dev) 所选 tty 的线路规程 linesw[tp->t_line].l_read。 */
comread(dev, uio, flag)
	dev_t dev;
	struct uio *uio;
{
	register struct tty *tp = &com_tty[UNIT(dev)];
 
	return ((*linesw[tp->t_line].l_read)(tp, uio, flag));
}
 
/* EN: Clears constty when writing to the serial console unit (disallowing a virtual console there), then forwards the write to the tty's l_write handler. */
/* 中文：向串口控制台单元写入时先清空 constty（禁止该处使用虚拟控制台），再把写请求转发给该 tty 的 l_write 处理函数。 */
comwrite(dev, uio, flag)
	dev_t dev;
	struct uio *uio;
{
	int unit = UNIT(dev);
	register struct tty *tp = &com_tty[unit];
 
	/*
	 * (XXX) We disallow virtual consoles if the physical console is
	 * a serial port.  This is in case there is a display attached that
	 * is not the console.  In that situation we don't need/want the X
	 * server taking over the console.
	 */
	if (constty && unit == comconsole)
		constty = NULL;
	return ((*linesw[tp->t_line].l_write)(tp, uio, flag));
}
 
/* EN: UART interrupt loop on IIR: feeds bytes to l_rint (or kgdb) and drains the FIFO, restarts output on TXRDY after clearing TS_BUSY/TS_FLUSH, logs unknown codes then modem status. */
/* 中文：UART 中断处理：循环读取 IIR，把接收字节交给 l_rint（或 kgdb）并排空 FIFO，TXRDY 时清 TS_BUSY/TS_FLUSH 重启发送，未知码记日志后按调制解调器状态处理。 */
comintr(unit)
	register int unit;
{
	register com;
	register u_char code;
	register struct tty *tp;

	com = com_addr[unit];
	while (1) {
		code = inb(com+com_iir);
		switch (code & IIR_IMASK) {
		case IIR_NOPEND:
			return (1);
		case IIR_RXTOUT:
		case IIR_RXRDY:
			tp = &com_tty[unit];
/*
 * Process received bytes.  Inline for speed...
 */
#ifdef KGDB
#define	RCVBYTE() \
			code = inb(com+com_data); \
			if ((tp->t_state & TS_ISOPEN) == 0) { \
				if (kgdb_dev == makedev(commajor, unit) && \
				    code == FRAME_END) \
					kgdb_connect(0); /* trap into kgdb */ \
			} else \
				(*linesw[tp->t_line].l_rint)(code, tp)
#else
#define	RCVBYTE() \
			code = inb(com+com_data); \
			if (tp->t_state & TS_ISOPEN) \
				(*linesw[tp->t_line].l_rint)(code, tp)
#endif

			RCVBYTE();

			if (com_hasfifo & (1 << unit))
				while ((code = inb(com+com_lsr)) & LSR_RCV_MASK) {
					if (code == LSR_RXRDY) {
						RCVBYTE();
					} else
						comeint(unit, code, com);
				}
			break;
		case IIR_TXRDY:
			tp = &com_tty[unit];
			tp->t_state &=~ (TS_BUSY|TS_FLUSH);
			if (tp->t_line)
				(*linesw[tp->t_line].l_start)(tp);
			else
				comstart(tp);
			break;
		case IIR_RLS:
			comeint(unit, inb(com+com_lsr), com);
			break;
		default:
			if (code & IIR_NOPEND)
				return (1);
			log(LOG_WARNING, "com%d: weird interrupt: 0x%x\n",
			    unit, code);
			/* fall through */
		case IIR_MLSC:
			commint(unit, com);
			break;
		}
	}
}

/* EN: Receiver line-status handler: reads the data byte, tags TTY_FE for break/framing or TTY_PE for parity, logs silo overflow on OE, and hands the byte to l_rint. */
/* 中文：接收线路状态处理：读出数据字节，断帧/帧错置 TTY_FE、奇偶错置 TTY_PE、OE 记 FIFO 溢出日志，最后把字节交给 l_rint。 */
comeint(unit, stat, com)
	register int unit, stat;
	register com;
{
	register struct tty *tp;
	register int c;

	tp = &com_tty[unit];
	c = inb(com+com_data);
	if ((tp->t_state & TS_ISOPEN) == 0) {
#ifdef KGDB
		/* we don't care about parity errors */
		if (((stat & (LSR_BI|LSR_FE|LSR_PE)) == LSR_PE) &&
		    kgdb_dev == makedev(commajor, unit) && c == FRAME_END)
			kgdb_connect(0); /* trap into kgdb */
#endif
		return;
	}
	if (stat & (LSR_BI | LSR_FE))
		c |= TTY_FE;
	else if (stat & LSR_PE)
		c |= TTY_PE;
	else if (stat & LSR_OE)
		log(LOG_WARNING, "com%d: silo overflow\n", unit);
	(*linesw[tp->t_line].l_rint)(c, tp);
}

/* EN: Modem-status handler: reports DCD changes to l_modem (dropping DTR/RTS if it returns 0) and implements RTS/CTS flow control by toggling TS_TTSTOP and calling ttstart. */
/* 中文：调制解调器状态处理：把 DCD 变化报告给 l_modem（其返回 0 时撤销 DTR/RTS），并在 CRTSCTS 下按 CTS 置位或清除 TS_TTSTOP 并调用 ttstart 实现流控。 */
commint(unit, com)
	register int unit;
	register com;
{
	register struct tty *tp;
	register int stat;

	tp = &com_tty[unit];
	stat = inb(com+com_msr);
	if ((stat & MSR_DDCD) && (comsoftCAR & (1 << unit)) == 0) {
		if (stat & MSR_DCD)
			(void)(*linesw[tp->t_line].l_modem)(tp, 1);
		else if ((*linesw[tp->t_line].l_modem)(tp, 0) == 0)
			outb(com+com_mcr,
				inb(com+com_mcr) & ~(MCR_DTR | MCR_RTS) | MCR_IENABLE);
	} else if ((stat & MSR_DCTS) && (tp->t_state & TS_ISOPEN) &&
		   (tp->t_flags & CRTSCTS)) {
		/* the line is up and we want to do rts/cts flow control */
		if (stat & MSR_CTS) {
			tp->t_state &=~ TS_TTSTOP;
			ttstart(tp);
		} else
			tp->t_state |= TS_TTSTOP;
	}
}

/* EN: Ioctl for com: tries l_ioctl then ttioctl, then handles TIOCSBRK/TIOCCBRK via CFCR_SBREAK and the modem-line ioctls via commctl, returning ENOTTY otherwise. */
/* 中文：com 的 ioctl：先试 l_ioctl 与 ttioctl，再通过 CFCR_SBREAK 处理 TIOCSBRK/TIOCCBRK，其余调制解调器线路控制经 commctl 处理，否则返回 ENOTTY。 */
comioctl(dev, cmd, data, flag, p)
	dev_t dev;
	int cmd, flag;
	caddr_t data;
	struct process *p;
{
	register struct tty *tp;
	register int unit = UNIT(dev);
	register com;
	register int error;
 
	tp = &com_tty[unit];
	error = (*linesw[tp->t_line].l_ioctl)(tp, cmd, data, flag, p);
	if (error >= 0)
		return (error);
	error = ttioctl(tp, cmd, data, flag);
	if (error >= 0)
		return (error);

	com = com_addr[unit];
	switch (cmd) {

	case TIOCSBRK:
		outb(com+com_cfcr, inb(com+com_cfcr) | CFCR_SBREAK);
		break;

	case TIOCCBRK:
		outb(com+com_cfcr, inb(com+com_cfcr) & ~CFCR_SBREAK);
		break;

	case TIOCSDTR:
		(void) commctl(dev, MCR_DTR | MCR_RTS, DMBIS);
		break;

	case TIOCCDTR:
		(void) commctl(dev, MCR_DTR | MCR_RTS, DMBIC);
		break;

	case TIOCMSET:
		(void) commctl(dev, *(int *)data, DMSET);
		break;

	case TIOCMBIS:
		(void) commctl(dev, *(int *)data, DMBIS);
		break;

	case TIOCMBIC:
		(void) commctl(dev, *(int *)data, DMBIC);
		break;

	case TIOCMGET:
		*(int *)data = commctl(dev, 0, DMGET);
		break;

	default:
		return (ENOTTY);
	}
	return (0);
}

/* EN: Programs line parameters: EINVAL on bad or mismatched speed, copies speeds/cflag to the tty, writes the baud divisor via CFCR_DLAB, sets word/parity/stop bits and the FIFO. */
/* 中文：设置线路参数：速率非法或不匹配返回 EINVAL，把速率与 cflag 抄入 tty，经 CFCR_DLAB 写入波特率除数，设置字长/校验/停止位并重开 FIFO。 */
comparam(tp, t)
	register struct tty *tp;
	register struct termios *t;
{
	register com;
	register int cfcr, cflag = t->c_cflag;
	int unit = UNIT(tp->t_dev);
	int ospeed = ttspeedtab(t->c_ospeed, comspeedtab);
 
	/* check requested parameters */
        if (ospeed < 0 || (t->c_ispeed && t->c_ispeed != t->c_ospeed))
                return(EINVAL);
        /* and copy to tty */
        tp->t_ispeed = t->c_ispeed;
        tp->t_ospeed = t->c_ospeed;
        tp->t_cflag = cflag;

	com = com_addr[unit];
	outb(com+com_ier, IER_ERXRDY | IER_ETXRDY | IER_ERLS /*| IER_EMSC*/);
	if (ospeed == 0) {
		(void) commctl(unit, 0, DMSET);	/* hang up line */
		return(0);
	}
	outb(com+com_cfcr, inb(com+com_cfcr) | CFCR_DLAB);
	outb(com+com_data, ospeed & 0xFF);
	outb(com+com_ier, ospeed >> 8);
	switch (cflag&CSIZE) {
	case CS5:
		cfcr = CFCR_5BITS; break;
	case CS6:
		cfcr = CFCR_6BITS; break;
	case CS7:
		cfcr = CFCR_7BITS; break;
	case CS8:
		cfcr = CFCR_8BITS; break;
	}
	if (cflag&PARENB) {
		cfcr |= CFCR_PENAB;
		if ((cflag&PARODD) == 0)
			cfcr |= CFCR_PEVEN;
	}
	if (cflag&CSTOPB)
		cfcr |= CFCR_STOPB;
	outb(com+com_cfcr, cfcr);

	if (com_hasfifo & (1 << unit))
		outb(com+com_fifo, FIFO_ENABLE | FIFO_TRIGGER_14);

	return(0);
}
 
/* EN: Starts output under spltty: returns if TS_TIMEOUT/TS_TTSTOP is set, wakes l_write/select waiters at low water, then sends one byte (up to 16 with FIFO) and sets TS_BUSY. */
/* 中文：在 spltty 下启动发送：置有 TS_TIMEOUT/TS_TTSTOP 时直接返回，输出队列降到低水位时唤醒 l_write/select 等待者，然后发送 1 字节（有 FIFO 最多 16 字节）并置 TS_BUSY。 */
void
comstart(tp)
	register struct tty *tp;
{
	register com;
	int s, unit, c;
 
	unit = UNIT(tp->t_dev);
	com = com_addr[unit];
	s = spltty();
	if (tp->t_state & (TS_TIMEOUT|TS_TTSTOP))
		goto out;
	if (tp->t_outq.c_cc <= tp->t_lowat) {
		if (tp->t_state&TS_ASLEEP) {
			tp->t_state &= ~TS_ASLEEP;
			wakeup((caddr_t)&tp->t_outq);
		}
		selwakeup(&tp->t_wsel);
	}
	if (tp->t_outq.c_cc == 0)
		goto out;
	if (inb(com+com_lsr) & LSR_TXRDY) {
		c = getc(&tp->t_outq);
		tp->t_state |= TS_BUSY;
		outb(com+com_data, c);
		if (com_hasfifo & (1 << unit))
			for (c = 1; c < 16 && tp->t_outq.c_cc; ++c)
				outb(com+com_data, getc(&tp->t_outq));
	}
out:
	splx(s);
}
 
/*
 * Stop output on a line.
 */
/*ARGSUSED*/
/* EN: Raises spltty; for a busy tty it sets TS_FLUSH (unless TS_TTSTOP) so the next interrupt drains pending output, then restores the mask; returns nothing. */
/* 中文：提升到 spltty 优先级；对正在发送的 tty 置 TS_FLUSH（已有 TS_TTSTOP 时除外），使中断处理程序清空待发数据，然后恢复中断屏蔽；无返回值。 */
comstop(tp, flag)
	register struct tty *tp;
{
	register int s;

	s = spltty();
	if (tp->t_state & TS_BUSY) {
		if ((tp->t_state&TS_TTSTOP)==0)
			tp->t_state |= TS_FLUSH;
	}
	splx(s);
}
 
/* EN: Under spltty applies how (DMSET/DMBIS/DMBIC) to the unit's com_mcr modem control register, always keeping MCR_IENABLE, or for DMGET returns the com_msr status. */
/* 中文：在 spltty 下按 how 对单元 com_mcr 调制解调器控制寄存器执行 DMSET/DMBIS/DMBIC 读改写并始终保留 MCR_IENABLE；DMGET 时改为返回 com_msr 状态。 */
commctl(dev, bits, how)
	dev_t dev;
	int bits, how;
{
	register com;
	register int unit;
	int s;

	unit = UNIT(dev);
	com = com_addr[unit];
	s = spltty();
	switch (how) {

	case DMSET:
		outb(com+com_mcr, bits | MCR_IENABLE);
		break;

	case DMBIS:
		outb(com+com_mcr, inb(com+com_mcr) | bits | MCR_IENABLE);
		break;

	case DMBIC:
		outb(com+com_mcr, inb(com+com_mcr) & ~bits | MCR_IENABLE);
		break;

	case DMGET:
		bits = inb(com+com_msr);
		break;
	}
	(void) splx(s);
	return(bits);
}

/*
 * Following are all routines needed for COM to act as console
 */
#include <i386/i386/cons.h>

/* EN: Scans cdevsw for the major whose d_open is comopen, hardwires unit CONUNIT at address CONADDR, then fills cp->cn_dev, cn_tp and cn_pri (CN_REMOTE if COMCONSOLE, else CN_NORMAL). */
/* 中文：在 cdevsw 中查找 d_open 为 comopen 的主设备号，固定使用 CONUNIT 单元和 CONADDR 地址，然后填充 cp 的 cn_dev、cn_tp 和 cn_pri（定义 COMCONSOLE 则为 CN_REMOTE，否则 CN_NORMAL）。 */
comcnprobe(cp)
	struct consdev *cp;
{
	int unit;

	/* locate the major number */
	for (commajor = 0; commajor < nchrdev; commajor++)
		if (cdevsw[commajor].d_open == comopen)
			break;

	/* XXX: ick */
	unit = CONUNIT;
	com_addr[CONUNIT] = CONADDR;

	/* make sure hardware exists?  XXX */

	/* initialize required fields */
	cp->cn_dev = makedev(commajor, unit);
	cp->cn_tp = &com_tty[unit];
#ifdef	COMCONSOLE
	cp->cn_pri = CN_REMOTE;		/* Force a serial port console */
#else
	cp->cn_pri = CN_NORMAL;
#endif
}

/* EN: Console init hook: derives the unit from cp->cn_dev, programs that port with cominit at comdefaultrate, then sets comconsole to the unit and comconsinit to 1. */
/* 中文：控制台初始化钩子：从 cp->cn_dev 求出单元号，调用 cominit 以 comdefaultrate 速率配置该端口，然后把 comconsole 记为该单元并置 comconsinit 为 1。 */
comcninit(cp)
	struct consdev *cp;
{
	int unit = UNIT(cp->cn_dev);

	cominit(unit, comdefaultrate);
	comconsole = unit;
	comconsinit = 1;
}

/* EN: Under splhigh programs the unit UART: DLAB divisor from ttspeedtab(comdefaultrate) (rate arg ignored), 8 data bits, RX/TX interrupts, FIFO enable+reset, then clears IIR. */
/* 中文：在 splhigh 下初始化单元 UART：置 DLAB 后写入 ttspeedtab(comdefaultrate) 得到的波特率除数（忽略传入的 rate），设 8 位数据、收发中断、使能并复位 FIFO，最后读 IIR 清中断。 */
cominit(unit, rate)
	int unit, rate;
{
	register int com;
	int s;
	short stat;

#ifdef lint
	stat = unit; if (stat) return;
#endif
	com = com_addr[unit];
	s = splhigh();
	outb(com+com_cfcr, CFCR_DLAB);
	rate = ttspeedtab(comdefaultrate, comspeedtab);
	outb(com+com_data, rate & 0xFF);
	outb(com+com_ier, rate >> 8);
	outb(com+com_cfcr, CFCR_8BITS);
	outb(com+com_ier, IER_ERXRDY | IER_ETXRDY);
	outb(com+com_fifo, FIFO_ENABLE|FIFO_RCV_RST|FIFO_XMT_RST|FIFO_TRIGGER_14);
	stat = inb(com+com_iir);
	splx(s);
}

/* EN: Polling console getc: raises splhigh, spins on the LSR until LSR_RXRDY, reads and returns the byte from com_data, clears IIR, then restores the mask. */
/* 中文：轮询式控制台取字符：提升 splhigh，循环读 LSR 等待 LSR_RXRDY，从 com_data 取出并返回该字节，读 IIR 清中断后恢复中断屏蔽。 */
comcngetc(dev)
{
	register com = com_addr[UNIT(dev)];
	short stat;
	int c, s;

#ifdef lint
	stat = dev; if (stat) return(0);
#endif
	s = splhigh();
	while (((stat = inb(com+com_lsr)) & LSR_RXRDY) == 0)
		;
	c = inb(com+com_data);
	stat = inb(com+com_iir);
	splx(s);
	return(c);
}

/*
 * Console kernel output character routine.
 */
/* EN: Polling console putc: splhigh, lazily calls cominit if comconsinit is 0, waits up to 50000 for TXRDY, writes c, waits up to 1500000 for completion and clears IIR. */
/* 中文：轮询式控制台输出：提升 splhigh，若 comconsinit 为 0 则先调用 cominit，等待 TXRDY 最多 50000 次后写入字符 c，再等待发送完成（上限 1500000）并读 IIR 清中断。 */
comcnputc(dev, c)
	dev_t dev;
	register int c;
{
	register com = com_addr[UNIT(dev)];
	register int timo;
	short stat;
	int s = splhigh();

#ifdef lint
	stat = dev; if (stat) return;
#endif
#ifdef KGDB
	if (dev != kgdb_dev)
#endif
	if (comconsinit == 0) {
		(void) cominit(UNIT(dev), comdefaultrate);
		comconsinit = 1;
	}
	/* wait for any pending transmission to finish */
	timo = 50000;
	while (((stat = inb(com+com_lsr)) & LSR_TXRDY) == 0 && --timo)
		;
	outb(com+com_data, c);
	/* wait for this transmission to complete */
	timo = 1500000;
	while (((stat = inb(com+com_lsr)) & LSR_TXRDY) == 0 && --timo)
		;
	/* clear any interrupts generated by this transmission */
	stat = inb(com+com_iir);
	splx(s);
}
#endif
