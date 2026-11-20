/*
 * glue.c - minimal 4.4BSD-Lite2 kernel runtime for the i386 ELF port.
 *
 * The machine dependent core (locore.s, pmap.c, machdep.c, cons.c) is the
 * unmodified 4.4BSD-Lite2 source.  This file supplies only what a
 * stand-alone (no userland, no vfs) bring-up kernel needs:
 *	proc0 / curproc / proc0paddr
 *	kprintf / panic
 *	vm_set_page_size
 *	trap()
 *	main()
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/reboot.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>

#include <machine/cpu.h>
#include <machine/psl.h>
#include <machine/pmap.h>
#include <machine/trap.h>

#include "atapi.h"
#include "ata.h"
#include "isofs.h"
#include "ffs/ffs.h"
#include "jlfs/jlfs.h"
#include "vfs.h"
#include "lib/kstr.h"
#include "sys/platform.h"
#include "sys/smp.h"
#include "thread/kthread.h"
#include "vm/zcomp.h"

/* GCC builtins: the mingw <stdarg.h> is only an include_next stub. */
typedef __builtin_va_list	va_list;
#define	va_start(ap, last)	__builtin_va_start(ap, last)
#define	va_arg(ap, type)	__builtin_va_arg(ap, type)
#define	va_end(ap)		__builtin_va_end(ap)

struct proc	proc0;
struct proc	*curproc = &proc0;
struct user	*proc0paddr;
struct vmmeter	cnt;

extern char	etext[], edata[], end[];

void	monitor(void);
void	aio_user_init(void);
void	proc_table_init(void);
extern void	hal_outb(int, int);		/* shared-window proc table (usermode.c) */
int	pccom_present(void);
void	reboot_now(void);
int	user_page_fault(unsigned va, unsigned err);
void	user_fault(int sig, unsigned eip, unsigned va);

/*
 * Called from pmap/machdep before page-size dependent code runs.
 */
/* 中文：在页面相关代码运行前设置系统页大小：校验并固定为 DEFAULT_PAGE_SIZE，计算页移位与页掩码 */
/* EN: Set the system page size before page-dependent code runs: validate and fix it to DEFAULT_PAGE_SIZE, compute page shift and mask */
void
vm_set_page_size(void)
{

	if (cnt.v_page_size == 0)
		cnt.v_page_size = DEFAULT_PAGE_SIZE;
	if (cnt.v_page_size != DEFAULT_PAGE_SIZE)
		panic("vm_set_page_size: page size mismatch");
	page_shift = 12;
	page_mask = (vm_size_t)cnt.v_page_size - 1;
}

/*
 * Formatting output.  Deliberately small: %c %s %d %u %x %o %p %b.
 */
/* 中文：按给定进制、宽度和填充字符把一个无符号长整型格式化输出到控制台 */
/* EN: Format and output an unsigned long to the console in the given base, width and pad character */
static void
putnum(unsigned long v, int base, int width, int pad)
{
	char buf[32];
	int i = 0;

	if (v == 0)
		buf[i++] = '0';
	while (v) {
		int d = v % base;

		buf[i++] = d < 10 ? '0' + d : 'a' + d - 10;
		v /= base;
	}
	while (i < width--)
		buf[i++] = pad;
	while (i-- > 0)
		cnputc(buf[i]);
}

/* 中文：精简格式化输出核心：解析 %c/%s/%d/%u/%x/%o/%p/%b 等转换并逐字符经 cnputc 输出 */
/* EN: Minimal formatted-output core: parse conversions like %c/%s/%d/%u/%x/%o/%p/%b and emit characters via cnputc */
static void
vprintf(const char *fmt, va_list ap)
{
	const char *p;

	for (p = fmt; *p; p++) {
		int width = 0, pad = ' ', lflag = 0;

		if (*p != '%') {
			cnputc(*p);
			continue;
		}
		p++;
		if (*p == '-') { pad = ' '; p++; }
		if (*p == '0') { pad = '0'; p++; }
		while (*p >= '0' && *p <= '9')
			width = width * 10 + (*p++ - '0');
		while (*p == 'l') { lflag++; p++; }
		switch (*p) {
		case 'd': {
			long v = lflag ? va_arg(ap, long) : va_arg(ap, int);

			if (v < 0) { cnputc('-'); v = -v; }
			putnum(v, 10, width, pad);
			break;
		}
		case 'u':
			putnum(lflag ? va_arg(ap, unsigned long) :
			    va_arg(ap, unsigned int), 10, width, pad);
			break;
		case 'x':
			putnum(lflag ? va_arg(ap, unsigned long) :
			    va_arg(ap, unsigned int), 16, width, pad);
			break;
		case 'o':
			putnum(lflag ? va_arg(ap, unsigned long) :
			    va_arg(ap, unsigned int), 8, width, pad);
			break;
		case 'p':
			cnputc('0'); cnputc('x');
			putnum((unsigned long)va_arg(ap, void *), 16, 8, '0');
			break;
		case 'c':
			cnputc(va_arg(ap, int));
			break;
		case 's': {
			char *s = va_arg(ap, char *);

			if (s == NULL)
				s = "(null)";
			while (*s)
				cnputc(*s++);
			break;
		}
		case 'b': {
			int v = va_arg(ap, int);

			putnum(v, 2, width, pad);
			break;
		}
		case '%':
			cnputc('%');
			break;
		default:
			cnputc('%');
			cnputc(*p);
			break;
		}
	}
}

/* 中文：可变参数格式化输出到控制台，封装 vprintf */
/* EN: Variadic formatted output to the console, wrapping vprintf */
void
printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* 中文：内核 printf 别名，与 printf 同样经 vprintf 格式化输出 */
/* EN: Kernel printf alias, formatting output through vprintf just like printf */
void
kprintf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* 中文：内核致命错误处理：打印 panic 前缀与消息后进入无限循环停机 */
/* EN: Kernel fatal-error handler: print a panic prefix and message, then halt in an infinite loop */
void
panic(const char *fmt, ...)
{
	va_list ap;

	printf("\npanic: ");
	va_start(ap, fmt);
	/* simple: panic strings are plain */
	printf("%s", fmt);
	va_end(ap);
	printf("\n");
	for (;;)
		;
}

/*
 * Trap entry.  locore pushes a trapframe and calls trap().  Without the
 * VM fault machinery we report and stop rather than corrupt state.
 */
/* 中文：陷阱/异常入口：用户态错误映射为信号交付，内核态错误打印寄存器与调用栈信息后停机 */
/* EN: Trap/exception entry: map user-mode faults to signal delivery; for kernel faults print registers and halt the system */
void
trap(struct trapframe *frame)
{
	static const char *trapname[] = {
		"divide", "debug", "nmi", "breakpoint", "overflow", "bounds",
		"invalid opcode", "no coprocessor", "double fault",
		"coprocessor overrun", "invalid tss", "segment not present",
		"stack fault", "general protection", "page fault", "reserved",
	};

	/*
	 * os.md mechanism 1: a user-mode fault is delivered to the process,
	 * never used to halt the machine.  COW write faults are fixed here
	 * and the faulting instruction simply retries.
	 */
	if ((frame->tf_cs & 3) != 0) {
		unsigned va = rcr2();
		int sig = SIGSEGV;

		if (frame->tf_trapno == T_PAGEFLT &&
		    user_page_fault(va, frame->tf_err))
			return;
		switch (frame->tf_trapno) {
		case T_DIVIDE:
		case T_OFLOW:
		case T_ARITHTRAP:
		case T_FPOPFLT:
			sig = SIGFPE;
			break;
		case T_TRCTRAP:
		case T_BPTFLT:
			sig = SIGTRAP;
			break;
		case T_RESOPFLT:
		case T_PRIVINFLT:
		case T_DNA:
			sig = SIGILL;
			break;
		case T_BOUND:
		case T_SEGFLT:
		case T_PROTFLT:
		case T_STKFLT:
		case T_SEGNPFLT:
		case T_PAGEFLT:
		case T_TABLEFLT:
			sig = SIGSEGV;
			break;
		default:
			sig = SIGSEGV;
			break;
		}
		user_fault(sig, frame->tf_eip, va);
	}
	printf("\n\n*** kernel TRAP %d (%s) err=%x\n", frame->tf_trapno,
	    frame->tf_trapno < 16 ? trapname[frame->tf_trapno] : "unknown",
	    frame->tf_err);
	printf("eip=%x cs=%x eflags=%x esp=%x ss=%x\n",
	    frame->tf_eip, frame->tf_cs, frame->tf_eflags,
	    frame->tf_esp, frame->tf_ss);
	if ((frame->tf_cs & 3) != 0) {
		const unsigned char *p =
		    (const unsigned char *)(frame->tf_eip - 8);
		int i;

		printf("code:");
		for (i = 0; i < 16; i++)
			printf(" %x", p[i]);
		printf("\n");
	}
	printf("eax=%x ebx=%x ecx=%x edx=%x\n",
	    frame->tf_eax, frame->tf_ebx, frame->tf_ecx, frame->tf_edx);
	printf("esi=%x edi=%x ebp=%x ds=%x es=%x\n",
	    frame->tf_esi, frame->tf_edi, frame->tf_ebp,
	    frame->tf_ds, frame->tf_es);
	printf("cr2=%x\n", rcr2());

	printf("system halted.\n");
	for (;;)
		;
}

/* 中文：系统调用占位实现：打印未实现提示并返回 0 */
/* EN: Placeholder system-call implementation: print a not-implemented message and return 0 */
int
syscall(void)
{

	printf("syscall: not implemented\n");
	return (0);
}

/*
 * The 4.4BSD kernel proper enters userland here.  We have no userland:
 * hand control to the on-board monitor instead.
 */
/* 中文：内核主入口：打印系统信息，探测并挂载根文件系统（ISO/FFS/tmpfs），初始化压缩池、子系统、线程、软中断与时钟，最后交控制权给 monitor */
/* EN: Kernel main entry: print system info, probe and mount the root filesystem (ISO/FFS/tmpfs), init zcomp pool, subsystems, threads, softirqs and clock, then hand control to monitor */
void
main(vm_offset_t firstaddr)
{
	extern vm_offset_t avail_start, avail_end, virtual_avail, virtual_end;
	extern int IdlePTD;
	extern int Maxmem, physmem;
	extern void page_alloc_set_floor(unsigned);

	/*
	 * The bump allocator must start *after* this image.  locore passes the
	 * page-rounded _end plus its bootstrap pages, and the setter only ever
	 * clamps upwards (floor 0x200000), so a kernel that grows past 2 MB can
	 * never have its own pages handed out as free memory.
	 * bump 分配器起点必须在内核镜像之后（只向上取整，最低 0x200000），
	 * 内核长大后不会把自己的页当空闲页发出去。
	 */
	page_alloc_set_floor((unsigned)firstaddr);

	/*
	 * Layout guard: the scratch mapping pages (0xFE200000..0xFE202FFF,
	 * see SCRATCH_PD in usermode.c) are rewritten by map_page() on every
	 * page access, so the kernel image must stay below them.  A silent
	 * overlap corrupts kernel variables instead of faulting, which is very
	 * expensive to debug - fail loudly instead.
	 * locore passes firstaddr = page_rounded(_end) + 7 bootstrap pages.
	 * 布局护栏：临时映射页（见 usermode.c 的 SCRATCH_PD）每次访问物理页都会
	 * 被重写，镜像必须留在它们下面；重叠只会静默踩坏内核变量，所以显式 panic。
	 */
	if (((unsigned)firstaddr - 7 * NBPG) >= 0xFE200000)
		panic("kernel image ends %x, overlaps scratch window 0xFE200000",
		    (unsigned)firstaddr - 7 * NBPG);

printf("\n");
	printf("4.5BSD-Lite2 (i386) ELF kernel\n");
	printf("Kernel: ELF32 i386, KERNBASE %x, first free page %x\n",
	    KERNBASE, firstaddr);
	printf("Memory: maxmem %x, physmem %x, avail %x-%x\n",
	    ctob(Maxmem), ctob(physmem), avail_start, avail_end);
	printf("pmap: kernel_pmap %p pdir %p IdlePTD %x\n",
	    kernel_pmap, kernel_pmap->pm_pdir, IdlePTD);
	printf("VM: virtual_avail %x virtual_end %x\n",
	    virtual_avail, virtual_end);
	printf("Console: VGA 80x25, COM1 %s\n",
	    pccom_present() ? "present" : "absent");

	/*
	 * S4 / install: pick the root filesystem.  When the live ISO is in
	 * the drive it wins (so the `install` command is available); with no
	 * CD we look for an FFS on the ATA disk -- that is the installed
	 * system booting from its VMware virtual disk.  vfs_init() is always
	 * called so rootvnode->ops is never NULL when the shell starts.
	 */
	{
		int from_disk = 0;

		if (ata_probe() == 0 && ata_present()) {
			if (jlfs_mount() == 0) {
				printf("ata: ATA disk %d sectors, JLFS mounted (rw)\n",
				    ata_capacity());
				from_disk = 2;    /* VFS_ROOT_JLFS */
			} else if (ffs_mount() == 0) {
				printf("ata: ATA disk %d sectors, FFS mounted (ro)\n",
				    ata_capacity());
				from_disk = 1;    /* VFS_ROOT_FFS */
			} else
				printf("ata: no FFS/JLFS on ATA disk\n");
		} else
			printf("ata: no ATA disk\n");

		if (from_disk == 0 && atapi_probe() == 0) {
			char vendor[9], product[17];

			if (atapi_inquiry(vendor, product) == 0)
				printf("atapi: %s %s\n", vendor, product);
			{
				unsigned cap;

				if (atapi_capacity(&cap) == 0)
					printf("atapi: capacity %d sectors\n", cap + 1);
			}
			if (iso_mount() == 0) {
				vfs_set_root_source(VFS_ROOT_ISO);
				vfs_init();
				printf("root: ISO9660 on ATAPI CD, lba %d, %d bytes\n",
				    iso_root(), iso_root_size());
			} else {
				/* CD present but unreadable: fall back to the disk. */
				vfs_set_root_source(from_disk == 2 ? VFS_ROOT_JLFS :
				    (from_disk ? VFS_ROOT_FFS : VFS_ROOT_TMPFS));
				vfs_init();
				printf("root: %s\n", from_disk ?
				    "FFS on ATA disk" : "tmpfs (empty)");
			}
		} else {
			vfs_set_root_source(from_disk == 2 ? VFS_ROOT_JLFS :
				    (from_disk ? VFS_ROOT_FFS : VFS_ROOT_TMPFS));
			vfs_init();
			printf("root: %s (no ATAPI CD-ROM)\n", from_disk ?
			    "FFS on ATA disk" : "tmpfs (empty)");
		}
	}
	/*
	 * os.md mechanism 3: reserve the in-RAM compressed-page pool before
	 * userland starts.  No swap device exists; this is the reclaim layer.
	 * os.md 机制 3：在用户态启动前预留内存压缩池；本端口没有 swap
	 * 设备，这一层就是回收层。
	 */
	zcomp_init();
	printf("kstr: strings %s\n", KSTR_UTF16 ? "UTF-16" : "ASCII");
	/*
	 * Kernel execution carriers for deferred/page-reclaim work.  The old
	 * subsys/ personality layer and IPL/preemption space were removed:
	 * syscalls dispatch straight to bsd45_syscall(), and the PIT is a
	 * plain tick source.
	 */
	kthread_init();
	proc_table_init();		/* shared-window proc table (before pmap_create) */
	softirq_init();
	vfs_aio_init();
	aio_user_init();
	smp_init();			/* per-CPU state + Local APIC probe */
	clock_init();
	printf("\n");

	monitor();
}

