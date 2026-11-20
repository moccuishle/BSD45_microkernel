/*
 * pccon.c - PC console (VGA text + COM1 mirror) for the 4.4BSD-Lite2
 *	     i386 ELF kernel.
 *
 * This is a replacement for the tty-bound pccons.c of 4.4BSD.  It plugs
 * into the genuine i386/i386/cons.c console layer through struct consdev,
 * so cninit()/cnputc()/cngetc() are the unmodified 4.4BSD code.
 *
 * Crtat is also consumed by locore.s before init386() runs; its initial
 * value must be the virtual address of a text buffer (KERNBASE + 0xB8000).
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/tty.h>

#include <i386/i386/cons.h>

#include "arch/i386/io.h"

#define	COM1		0x3f8
#define	COM1_LSR	(COM1 + 5)
#define	COM1_SCR	(COM1 + 7)

#define	VGA_COLOR	((u_short *)0xFE0B8000)
#define	VGA_MONO	((u_short *)0xFE0B0000)

#define	COL		80
#define	ROW		25
#define	ATTR		0x07

u_short *Crtat = VGA_COLOR;

static int	crt_row, crt_col;
static u_short *crt_base;

static int	com_present;	/* COM1 really exists (not an unassigned port) */

/*
 * RAM copy of the console log.  VMware's snapshot (.vmem) does not contain
 * the VGA aperture - 0xA0000-0xBFFFF belongs to the emulated SVGA device, so
 * reading 0xB8000 out of a dump always yields zeros and `vmrun captureScreen`
 * needs VMware Tools.  Keeping the last few KB of console text in ordinary
 * RAM gives a host-side log channel that needs neither: take a snapshot and
 * read this buffer (see tools/vm_screen.py --klog).
 * 控制台日志的 RAM 副本。VMware 的 .vmem 不包含 VGA 显存（0xA0000-0xBFFFF
 * 属于虚拟 SVGA 设备），快照里读 0xB8000 永远是 0，而 captureScreen 又需要
 * VMware Tools。把最近几 KB 的控制台文本留在普通内存里，主机侧拍快照即可读。
 */
#define	KLOG_SIZE	8192
char		klog_buf[KLOG_SIZE];
unsigned	klog_len;

static void
klog_putc(int c)
{

	if (klog_len < KLOG_SIZE - 1)
		klog_buf[klog_len++] = (char)c;
	klog_buf[klog_len] = 0;
}

static void	vga_cursor(void);

/*
 * Probe for a 16550-style UART.  On firmware that does not map COM1 (for
 * example a VMware VM with no serial port configured) reads to 0x3f8..
 * return 0xff, which would otherwise look like "LSR: data ready" and make
 * cngetc() return 0xff forever, so the keyboard was never polled.
 */
/* EN: Probes COM1: rejects 0xff (unmapped port), then uses the 16550A scratch register (0x3f8+7) write/read-back test, falling back to the canonical LSR THRE|TEMT pattern. Returns 1 when a UART answers. */
/* 中文：探测 COM1：读到 0xff（端口未映射）直接判无；再用 16550A 的 scratch 寄存器（0x3f8+7）写读比对，退路是 LSR 的 THRE|TEMT 位型。有 UART 应答返回 1。 */
static int
com_probe(void)
{
	int lsr, scr;

#ifdef VM_FORCE_COM1
	/* VMware verification build: file-backed serial may not present the
	 * canonical 16550 LSR (0x60) at probe time, so trust it unconditionally.
	 * VMware 验证构建：文件型串口探测时可能尚未呈现标准 16550 LSR，强制信任。 */
	return (1);
#else
	if (hal_inb(COM1_LSR) == 0xff)
		return (0);
	/*
	 * The scratch register exists on the 16550A and on every virtual
	 * UART (QEMU's serial, VMware's file/pipe-backed port).  It is the
	 * only probe that works before the port is programmed: VMware's
	 * file-backed UART reports LSR 0x00 until LCR/FCR have been
	 * written, which made the old LSR-only probe declare COM1 absent
	 * and silently drop the whole boot log.
	 * scratch 寄存器是 16550A 与所有虚拟 UART 都实现的，且不依赖端口
	 * 是否已初始化——VMware 文件型串口在 LCR/FCR 写入前 LSR 读回 0x00，
	 * 旧的纯 LSR 探测会误判"没有串口"，整份启动日志被丢弃。
	 */
	hal_outb(COM1_SCR, 0xa5);
	scr = hal_inb(COM1_SCR);
	hal_outb(COM1_SCR, 0x00);
	if (scr == 0xa5)
		return (1);

	/* Fallback for a plain 8250 without SCR. */
	lsr = hal_inb(COM1_LSR);
	if ((lsr & 0xe0) == 0x60)
		return (1);
	return (0);
#endif
}

/* EN: Returns the cached com_present flag that pccninit() stored from com_probe(); it performs no COM1 access itself. */
/* 中文：返回 pccninit() 用 com_probe() 探测后缓存的 com_present 标志，本身不访问 COM1 硬件。 */
int
pccom_present(void)
{

	return (com_present);
}

/* EN: Drains up to 32 stale bytes from the 8042 output buffer (ports 0x64/0x60), waits for the input buffer to clear, then writes 0xae to enable the keyboard. */
/* 中文：先最多读 32 次清掉 8042 输出缓冲区（端口 0x64/0x60）中 BIOS 遗留的数据，等待输入缓冲区变空，再向 0x64 写 0xae 使能键盘。 */
static void
kbd_init(void)
{
	int i;

	/* drain whatever the BIOS left in the 8042 output buffer */
	for (i = 0; i < 32 && (hal_inb(0x64) & 0x01); i++)
		(void)hal_inb(0x60);
	/* wait for the input buffer to drain, then enable the keyboard */
	for (i = 0; i < 100000 && (hal_inb(0x64) & 0x02); i++)
		;
	hal_outb(0x64, 0xae);
}

/* EN: Returns 0 at once if com_present is clear; otherwise polls COM1's LSR up to 100000 times for THRE (0x20), returning 1 when set and 0 on timeout. */
/* 中文：若 com_present 为 0 立即返回 0；否则最多轮询 COM1 的 LSR 100000 次等 THRE(0x20) 置位，置位返回 1，超时返回 0。 */
static int
com_ready(void)
{
	int i;

	if (!com_present)
		return (0);
	for (i = 0; i < 100000; i++)
		if (hal_inb(COM1_LSR) & 0x20)
			return (1);
	return (0);
}

/* EN: Waits for com_ready() and only then writes character c to COM1's data register; the byte is dropped silently if the UART is absent or never becomes ready. */
/* 中文：先调 com_ready() 等待发送就绪，就绪后才把字符 c 写入 COM1 数据寄存器；串口不存在或始终不就绪时该字符被静默丢弃。 */
static void
com_putc(int c)
{
#ifdef VM_FORCE_COM1
	/* VMware verification build: write unconditionally; the file-backed
	 * UART may not assert THRE the way com_ready() expects. */
	hal_outb(COM1, c);
#else
	if (com_ready())
		hal_outb(COM1, c);
#endif
}

/* EN: Programs COM1 for polled output: interrupts off, divisor 1 (115200 baud), 8N1 framing, FIFO enabled (0xc7), and asserts DTR|RTS (0x03). */
/* 中文：把 COM1 配置为轮询输出：关中断，分频值 1（115200 波特），8N1 帧格，使能 FIFO（0xc7），并置位 DTR|RTS（0x03）。 */
static void
com_init(void)
{
	hal_outb(COM1 + 1, 0x00);	/* no interrupts */
	hal_outb(COM1 + 3, 0x80);	/* DLAB */
	hal_outb(COM1 + 0, 0x01);	/* divisor 1 = 115200 */
	hal_outb(COM1 + 1, 0x00);
	hal_outb(COM1 + 3, 0x03);	/* 8N1 */
	hal_outb(COM1 + 2, 0xc7);	/* FIFO on */
	hal_outb(COM1 + 4, 0x03);	/* DTR|RTS */
}

/* EN: Console probe: points crt_base at the locore-relocated Crtat buffer rather than the raw 0xFE0B8000 constant, sets cp->cn_pri = CN_INTERNAL, and returns 0. */
/* 中文：控制台探测：把 crt_base 指向已由 locore 重定位的 Crtat 缓冲区而非裸地址 0xFE0B8000，设 cp->cn_pri = CN_INTERNAL，返回 0。 */
int
pccnprobe(struct consdev *cp)
{

	/*
	 * Crtat has already been relocated by locore.s to the I/O window
	 * (atdevbase + 0x18000), which maps physical 0xB8000.  Do not
	 * overwrite it: with a sub-1MB kernel the raw 0xFE0B8000 address
	 * is not identity mapped.
	 */
	crt_base = Crtat;
	cp->cn_pri = CN_INTERNAL;
	return (0);
}

/* EN: Console init: fills all 80x25 cells of Crtat with ATTR spaces, homes the cursor, probes and initializes COM1 if present, enables the keyboard, and returns 0. */
/* 中文：控制台初始化：把 Crtat 的 80x25 单元格全填为 ATTR 空白，光标归位，若探到 COM1 则初始化之，使能键盘，最后返回 0。 */
int
pccninit(struct consdev *cp)
{
	int i;

	crt_base = Crtat;
	for (i = 0; i < COL * ROW; i++)
		crt_base[i] = (ATTR << 8) | ' ';
	crt_row = crt_col = 0;
	vga_cursor();
	/*
	 * Program the UART first, then probe it.  Writing LCR/FCR makes a
	 * virtual UART report a real LSR; probing before that is exactly the
	 * bug that hid the whole VMware serial log (see com_probe()).
	 * 先初始化再探测：先写 LCR/FCR，虚拟 UART 才会给出真实的 LSR。
	 */
	com_init();
	com_present = com_probe();
	kbd_init();
	return (0);
}

/* EN: Writes c to the COM1 mirror and to VGA text memory; handles CR, LF, backspace and tab, wraps at column 80, scrolls up past row 25, refreshes the cursor, returns 0. */
/* 中文：把字符 c 同时输出到 COM1 和 VGA 文本显存；处理回车、换行、退格与制表符，满 80 列换行，超过第 25 行整屏上滚，刷新硬件光标，返回 0。 */
int
pccnputc(dev_t dev, int c)
{

	com_putc(c);
	klog_putc(c);
	if (c == '\r') {
		crt_col = 0;
		vga_cursor();
		return (0);
	}
	if (c == '\n') {
		crt_col = 0;
		crt_row++;
	} else if (c == '\b') {
		if (crt_col > 0)
			crt_col--;
		else if (crt_row > 0) {
			crt_row--;
			crt_col = COL - 1;
		}
		crt_base[crt_row * COL + crt_col] = (ATTR << 8) | ' ';
	} else if (c == '\t') {
		crt_col = (crt_col + 8) & ~7;
	} else {
		crt_base[crt_row * COL + crt_col] = (ATTR << 8) | (c & 0xff);
		crt_col++;
	}
	if (crt_col >= COL) {
		crt_col = 0;
		crt_row++;
	}
	if (crt_row >= ROW) {
		int i;

		for (i = 0; i < COL * (ROW - 1); i++)
			crt_base[i] = crt_base[i + COL];
		for (i = COL * (ROW - 1); i < COL * ROW; i++)
			crt_base[i] = (ATTR << 8) | ' ';
		crt_row = ROW - 1;
	}
	vga_cursor();
	return (0);
}

/* EN: Programs the VGA CRTC cursor registers 0x0f/0x0e through ports 0x3d4/0x3d5 with crt_row * COL + crt_col, writing the low byte first. */
/* 中文：通过 0x3d4/0x3d5 端口把光标位置 crt_row * COL + crt_col 写入 VGA CRTC 光标寄存器 0x0f/0x0e，先写低字节。 */
static void
vga_cursor(void)
{
	unsigned pos = crt_row * COL + crt_col;

	hal_outb(0x3d4, 0x0f);
	hal_outb(0x3d5, pos & 0xff);
	hal_outb(0x3d4, 0x0e);
	hal_outb(0x3d5, (pos >> 8) & 0xff);
}

/*
 * Scan code set 1 -> ASCII, unshifted / shifted.
 */
static const char scancode_map[128] = {
	0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
	'\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
	0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
	0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
	'*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static const char scancode_shift[128] = {
	0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
	'\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
	0, 'A','S','D','F','G','H','J','K','L',':','"','~',
	0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
	'*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static int shift_down;
static int ext_pending;

/* EN: Polled input: returns a pending COM1 byte, else reads one 8042 scancode (0 if none), tracks Shift/E0 prefix, skips releases, maps set-1 codes to ASCII, 0 for modifier keys. */
/* 中文：轮询式输入：有 COM1 数据就返回该字节，否则读一个 8042 扫描码（无则返 0），跟踪 Shift 与 E0 前缀，忽略松键，用 set-1 码表转 ASCII；修饰键返 0。 */
int
pccngetc(dev_t dev)
{
	unsigned char sc;
	static const char *map;
	int c;

	if (com_present && (hal_inb(COM1_LSR) & 0x01))
		return (hal_inb(COM1));

	if ((hal_inb(0x64) & 0x01) == 0)
		return (0);

	sc = hal_inb(0x60);
	if (sc == 0xe0) {
		ext_pending = 1;
		return (0);
	}
	if (ext_pending) {
		ext_pending = 0;
		return (0);
	}
	if (sc == 0x2a || sc == 0x36) {
		shift_down = 1;
		return (0);
	}
	if (sc == 0xaa || sc == 0xb6) {
		shift_down = 0;
		return (0);
	}
	if (sc & 0x80)
		return (0);
	map = shift_down ? scancode_shift : scancode_map;
	c = (unsigned char)map[sc];
	return (c);
}
