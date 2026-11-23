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
 *	@(#)wt.c	8.1 (Berkeley) 6/11/93
 */

/*
 *
 * Copyright (c) 1989 Carnegie-Mellon University.
 * All rights reserved.
 *
 * Authors: Robert Baron
 * 
 * Permission to use, copy, modify and distribute this software and
 * its documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS" 
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND 
 * FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 */

#include "wt.h"
#if NWT > 0
/* 
 * HISTORY
 * $Log:	wt.c,v $
 * Revision 2.2.1.3  90/01/08  13:29:38  rvb
 * 	Add Intel copyright.
 * 	[90/01/08            rvb]
 * 
 * Revision 2.2.1.2  89/12/21  18:00:09  rvb
 * 	Change WTPRI to make the streamer tape read/write
 * 	interruptible. 		[lin]
 * 
 * Revision 2.2.1.1  89/11/10  09:49:49  rvb
 * 	ORC likes their streamer port at 0x288.
 * 	[89/11/08            rvb]
 * 
 * Revision 2.2  89/09/25  12:33:02  rvb
 * 	Driver was provided by Intel 9/18/89.
 * 	[89/09/23            rvb]
 * 
 */

/*
 *
 *  Copyright 1988, 1989 by Intel Corporation
 *
 *	Support Bell Tech QIC-02 and WANGTEK QIC-36 or QIC-02
 */

#include <sys/param.h>
#include <sys/buf.h>
#include <sys/file.h>
#include <sys/kprocess.h>
#include <sys/user.h>

#include <i386/isa/wtreg.h>

#ifdef	ORC
unsigned wtport = 0x288;	/* base I/O port of controller	*/
#else	ORC
unsigned wtport = 0x300;	/* base I/O port of controller	*/
#endif	ORC
				/* standard = 0x300		*/
				/* alternate = 0x338		*/

unsigned wtchan = 1;		/* DMA channel number		*/
				/* stardard = 1			*/
				/* hardware permits 1, 2 or 3.	*/
		                /* (Avoid DMA 2: used by disks) */

int	first_wtopen_ever = 1;


#define	ERROR 		1	/* return from tape routines */
#define	SUCCESS		0	/* return from tape routines */

int	wci = 0;
int	exflag = 0;
int	bytes = 0;

static	unsigned char eqdma = 0x8;
static	unsigned char pagereg = 0x83;
static	unsigned char dmareg = 2;
static	unsigned char dma_write = 0x49;
static	unsigned char dma_read = 0x45;
static	unsigned char dma_done = 2;
static	unsigned char mode = 0;
static	unsigned char mbits;	/* map bits into each other */
static	long bufptr;
static	unsigned numbytes;
/*
_wci		dw	0	; interrupt chain finished normally
_exflag		dw	0	; exception variable
_bytes		dw	0	; current bytes

eqdma		db	8h	; enable dma command: ch1,ch2=8h, ch3=10h
pagereg		db	83h	; ch1=83h, ch2=81h, ch3=82h
dmareg		db	2	; ch1=2, ch2=4, ch3=6
dma_write	db	49h	; write dma command: 48h+_wtchan
dma_read	db	45h	; read dma command: 44h+_wtchan
dma_done	db	2	; dma done flag: 1<<_wtchan
mode		db	0	; dma operation mode
lbufptr		dw	0	; buffer pointer to data buffers, low word
hbufptr		dw	0	; buffer pointer to data buffers, high word
numbytes	dw	0	; number of bytes to read or write (new)
*/

#define PAGESIZ		4096
#define HZ		60

/* tape controller ports */
#define STATPORT	wtport
#define CTLPORT		STATPORT
#define CMDPORT		(wtport+1)
#define DATAPORT	CMDPORT

/* defines for reading out status from wangtek tape controller */
#define READY   	0x01    /* ready bit define        */
#define EXCEP		0x02	/* exception bit define    */
#define STAT		(READY|EXCEP)
#define	RESETMASK	0x7
#define	RESETVAL	(RESETMASK & ~EXCEP)

/* tape controller control bits (CTLPORT) */
#define	ONLINE	0x01
#define	RESET	0x02
#define	REQUEST	0x04		/* request command */
#define	CMDOFF	0xC0

/* QIC-02 commands (CMDPORT) */
#define	RDDATA	0x80		/* read data */
#define	READFM	0xA0		/* read file mark */
#define	WRTDATA	0x40		/* write data */
#define	WRITEFM	0x60		/* write file mark */
#define	RDSTAT	0xC0		/* read status command */
#define	REWIND	0x21		/* rewind command (position+bot) */

/* 8237 DMA controller regs */
#define	STATUSREG	0x8
#define MASKREG		0xA
#define MODEREG		0xB
#define CLEARFF		0xC

/* streamer tape block size */
#define BLKSIZE	512

/* Tape characteristics */
#define	NBPS		512	/* 512-byte blocks */
#define	ERROR 		1	/* return from tape routines */
#define	SUCCESS		0	/* return from tape routines */

/* Minor devs */
#define	TP_REWCLOSE(d)	((minor(d)&04) == 0) /* Rewind tape on close if read/write */
#define	TP_DENS(dev)	((minor(dev) >> 3) & 03) /* set density */
#define TPHOG(d)	0	/* use Hogproc during tape I/O	*/

/* defines for wtflags */
#define	TPINUSE	0x0001		/* tape is already open */
#define	TPREAD	0x0002		/* tape is only open for reading */
#define	TPWRITE	0x0004		/* tape is only open for writing */
#define	TPSTART 0x0008		/* tape must be rewound and reset */
#define	TPDEAD	0x0010		/* tape drive does not work or driver error */
#define	TPSESS	0x0020		/* no more reads or writes allowed in session */
				/* for example, when tape has to be changed */
#define	TPSTOP	0x0040		/* Stop command outstanding */
#define	TPREW	0x0080		/* Rewind command outstanding, see wtdsl2() */
#define	TPVOL	0x0100		/* Read file mark, or hit end of tape */
#define	TPWO	0x0200		/* write command outstanding */
#define	TPRO	0x0400		/* read command outstanding */
#define TPWANY	0x0800		/* write command requested */
#define TPRANY	0x1000		/* read command requested */
#define	TPWP	0x2000		/* write protect error seen */

unsigned int	wtflags = TPSTART;	/* state of tape drive */

struct	buf	rwtbuf;		/* header for raw i/o */
struct  process	*myproc;	/* process which opened tape driver */

char wtimeron;			/* wtimer() active flag */
char wtio;			/* dma (i/o) active flag */
char isrlock;			/* isr() flag */

struct process * Hogproc;	/* no Hogproc on Microport */
#define	ftoseg(x)	((unsigned) (x >> 16))

struct	wtstatus {
	ushort	wt_err;		/* code for error encountered */
	ushort	wt_ercnt;	/* number of error blocks */
	ushort	wt_urcnt;	/* number of underruns */
}	wterror;

/* defines for wtstatus.wt_err */
#define	TP_POR		0x100	/* Power on/reset occurred */
#define	TP_RES1		0x200	/* Reserved for end of media */
#define	TP_RES2		0x400	/* Reserved for bus parity */
#define	TP_BOM		0x800	/* Beginning of media */
#define	TP_MBD		0x1000	/* Marginal block detected */
#define	TP_NDT		0x2000	/* No data detected */
#define	TP_ILL		0x4000	/* Illegal command */
#define	TP_ST1		0x8000	/* Status byte 1 bits */
#define	TP_FIL		0x01	/* File mark detected */
#define	TP_BNL		0x02	/* Bad block not located */
#define	TP_UDA		0x04	/* Unrecoverable data error */
#define	TP_EOM		0x08	/* End of media */
#define	TP_WRP		0x10	/* Write protected cartridge */
#define	TP_USL		0x20	/* Unselected drive */
#define	TP_CNI		0x40	/* Cartridge not in place */
#define	TP_ST0		0x80	/* Status byte 0 bits */

/* Grounds for reporting I/O error to user */
#define	TP_ERR0		(TP_BNL|TP_UDA|TP_WRP|TP_CNI|TP_FIL|TP_EOM|TP_USL)
#define	TP_ERR1		(TP_MBD|TP_NDT|TP_ILL)
/* TP_ILL should never happen! */
/*
#define	TP_ERR0		0x7f
#define	TP_ERR1		0x7700
*/

/* defines for reading out status from wangtek tape controller */
#define READY   	0x01    /* ready bit define        */
#define EXCEP		0x02	/* exception bit define    */

/* sleep priority */
#define WTPRI	(PZERO+10)

char	pagebuf[NBPS];		/* buffer of size NBPS */
unsigned long	pageaddr;	/* physical addr of pagebuf */
				/* pageaddr is used with DMA controller */
time_t Hogtime;			/* lbolt when Hog timer started */
extern time_t	lbolt;

#define	debug	printf

/*
 * Strategy routine.
 *
 * Arguments:
 *  Pointer to buffer structure
 * Function:
 *  Start transfer.
 *
 * It would be nice to have this multiple-threaded.
 * There is a version of dump from Berkeley that works with multiple processes
 * trading off with disk & tape I/O.
 */

/* EN: Streamer strategy: splits the buffer at 64K DMA segment boundaries, starts or reuses read/write commands, polls both halves, then sets b_resid/b_error and biodone(bp). */
/* 中文：流式磁带策略例程：按 64KB DMA 段边界把缓冲区切成两段，启动或复用读/写命令并轮询两段传输，最后设置 b_resid/b_error 并调用 biodone(bp) 结束请求。 */
int
wtstrategy(bp)
register struct buf *bp;
{
	unsigned ucnt1, ucnt2, finished;
	unsigned long adr1, adr2;
	int	bad;

	adr1 = kvtop(bp->b_un.b_addr);
#ifdef DEBUG
	debug("bpaddr %x\n", adr1);
#endif
	ucnt1 = bp->b_bcount;
	ucnt2 = 0;
	adr2 = 0;
#ifdef DEBUG
	debug("WTstart: adr1 %lx cnt %x\n", adr1, ucnt1);
#endif
	if (ftoseg(adr1) != ftoseg(adr1 + (unsigned) ucnt1 - 1))
	{
		adr2 = (adr1 & 0xffff0000L) + 0x10000L;
		ucnt2 = (adr1 + ucnt1) - adr2;
		ucnt1 -= ucnt2;
	}
	/* at file marks and end of tape, we just return '0 bytes available' */
	if (wtflags & TPVOL) {
		bp->b_resid = bp->b_bcount;
		goto xit;
	}
	if ((Hogproc == (struct process *) 0) && TPHOG(bp->b_dev))
	{
#ifdef DEBUG
		printf("setting Hogproc\n");
#endif
		Hogtime = 0;
		Hogproc = myproc;
	}
	if (bp->b_flags & B_READ) {
		bad = 0;

		/* For now, we assume that all data will be copied out */
		/* If read command outstanding, just skip down */
		if (!(wtflags & TPRO)) {
			if (ERROR == wtsense(TP_WRP))	/* clear status */
				goto errxit;
#ifdef DEBUG
			debug("WTread: Start read\n");
#endif
			if (!(wtflags & TPREAD) || (wtflags & TPWANY) ||
			    (rstart() == ERROR))  {
#ifdef DEBUG
				debug("Tpstart: read init error\n"); /* */
#endif
				goto errxit;
			}
			wtflags |= TPRO|TPRANY;
		}

		finished = 0;
		/* Take a deep breath */
		if (ucnt1) {
			if ((rtape(adr1, ucnt1) == ERROR) &&
					(wtsense(TP_WRP) == ERROR))
				goto endio;
			/* wait for it */
			bad = pollrdy();
			finished = bytes;
			if (bad)
				goto endio;
		}
		/* if a second I/O region, start it */
		if (ucnt2) {
			if ((rtape(adr2, ucnt2) == ERROR) &&
					(wtsense(TP_WRP) == ERROR))
				ucnt2 = 0;	/* don't poll for me */
			}

		/* if second i/o pending wait for it */
		if (ucnt2) {
			pollrdy();
			/* whether pollrdy is ok or not */
			finished += bytes;
		}
	} else {
		if (wtflags & TPWP)	/* write protected */
			goto errxit;

		/* If write command outstanding, just skip down */
		if (!(wtflags & TPWO)) {
			if (ERROR == wtsense(0))	/* clear status */
			{
#ifdef DEBUG
				debug("TPstart: sense 0\n");
#endif
				goto errxit;
			}
			if (!(wtflags & TPWRITE) || (wtflags & TPRANY) ||
			    (wstart() == ERROR))  {
#ifdef DEBUG
				debug("Tpstart: write init error\n"); /* */
#endif
				wtsense(0);

errxit:				bp->b_flags |= B_ERROR;
				bp->b_resid = bp->b_bcount;
				goto xit;
			}
			wtflags |= TPWO|TPWANY;
		} 

		/* and hold your nose */
		if (ucnt1 && ((wtape(adr1, ucnt1) == ERROR)
				&& (wtsense(0) == ERROR)))
			finished = bytes;

		else if (ucnt2 &&
			(((ucnt1 && pollrdy()) ||
				(wtape(adr2, ucnt2) == ERROR)) &&
				(wtsense(0) == ERROR)))
			finished = ucnt1 + NBPS + bytes;
		/* All writes and/or copyins were fine! */
		else
			finished = bp->b_bcount;
		bad = pollrdy();
	}

	endio:
	if(bad == EIO) bad = 0;
	wterror.wt_err = 0;
	if (exflag && wtsense((bp->b_flags & B_READ) ? TP_WRP : 0)) {
		if ((wterror.wt_err & TP_ST0) 
			&& (wterror.wt_err & (TP_FIL|TP_EOM))) {
#ifdef DEBUG
			debug("WTsta: Hit end of tape\n"); /* */
#endif
			wtflags |= TPVOL;
			if (wterror.wt_err & TP_FIL) {
				if (wtflags & TPRO)
					/* interrupter is bogus */
					rstart();  /* restart read command */
				else
					wtflags &= ~TPWO;
				finished += NBPS; 
			}
		/* Reading file marks or writing end of tape return 0 bytes */
		} else	{
			bp->b_flags |= B_ERROR;
			wtflags &= ~(TPWO|TPRO);
		}
	}

	if(bad) {
		bp->b_flags |= B_ERROR;
		bp->b_error = bad;
	}
	bp->b_resid = bp->b_bcount - finished;
xit:
	biodone(bp);
	if (wtimeron)
		Hogtime = lbolt;
	else if (Hogproc == myproc)
		Hogproc = (struct process *) 0;
}

/*
 * simulate an interrupt periodically while I/O is going
 * this is necessary in case interrupts get eaten due to
 * multiple devices on a single IRQ line
 */
/* EN: Timeout handler: calls isr() to fake an interrupt when DMA is active and isrlock is clear, drops Hogproc after a second without I/O, and re-arms every HZ or clears wtimeron. */
/* 中文：超时处理程序：DMA 进行中且不在 isr 内时调用 isr() 模拟一次中断；Hogproc 超过 1 秒没有 I/O 就释放；随后每 HZ 重新挂接自身，否则清除 wtimeron。 */
wtimer()
{
	/* If I/O going and not in isr(), simulate interrupt
	 * If no I/O for at least 1 second, stop being a Hog
	 * If I/O done and not a Hog, turn off wtimer()
	 */
	if (wtio && !isrlock)
		isr();

	if ((Hogproc == myproc) && Hogtime && (lbolt-Hogtime > HZ))
		Hogproc = (struct process *) 0;

	if (wtio || (Hogproc == myproc))
		timeout(wtimer, (caddr_t) 0, HZ);
	else
		wtimeron = 0;
}


/* EN: Raw I/O wrapper: hands the buffer to wtstrategy() and blocks in biowait() until biodone(), always returning 0. */
/* 中文：原始 I/O 包装：把缓冲区交给 wtstrategy() 后阻塞在 biowait() 等待 biodone()，始终返回 0。 */
wtrawio(bp)
struct buf	*bp;
{
	wtstrategy(bp);
	biowait(bp);
	return(0);
}

/* EN: Physio minphys hook that clamps b_bcount down to PAGESIZ (4096) so each raw DMA transfer fits within one page. */
/* 中文：physio 的 minphys 钩子：把 b_bcount 限制到 PAGESIZ(4096) 以内，保证每次原始 DMA 传输不超过一页。 */
wt_minphys(bp)
struct buf	*bp;
{
	if (bp->b_bcount > PAGESIZ)
		bp->b_bcount = PAGESIZ;
}

/*
 * raw read routine
 */
/* EN: Raw read entry: returns EIO when TPSESS is set, otherwise runs physio() over wtrawio with B_READ and wt_minphys and returns 0. */
/* 中文：原始读入口：wtflags 置位 TPSESS 时返回 EIO，否则以 B_READ 和 wt_minphys 通过 physio() 调用 wtrawio 完成传输，返回 0。 */
wtread(dev, uio)
struct uio	*uio;
{
	if (wtflags & TPSESS) {
		return(EIO);
	}
	physio(wtrawio, &rwtbuf, dev, B_READ, wt_minphys, uio);
	return(0);
}

/*
 * raw write routine
 */
/* EN: Raw write entry: returns EIO when TPSESS is set, otherwise runs physio() over wtrawio with B_WRITE and wt_minphys and returns 0. */
/* 中文：原始写入口：wtflags 置位 TPSESS 时返回 EIO，否则以 B_WRITE 和 wt_minphys 通过 physio() 调用 wtrawio 完成传输，返回 0。 */
wtwrite(dev, uio)
struct uio	*uio;
{
	if (wtflags & TPSESS) {
		return(EIO);
	}
	physio(wtrawio, &rwtbuf, dev, B_WRITE, wt_minphys, uio);
	return(0);
}



/*
 * ioctl routine
 *  for user level QIC commands only
 */
/* EN: Accepts only WTQICMD: issues qicmd(arg), waits with rdyexc(HZ), senses on failure and returns EIO; any other command returns EINVAL. */
/* 中文：只接受 WTQICMD：先执行 qicmd(arg)，再用 rdyexc(HZ) 等待，失败则调用 wtsense(0) 并返回 EIO；其他命令一律返回 EINVAL。 */
wtioctl(dev, cmd, arg, mode)
int dev, cmd;
unsigned long arg;
int mode;
{
	if (cmd == WTQICMD)
	{
		if ((qicmd((int)arg) == ERROR) || (rdyexc(HZ) == ERROR))
		{
			wtsense(0);
			return(EIO);
		}
		return(0);
	}
	return(EINVAL);
}

/*
 * open routine
 * called on every device open
 */
/* EN: Open: inits driver on first use; ENXIO if no pageaddr or TPINUSE, EIO if TPDEAD; waits out a rewind, resets and senses, sets TPINUSE, r/w flags and the density command. */
/* 中文：打开例程：首次打开时初始化驱动；pageaddr 为空或已 TPINUSE 返回 ENXIO，TPDEAD 返回 EIO；等回卷结束后复位并取状态，设置 TPINUSE、读写标志和密度命令。 */
wtopen(dev, flag)
int	dev, flag;
{
	if (first_wtopen_ever) {
		wtinit();
		first_wtopen_ever = 0;
	}
#ifdef DEBUG
	printf("wtopen ...\n");
#endif
	if (!pageaddr) {
		return(ENXIO);
	}
	if (wtflags & (TPINUSE)) {
		return(ENXIO);
	}
	if (wtflags & (TPDEAD)) {
		return(EIO);
	}
	/* If a rewind from the last session is going on, wait */
	while(wtflags & TPREW) {
#ifdef DEBUG
		debug("Waiting for rew to finish\n");
#endif
		delay(1000000);	/* delay one second */
	}
	/* Only do reset and select when tape light is off, and tape is rewound.
	 * This allows multiple volumes. */
	if (wtflags & TPSTART) { 
		if (t_reset() != SUCCESS) {
			return(ENXIO);
		}
#ifdef DEBUG
		debug("reset done. calling wtsense\n");
#endif
		if (wtsense(TP_WRP) == ERROR) {
			return (EIO);
		}
#ifdef DEBUG
		debug("wtsense done\n");
#endif
		wtflags &= ~TPSTART;	
	}

	wtflags = TPINUSE;
	if (flag & FREAD)
		wtflags |= TPREAD;
	if (flag & FWRITE)
		wtflags |= TPWRITE;
	rwtbuf.b_flags = 0;
	myproc = curproc;		/* for comparison */
	switch(TP_DENS(dev)) {
case 0:
cmds(0x28);
break;
case 1:
cmds(0x29);
break;
case 2:
cmds(0x27);
break;
case 3:
cmds(0x24);
	}
	return(0);
}

/*
 * close routine
 * called on last device close
 * If not rewind-on-close, leave read or write command intact.
 */
/* EN: Close: clears Hogproc, optionally writes a file mark, then either rewinds and deselects via wtdsl2() or skips past the next file mark, keeping only rewind/dead/start/rw flags. */
/* 中文：关闭例程：清除 Hogproc，必要时写文件标记并等待，然后按需回卷并注销驱动器（由 wtdsl2 超时完成）或前跳越过下一个文件标记，最后只保留回卷/失效/启动/读写状态位。 */
wtclose(dev)
{
	int wtdsl2();

#ifdef DEBUG
	debug("WTclose:\n");
#endif
	if (Hogproc == myproc)
		Hogproc = (struct process *) 0;
	if (!exflag && (wtflags & TPWANY) && !(wtflags & (TPSESS|TPDEAD))) {
		if (!(wtflags & TPWO))
			wstart();
#ifdef DEBUG
		debug("WT: Writing file mark\n");
#endif
		wmark();	/* write file mark */
#ifdef DEBUG
		debug("WT: Wrote file mark, going to wait\n");
#endif
		if (rdyexc(HZ/10) == ERROR) {
			wtsense(0);
			}
		}
	if (TP_REWCLOSE(dev) || (wtflags & (TPSESS|TPDEAD))) {
	/* rewind tape to beginning of tape, deselect tape, and make a note */
	/* don't wait until rewind, though */
		/* Ending read or write causes rewind to happen, if no error,
		 * and READY and EXCEPTION stay up until it finishes */
		if (wtflags & (TPRO|TPWO))
		{
#ifdef DEBUG
			debug("End read or write\n");
#endif
			rdyexc(HZ/10);
			ioend();
			wtflags &= ~(TPRO|TPWO);
		}
		else	wtwind();
		wtflags |= TPSTART | TPREW;
		timeout(wtdsl2, 0, HZ);
	}
	else if (!(wtflags & (TPVOL|TPWANY)))
	{
		/* space forward to after next file mark no writing done */
		/* This allows skipping data without reading it.*/
#ifdef DEBUG
		debug("Reading past file mark\n");
#endif
		if (!(wtflags & TPRO))
			rstart();
		rmark();
		if (rdyexc(HZ/10))
		{
			wtsense(TP_WRP);
		}
	}
	wtflags &= TPREW|TPDEAD|TPSTART|TPRO|TPWO;
	return(0);
}

/* return ERROR if user I/O request should receive an I/O error code */

/* EN: Clears TPRO/TPWO, reads the 6-byte status via rdstatus(), masks the ignor bits, reports through reperr(), and returns ERROR on TP_ERR0/TP_ERR1 hits else SUCCESS. */
/* 中文：清除 TPRO/TPWO，用 rdstatus() 读 6 字节状态块，屏蔽 ignor 指定的错误位并交 reperr() 打印，命中 TP_ERR0/TP_ERR1 时返回 ERROR，否则返回 SUCCESS。 */
wtsense(ignor)
{
	wtflags &= ~(TPRO|TPWO);
#ifdef DEBUGx
	debug("WTsense: start ");
#endif
	if (rdstatus(&wterror) == ERROR)
	{
#ifdef DEBUG
		debug("WTsense: Can't read status\n");
#endif
		return(ERROR);
	}
#ifdef DEBUG
	if (wterror.wt_err & (TP_ST0|TP_ST1))
	{
		debug("Tperror: status %x error %d underruns %d\n",
			wterror.wt_err, wterror.wt_ercnt, wterror.wt_urcnt);
	}
	else
		debug("done. no error\n");
#endif
	wterror.wt_err &= ~ignor;	/* ignore certain errors */
	reperr(wterror.wt_err);
	if (((wterror.wt_err & TP_ST0) && (wterror.wt_err & TP_ERR0)) ||
		    ((wterror.wt_err & TP_ST1) && (wterror.wt_err & TP_ERR1)))
			return	ERROR;

	return SUCCESS;
}

/* lifted from tdriver.c from Wangtek */
/* EN: Decodes the status error bits and prints the first matching message via sterr() (not online, no cartridge, write protect, bad block), setting TPWP; returns nothing. */
/* 中文：解码状态字里的错误位，按优先级用 sterr() 打印第一条匹配信息（未联机、无磁带、写保护、坏块等），遇到写保护时置 TPWP，无返回值。 */
reperr(srb0)
int srb0;
{
	int s0 = srb0 & (TP_ERR0|TP_ERR1);	/* find out which exception to report */
 
	if (s0) {
		if (s0 & TP_USL) 
			sterr("Drive not online");
		else if (s0 & TP_CNI) 
			sterr("No cartridge");
		else if ((s0 & TP_WRP) && !(wtflags & TPWP))
		{
			sterr("Tape is write protected");
			wtflags |= TPWP;
		}
		/*
		if (s0 & TP_FIL)
			sterr("Filemark detected");
		*/
		else if (s0 & TP_BNL)
			sterr("Block in error not located");
		else if (s0 & TP_UDA)
			sterr("Unrecoverable data error");
		/*
		else if (s0 & TP_EOM)
			sterr("End of tape");
		*/
		else if (s0 & TP_NDT)
			sterr("No data detected");
		/*
		if (s0 & TP_POR)
			sterr("Reset occured");
		*/
		else if (s0 & TP_BOM)
			sterr("Beginning of tape");
		else if (s0 & TP_ILL)
			sterr("Illegal command");
	}
}
	
/* EN: Diagnostic helper that prints the string to the console with a Streamer: prefix; changes no state and returns nothing. */
/* 中文：诊断辅助函数：以 Streamer: 为前缀把给定字符串打印到控制台，不修改任何全局状态，无返回值。 */
sterr(errstr)
char	*errstr;
{
	printf("Streamer: %s\n", errstr);
}

/* Wait until rewind finishes, and deselect drive */
/* 中文：等待磁带倒带完成并取消选中驱动器；轮询状态寄存器，超时则报错。 */
/* EN: Wait for the tape rewind to finish and deselect the drive; poll the status register and report errors on timeout. */
wtdsl2() {
	int	stat;

	stat = inb(wtport) & (READY|EXCEP);
#ifdef DEBUG
	debug("Timeout: Waiting for rewind to finish: stat %x\n", stat);
#endif
	switch (stat) {
		/* They're active low, ya'know */
		case READY|EXCEP:
			timeout(wtdsl2, (caddr_t) 0, HZ);
			return;
		case EXCEP: 
			wtflags &= ~TPREW;
			return;
		case READY:
		case	0:
			wtflags &= ~TPREW;
			sterr("Rewind failed");
			wtsense(TP_WRP);
			return;
			}
	}

/* 中文：启动磁带倒带操作。 */
/* EN: Start rewinding the tape. */
wtwind() {
#ifdef DEBUG
	debug("WT: About to rewind\n");
#endif
	rwind();	/* actually start rewind */
}

/* 中文：磁带驱动中断处理例程：设置中断服务锁并调用 isr() 处理完成的传输。 */
/* EN: Tape driver interrupt handler: set the ISR lock and call isr() to service completed transfers. */
wtintr(unit) {
	if (wtflags & (TPWO|TPRO))
	{
		isrlock = 1;
		if (wtio) isr();
		isrlock = 0;
	}
}

/* 中文：初始化磁带驱动：校验 DMA 通道合法性并初始化汇编层变量。 */
/* EN: Initialize the tape driver: validate the DMA channel and initialize assembly-language variables. */
wtinit() {
	if (wtchan < 1 || wtchan > 3)
	{
		sterr("Bad DMA channel, cannot init driver");
		return;
	}
	wtlinit();	/* init assembly language variables */
	pageset();
}

/* EN: Polls the controller status register, delaying ticks/HZ seconds at splbio between polls without decrementing a budget, and returns SUCCESS if EXCEP is set on exit, else ERROR. */
/* 中文：轮询控制器状态寄存器，每轮在 splbio 保护下延时 ticks/HZ 秒且不递减超时预算，退出时若 EXCEP 置位返回 SUCCESS，否则返回 ERROR。 */
rdyexc(ticks)
{
	int s;
#ifdef DEBUG
	int os = 0xffff;		/* force printout first time */
#endif
	for (;;) {			/* loop until ready or exception */
		s=(inb(wtport) & 0xff);	/* read the status register */
#ifdef DEBUG
		if (os != s) {
			debug("Status reg = %x\n", s); /* */
			os = s;
			}
#endif
		if (!(s & EXCEP))	/* check if exception have occured */
			break;
		if (!(s & READY))	/* check if controller is ready */
			break;
		s = splbio();
		delay((ticks/HZ)*1000000); /* */
		splx(s);
	}
#ifdef DEBUG
	debug("Status reg = %x on return\n", s); /* */
#endif
	return((s & EXCEP)?SUCCESS:ERROR);  /* return exception if it occured */
}

/* EN: Sleeps on &wci at WTPRI|PCATCH while wtio is set, returning the tsleep error if interrupted; on normal DMA completion it returns EIO, which callers treat as success. */
/* 中文：在 wtio 置位期间以 WTPRI|PCATCH 睡眠在 &wci 上，被信号打断则返回 tsleep 的错误码；DMA 正常结束时返回 EIO，调用者按成功处理。 */
pollrdy()
{
	int	 sps;
#ifdef DEBUG
	debug("Pollrdy\n");
#endif
	sps = splbio();
	while (wtio) {
		int error;

		if (error = tsleep((caddr_t)&wci, WTPRI | PCATCH,
			"wtpoll", 0)) {
			splx(sps);
			return(error);
		}
	}
	splx(sps);
#ifdef DEBUG
	debug("Finish poll, wci %d exflag %d\n", wci, exflag);
#endif
	return (EIO);
}

/* EN: Marks DMA as active by setting wtio and arms the wtimer() timeout at HZ/2 if it is not already running; called from dma() in wtlib1.s. */
/* 中文：把 wtio 置 1 表示 DMA 已开始，若 wtimer 尚未运行则按 HZ/2 挂接 wtimer() 超时；由 wtlib1.s 中的 dma() 调用。 */
wtdma()		/* start up i/o operation, called from dma() in wtlib1.s */
{
	wtio = 1;
	if (!wtimeron)
	{
		wtimeron = 1;
		timeout(wtimer, (caddr_t) 0, HZ/2);
	}
}

/* EN: Ends the DMA operation: clears wtio and wakes all sleepers on &wci; called from isr() in wtlib1.s. */
/* 中文：结束 DMA 操作：清除 wtio 并唤醒所有睡眠在 &wci 上的进程；由 wtlib1.s 中的 isr() 调用。 */
wtwake()	/* end i/o operation, called from isr() in wtlib1.s */
{
	wtio = 0;
	wakeup(&wci);
}

/* EN: Converts the virtual address of pagebuf to a physical address via kvtop() and stores it in pageaddr for the 8237 DMA controller; nothing else is touched. */
/* 中文：用 kvtop() 把 pagebuf 的虚拟地址换算成物理地址存入 pageaddr 供 8237 DMA 控制器使用，不改动其他状态。 */
pageset()
{
	unsigned long pp;

	pp = (unsigned long) pagebuf;
	pageaddr = kvtop(pp);
#ifdef DEBUG
	debug("pageset: addr %lx\n", pageaddr);
#endif
}



#define near

/* EN: Controller handshake: asserts REQUEST with the global mbits byte, waits for READY to clear, clears REQUEST, then waits for READY to come back; no return value. */
/* 中文：控制器握手：带上全局 mbits 字节发出 REQUEST，等 READY 变低后撤销 REQUEST，再等 READY 恢复；无返回值。 */
static near
sendcmd()
{
	/* desired command in global mbits */

	outb(CTLPORT, mbits | REQUEST);		/* set request */
	while (inb(STATPORT) & READY);		/* wait for ready */
	outb(CTLPORT, mbits & ~REQUEST);	/* reset request */
	while ((inb(STATPORT) & READY) == 0);	/* wait for not ready */
}

static near		/* execute command */
/* EN: Waits for the controller to go idle, returns ERROR if EXCEP is clear, else writes cmd to CMDPORT, sets mbits=ONLINE and pulses it via sendcmd(); returns SUCCESS. */
/* 中文：等待控制器空闲，EXCEP 位为 0 时返回 ERROR，否则把 cmd 写入 CMDPORT、置 mbits=ONLINE 并经 sendcmd() 发出，返回 SUCCESS。 */
cmds(cmd)
{
	register s;

	do s = inb(STATPORT);
	while ((s & STAT) == STAT);	/* wait for ready */

	if ((s & EXCEP) == 0)		/* if exception */
		return ERROR;		/* error */
	
	outb(CMDPORT, cmd);		/* output the command	*/

	outb(CTLPORT, mbits=ONLINE);	/* set & send ONLINE	*/
	sendcmd();

	return SUCCESS;
}

/* EN: Thin wrapper that just returns cmds(cmd), issuing the caller's raw QIC-02 command byte to the tape controller. */
/* 中文：薄封装，直接返回 cmds(cmd)，把调用者给的原始 QIC-02 命令字节发给磁带控制器。 */
qicmd(cmd)
{
	return cmds(cmd);
}

/* EN: Thin wrapper that returns cmds(RDDATA), starting a read-data command on the tape controller. */
/* 中文：薄封装，返回 cmds(RDDATA)，向磁带控制器发出读数据命令。 */
rstart()
{
	return cmds(RDDATA);
}

/* EN: Thin wrapper returning cmds(READFM), issuing a read-file-mark command to advance to the next file mark. */
/* 中文：薄封装，返回 cmds(READFM)，发出读文件标记命令以定位到下一个文件标记。 */
rmark()
{
	return cmds(READFM);
}

/* EN: Thin wrapper returning cmds(WRTDATA), starting a write-data command on the tape controller. */
/* 中文：薄封装，返回 cmds(WRTDATA)，向磁带控制器发出写数据命令。 */
wstart()
{
	return cmds(WRTDATA);
}

/* EN: Waits for idle, records ERROR if EXCEP is clear, then clears ONLINE in mbits, masks the DMA channel at MASKREG and resets the flip-flop; returns the recorded status. */
/* 中文：等待控制器空闲，EXCEP 为 0 时记下 ERROR，然后清除 mbits 的 ONLINE 位、在 MASKREG 屏蔽 DMA 通道并复位方向触发器，返回记录的状态。 */
ioend()
{
	register s;
	register rval = SUCCESS;

	do s = inb(STATPORT);
	while ((s & STAT) == STAT);	/* wait for ready */

	if ((s & EXCEP) == 0)		/* if exception */
		rval = ERROR;		/* error */
	
	mbits &= ~ONLINE;
	outb(CTLPORT, mbits);		/* reset ONLINE */
	outb(MASKREG, wtchan+4);	/* turn off dma */
	outb(CLEARFF, 0);		/* reset direction flag */

	return rval;
}

/* EN: Writes a file mark: returns ERROR if cmds(WRITEFM) fails, waits for idle, then returns ERROR when EXCEP is clear and SUCCESS otherwise. */
/* 中文：写文件标记：cmds(WRITEFM) 失败即返回 ERROR，否则等控制器空闲，EXCEP 为 0 再返回 ERROR，成功返回 SUCCESS。 */
wmark()
{
	register s;

	if (cmds(WRITEFM) == ERROR)
		return ERROR;

	do s = inb(STATPORT);
	while ((s & STAT) == STAT);	/* wait for ready */

	if ((s & EXCEP) == 0)		/* if exception */
		return ERROR;		/* error */

	return SUCCESS;
}

/* EN: Starts a rewind: sets mbits=CMDOFF, waits for idle, writes REWIND to CMDPORT and pulses sendcmd(); always returns SUCCESS without checking status. */
/* 中文：启动回卷：置 mbits=CMDOFF，等控制器空闲后把 REWIND 写入 CMDPORT 并调用 sendcmd()，不检查状态，恒返回 SUCCESS。 */
rwind()
{
	register s;

	mbits = CMDOFF;

	do s = inb(STATPORT);
	while ((s & STAT) == STAT);	/* wait for ready */

	outb(CMDPORT, REWIND);
	sendcmd();

	return SUCCESS;
}

/* EN: Sends RDSTAT and reads the 6-byte status block into stp, handshaking each byte with REQUEST and a settling delay; returns ERROR if EXCEP drops mid-read, else SUCCESS. */
/* 中文：发送 RDSTAT 命令，逐字节用 REQUEST 握手并加短延时，把 6 字节状态块读入 stp；读取途中 EXCEP 为 0 返回 ERROR，读完返回 SUCCESS。 */
rdstatus(stp)
char *stp;		/* pointer to 6 byte buffer */
{
	register s;
	int n;

	do s = inb(STATPORT);
	while ((s & STAT) == STAT);	/* wait for ready or exception */

	outb(CMDPORT, RDSTAT);
	sendcmd();			/* send read status command */

	for (n=0; n<6; n++)
	{
#ifdef DEBUGx
		debug("rdstatus: waiting, byte %d\n", n);
#endif
		do s = inb(STATPORT);
		while ((s & STAT) == STAT);	/* wait for ready */
#ifdef DEBUGx
		debug("rdstatus: done\n");
#endif
		if ((s & EXCEP) == 0)		/* if exception */
			return ERROR;		/* error */

		*stp++ = inb(DATAPORT);		/* read status byte */

		outb(CTLPORT, mbits | REQUEST);	/* set request */
#ifdef DEBUGx
		debug("rdstatus: waiting after request, byte %d\n", n);
#endif
		while ((inb(STATPORT)&READY) == 0);	/* wait for not ready */
		for (s=100; s>0; s--);		/* wait an additional time */

		outb(CTLPORT, mbits & ~REQUEST);/* unset request */
#ifdef DEBUGx
		debug("rdstatus: done\n");
#endif
	}
	return SUCCESS;
}

/* EN: Pulses the controller RESET bit with delay(20) between assert and clear, then returns SUCCESS only if the status register low bits read RESETVAL, else ERROR. */
/* 中文：脉冲式复位控制器：置位 RESET 后 delay(20) 再清除，仅当状态寄存器低位等于 RESETVAL 时返回 SUCCESS，否则返回 ERROR。 */
t_reset()
{
	register i;
	mbits |= RESET;
	outb(CTLPORT, mbits);		/* send reset */
	delay(20);
	mbits &= ~RESET;
	outb(CTLPORT, mbits);		/* turn off reset */
	if ((inb(STATPORT) & RESETMASK) == RESETVAL)
		return SUCCESS;
	return ERROR;
}

/* EN: Programs the 8237 under splbio: clear flip-flop, set mode, load bufptr address/page and BLKSIZE count, assert ONLINE, then unmask wtchan to start the transfer. */
/* 中文：在 splbio 保护下编程 8237：清方向触发器、设模式、装入 bufptr 的地址/页号和 BLKSIZE 计数，置 ONLINE 后解除 wtchan 屏蔽以启动 DMA。 */
static
dma()
{
	int x=splbio();
	wtdma();
	outb(CLEARFF, 0);
	outb(MODEREG, mode);	/* set dma mode */
	outb(dmareg, bufptr & 0xFF);
	outb(dmareg, (bufptr>>8) & 0xFF);
	outb(pagereg, (bufptr>>16) & 0xFF);                         
	outb(dmareg+1, (BLKSIZE-1) & 0xFF);
	outb(dmareg+1, (BLKSIZE-1) >> 8);
	outb(wtport, eqdma+ONLINE);
	outb(MASKREG, wtchan);	/* enable command to 8237, start dma */
	splx(x);
}

/* EN: Sets bufptr/numbytes and clears wci, exflag and bytes, waits for the controller to leave the busy state, and if EXCEP is set calls dma() and returns SUCCESS, else ERROR. */
/* 中文：保存 bufptr/numbytes 并清零 wci、exflag、bytes，等控制器脱离忙状态；EXCEP 置位则调用 dma() 启动传输并返回 SUCCESS，否则返回 ERROR。 */
static near
wtstart(buf, cnt)
long buf;
int cnt;
{
	register s;

	bufptr = buf;		/* init statics */
	numbytes = cnt;
	wci = 0;		/* init flags */
	exflag = 0;
	bytes = 0;		/* init counter */

	do s = inb(STATPORT) & STAT;
	while (s == STAT);	/* wait for ready or error */

	if (s & EXCEP)		/* no error */
	{
		dma();
		return SUCCESS;
	}
	return ERROR;		/* error */
}

/* EN: Selects dma_read mode and passes the physical buffer address and byte count to wtstart() to program one 8237 DMA block; returns SUCCESS(0) or ERROR(1). */
/* 中文：将静态变量 mode 设为 dma_read，再把物理缓冲区地址和字节数交给 wtstart() 启动一次读方向 DMA；成功返回 SUCCESS(0)，失败返回 ERROR(1)。 */
rtape(buf, cnt)
long buf;			/* physical address */
int cnt;			/* number of bytes */
{
	mode = dma_read;
	return wtstart(buf,cnt);
}

/* EN: Selects dma_write mode and passes the physical buffer address and byte count to wtstart() to program one 8237 DMA block; returns SUCCESS(0) or ERROR(1). */
/* 中文：将静态变量 mode 设为 dma_write，再把物理缓冲区地址和字节数交给 wtstart() 启动一次写方向 DMA；成功返回 SUCCESS(0)，失败返回 ERROR(1)。 */
wtape(buf, cnt)
long buf;			/* physical address */
int cnt;			/* number of bytes */
{
	mode = dma_write;
	return wtstart(buf,cnt);
}

/* EN: Tape interrupt handler: adds BLKSIZE to bytes; at numbytes or on an exception it sets wci, masks DMA and wakes sleepers via wtwake(), else advances bufptr and restarts dma(). */
/* 中文：磁带中断处理程序：累加 BLKSIZE 到 bytes；达到 numbytes 或控制器上报异常时置 wci、关闭 DMA 并经 wtwake() 唤醒等待进程，否则 bufptr 前移并调 dma() 启动下一块。 */
isr()
{
	int stat = inb(wtport);
	if (!(stat & EXCEP))	/* exception during I/O */
	{
		if (bytes + BLKSIZE >= numbytes) wci = 1;
		exflag = 1;
		goto isrwake;
	}
	if ((stat & READY) || !(inb(STATUSREG) & dma_done))
		return;
	exflag = 0;
	outb(wtport, ONLINE);
	bytes += BLKSIZE;
	if (bytes >= numbytes)	/* normal completion of I/O */
	{
		wci = 1;
isrwake:
		outb(MASKREG, 4+wtchan);	/* turn off dma */
		wtwake();			/* wake up user level */
	}
	else
	{			/* continue I/O */
		bufptr += BLKSIZE;
		dma();
	}
}

/* EN: Derives the 8237 DMA constants (pagereg, dma_done, eqdma, dma_read, dma_write, dmareg) from wtchan; channel 1 keeps the compile-time defaults and returns at once. */
/* 中文：根据 wtchan 计算 8237 DMA 寄存器常量（pagereg、dma_done、eqdma、dma_read、dma_write、dmareg）；通道 1 直接返回沿用编译时默认值，通道 2、3 才改写。 */
wtlinit()
{
	switch (wtchan) {
	case 1:
		return;
	case 2:
		pagereg = 0x81;
		dma_done = 4;
		break;
	case 3:
		eqdma = 0x10;
		pagereg = 0x82;
		dma_done = 8;
		break;
	}
	dma_write = wtchan+0x48;
	dma_read = wtchan+0x44;
	dmareg = wtchan+wtchan;
}

/*
 * delay i microseconds
 */
/* EN: Busy-waits i milliseconds by calling DELAY(1000) i times; used for controller timing and never sleeps. */
/* 中文：通过循环 i 次调用 DELAY(1000) 实现 i 毫秒的忙等待，用于磁带控制器时序，不会睡眠。 */
delay(i)
register int i;
{
	while (i-- > 0)
		DELAY(1000);
}

/* EN: Empty stub with a completely empty body: it computes nothing, performs no I/O and returns no value; nothing in the tree calls it. */
/* 中文：完全空的桩函数：函数体为空，不计算任何内容、不访问硬件、无返回值，仓库中没有调用点。 */
wtsize()
{
}

/* EN: Empty stub with a completely empty body: it performs no dump, no I/O and returns no value; nothing in the tree calls it. */
/* 中文：完全空的桩函数：函数体为空，不执行内核转储、不访问硬件、无返回值，仓库中没有调用点。 */
wtdump()
{
}

#include <i386/isa/isa_device.h>
#include <i386/isa/icu.h>

int	wtprobe(), wtattach();
struct	isa_driver wtdriver = {
	wtprobe, wtattach, "wt",
};

/* EN: Autoconfig probe: stores dvp->id_iobase in wtport, resets the controller via t_reset(), and returns 1 when the reset succeeded, otherwise 0. */
/* 中文：自动配置探测：把 dvp->id_iobase 记入 wtport，调 t_reset() 复位磁带控制器，复位成功返回 1，否则返回 0。 */
wtprobe(dvp)
	struct isa_device *dvp;
{
	int val,i,s;

#ifdef lint
	wtintr(0);
#endif

	wtport = dvp->id_iobase;
	if(t_reset() != SUCCESS) return(0);
	return(1);
}

wtattach() { }

#endif NWT
