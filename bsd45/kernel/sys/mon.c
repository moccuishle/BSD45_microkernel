/*
 * mon.c - on-board monitor for the stand-alone 4.4BSD-Lite2 i386 kernel.
 *
 * The monitor is not part of 4.4BSD; it is the "userland" of this
 * bring-up kernel, exercising the genuine pmap/VM code from the source
 * tree.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/kprocess.h>
#include <sys/user.h>
#include <sys/reboot.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_kern.h>

#include <machine/cpu.h>
#include <machine/pmap.h>

#include "ata.h"
#include "isofs.h"
#include "ffs/ffs.h"
#include "vfs.h"
#include "sys/platform.h"
#include "thread/kthread.h"
#include "sys/sync.h"
#include "sys/smp.h"
#include "sys/kipc.h"
#include "vm/zcomp.h"
#include <vm/mem_plan.h>
extern void cow_print(void);
extern int install_to_disk(void);

extern vm_offset_t avail_start, avail_end, virtual_avail, virtual_end;
extern int KPTphys;
extern int Maxmem, physmem;
extern void start_userland(const char *script);
extern void start_prog(const char *path, const char *script);
extern int IdlePTD;
extern char etext[], edata[], end[];

static char line[128];

/* 中文：从控制台读取一行输入到全局 line 缓冲区，处理退格并回显，返回字符数 */
/* EN: Read a line of input from the console into the global line buffer, handling backspace with echo and returning the character count */
static int
getline(void)
{
	int n = 0, c;

	for (;;) {
		c = cngetc();
		if (c == 0)
			continue;
		if (c == '\r' || c == '\n') {
			cnputc('\n');
			line[n] = 0;
			return (n);
		}
		if (c == '\b' || c == 0x7f) {
			if (n > 0) {
				n--;
				printf("\b \b");
			}
			continue;
		}
		if (c >= ' ' && c < 0x7f && n < (int)sizeof(line) - 1) {
			line[n++] = c;
			cnputc(c);
		}
	}
}

/* 中文：把单个十六进制字符转换为其数值，非法字符返回 -1 */
/* EN: Convert a single hexadecimal character to its numeric value, returning -1 for invalid characters */
static int
hexval(int c)
{
	if (c >= '0' && c <= '9')
		return (c - '0');
	if (c >= 'a' && c <= 'f')
		return (c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return (c - 'A' + 10);
	return (-1);
}

/* 中文：解析字符串 s 中的十六进制数（可带 0x 前缀与前导空格），结果写入 *vp 并返回解析的位数 */
/* EN: Parse a hexadecimal number from string s (optional 0x prefix and leading spaces), storing the result in *vp and returning the digit count */
static int
parsehex(const char *s, unsigned *vp)
{
	unsigned v = 0;
	int n = 0, d;

	while (*s == ' ')
		s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while ((d = hexval(*s)) >= 0) {
		v = (v << 4) | d;
		s++;
		n++;
	}
	*vp = v;
	return (n);
}

/* 中文：monitor 命令，打印物理内存与虚拟地址空间的统计和内核各段地址 */
/* EN: Monitor command that prints physical memory and virtual address space statistics and the kernel segment addresses */
static void
cmd_mem(void)
{
	printf("Maxmem    %d pages  (%dK)\n", Maxmem, ctob(Maxmem));
	printf("physmem   %d pages  (%dK)\n", physmem, ctob(physmem));
	printf("avail     %x - %x\n", avail_start, avail_end);
	printf("virt      %x - %x\n", virtual_avail, virtual_end);
	printf("kernel    text %x data %x end %x\n", etext, edata, end);
}

/* 中文：monitor 命令，解析给定虚拟地址并打印其在内核页表中的页表项与属性位 */
/* EN: Monitor command that parses a given virtual address and prints its kernel page table entry and attribute bits */
static void
cmd_pte(const char *arg)
{
	unsigned va;
	struct pte *pte;

	if (parsehex(arg, &va) == 0) {
		printf("usage: pte <hex-address>\n");
		return;
	}
	pte = pmap_pte(kernel_pmap, (vm_offset_t)va);
	if (pte == NULL) {
		printf("%x: no page table\n", va);
		return;
	}
	printf("%x: pte %x pfn %x%s%s%s\n", va, *(int *)pte,
	    *(int *)pte >> 12,
	    (*(int *)pte & PG_V) ? " V" : "",
	    (*(int *)pte & PG_RW) ? " RW" : " RO",
	    (*(int *)pte & PG_U) ? " U" : " K");
}

/*
 * cmd_vstat - S10.1: vnode pool + physical page accounting.  Run it before
 * and after a stress loop: "vnode used" must come back down and
 * "pages alloc" must not keep growing.
 */
/* 中文：monitor 命令，打印 vnode 池与物理页的统计以及缓冲/名字/页缓存计数 */
/* EN: Monitor command that prints vnode pool and physical page statistics plus buffer/name/page cache counters */
static void
cmd_vstat(void)
{
	unsigned vt, vu, vf, pa, pf, pi;
	char buf[512];

	vfs_stats(&vt, &vu, &vf, &pa, &pf, &pi);
	printf("vstat: vnode total %d used %d free %d\n", vt, vu, vf);
	printf("vstat: pages alloc %d freed %d inuse %d\n", pa, pf, pi);
	/* S17: buffer/name/page cache counters (/dev/vmstat has the same) */
	vfs_cache_report(buf, sizeof(buf));
	printf("%s", buf);
}

/* 中文：monitor 命令，测试 kstr 路径API，解析 /readme.txt 并报告 ASCII/UTF16 路径转换是否成功 */
/* EN: Monitor command that tests the kstr path API by resolving /readme.txt and reporting whether ASCII/UTF16 path conversion succeeds */
static void
cmd_wtest(void)
{
#ifdef KERNEL_UNICODE
	unsigned short buf[64];
#else
	char buf[64];
#endif
	struct kstr s;
	struct vnode *vp = 0;

	kstr_init(&s, buf, sizeof(buf));
	if (kstr_set_a(&s, "/readme.txt") < 0 ||
	    vfs_namei_k(&s, &vp) < 0 || vp == 0) {
		printf("wtest: FAIL\n");
		return;
	}
	vn_put(vp);
	printf("wtest: kstr path %s\n", KSTR_UTF16 ? "utf16" : "ascii");
	printf("wtest: PASS\n");
}

/*
 * cmd_vstress - S10.1 self test: run VFS-heavy loops inside the kernel and
 * print the pool counters before/after.  If vnodes or pages leak, "used"
 * and "inuse" keep climbing instead of returning to the baseline.
 */
/* 中文：把前缀 prefix 与整数 n 的十进制表示拼接成路径字符串写入 buf */
/* EN: Build a path string in buf by concatenating the prefix and the decimal representation of integer n */
static void
mkpath(char *buf, const char *prefix, int n)
{
	char *p = buf;

	while (*prefix)
		*p++ = *prefix++;
	if (n >= 100)
		*p++ = '0' + (n / 100) % 10;
	if (n >= 10)
		*p++ = '0' + (n / 10) % 10;
	*p++ = '0' + n % 10;
	*p = 0;
}

/* 中文：以给定标签 tag 打印 vnode 池与物理页的统计数据 */
/* EN: Print vnode pool and physical page statistics with the given tag label */
static void
show_vstat(const char *tag)
{
	unsigned vt, vu, vf, pa, pf, pi;

	vfs_stats(&vt, &vu, &vf, &pa, &pf, &pi);
	printf("vstat[%s]: vnode total %d used %d free %d\n", tag, vt, vu, vf);
	printf("vstat[%s]: pages alloc %d freed %d inuse %d\n",
	    tag, pa, pf, pi);
}

/* 中文：monitor 压力自测命令，反复执行 VFS 密集操作（ISO 查找读取、tmpfs 建删目录/重写文件）并对比前后池计数以检测泄漏 */
/* EN: Monitor stress self-test that repeatedly performs VFS-heavy operations (ISO lookups/reads, tmpfs mkdir/rmdir and file rewrites) and compares pool counters before and after to detect leaks */
static void
cmd_vstress(void)
{
	struct vnode *vp, *dv;
	char path[32], name[48], buf[64];
	int i, ok;

	show_vstat("before");

	/* 100x ISO lookup + read + vn_put */
	ok = 0;
	for (i = 0; i < 100; i++) {
		if (vfs_namei("/readme.txt", &vp) == 0) {
			if (vp->ops->read)
				vp->ops->read(vp, 0, buf, sizeof(buf));
			vn_put(vp);
			ok++;
		}
	}
	printf("vstress: iso read 100x ok=%d\n", ok);

	/* 100x tmpfs mkdir + rmdir */
	ok = 0;
	for (i = 0; i < 100; i++) {
		mkpath(path, "/tmp/s", i);
		if (vfs_create(path, 1, &vp) == 0) {
			vn_put(vp);
			if (vfs_lookup_parent(path, &dv, name) == 0) {
				if (dv->ops->rmdir && dv->ops->rmdir(dv, name) == 0)
					ok++;
				vn_put(dv);
			}
		}
	}
	printf("vstress: tmpfs mkdir/rmdir 100x ok=%d\n", ok);

	/* 100x rewrite the same tmpfs file (truncate must free the old page) */
	ok = 0;
	for (i = 0; i < 100; i++) {
		if (vfs_create("/tmp/v", 0, &vp) == 0) {
			if (vp->ops->truncate)
				vp->ops->truncate(vp);
			if (vp->ops->write &&
			    vp->ops->write(vp, 0, "x", 1) == 1)
				ok++;
			vn_put(vp);
		}
	}
	printf("vstress: tmpfs rewrite 100x ok=%d\n", ok);
	if (vfs_lookup_parent("/tmp/v", &dv, name) == 0) {
		if (dv->ops->remove)
			dv->ops->remove(dv, name);
		vn_put(dv);
	}

	show_vstat("after");
}

/* 中文：monitor 命令，分配一个物理页映射到内核，写入并回读数据以验证 pmap 映射正确 */
/* EN: Monitor command that allocates and maps a physical page in the kernel, then writes and reads it back to verify pmap mapping */
static void
cmd_map(void)
{
	void *p;
	int i;

	p = pmap_bootstrap_alloc(PAGE_SIZE);
	if (p == NULL) {
		printf("pmap_bootstrap_alloc failed\n");
		return;
	}
	outb(0xE9, 'm');
	printf("mapped page at %p\n", p);
	printf("pte new page: %x\n", *(int *)pmap_pte(kernel_pmap, (vm_offset_t)p));
	((int *)p)[0] = 0x4f534442;	/* "BSDO" */
	((int *)p)[1023] = 0x444e4521;
	printf("readback %x %x\n", ((int *)p)[0], ((int *)p)[1023]);
	printf("ok\n");
}

/* 中文：通过键盘控制器复位重启系统，失败时以三重故障兜底 */
/* EN: Reboot the system via the keyboard controller, falling back to a triple fault if that fails */
void
reboot_now(void)
{
	int i;

	printf("rebooting...\n");
	for (i = 0; i < 100000; i++)
		;
	outb(0x64, 0xfe);
	for (i = 0; i < 100000; i++)
		;
	*(int *)0 = 0;		/* triple fault fallback */
	for (;;)
		;
}

/* 中文：板载监控器主循环，打印提示符读取命令并分派执行内存、VFS、子系统、线程、时钟等测试与操作命令 */
/* EN: The on-board monitor main loop, printing a prompt, reading commands and dispatching memory, VFS, subsystem, thread and clock tests and operations */
void
monitor(void)
{
	printf("BSD/386 monitor - type 'help' for commands\n");
#ifdef S10_AUTOTEST
	/* verification build only: run the S10.1 stress without a keyboard */
	cmd_vstress();
#endif
#ifdef VM_INSTALL_AUTOTEST
	/* verification build only: run the ISO->disk install without a keyboard.
	 * VMware serial pipes cannot be read reliably from the host, so the
	 * install is triggered in-kernel and the console is captured to a file. */
	if (install_to_disk() == 0)
		printf("AUTOINSTALL: SUCCESS\n");
	else
		printf("AUTOINSTALL: FAILED\n");
#endif
#ifdef VM_PROCTEST_AUTOTEST
	/* verification build only: run the dynamic process-count stress test
	 * without a keyboard; console is captured to the VMware serial file.
	 * VMware 验证构建：免键盘自动运行动态进程数压力测试，控制台捕获到串口文件。 */
	printf("AUTOTEST: launching /bin/proctest via init script\n");
	/* Go through init->sh -c so execve() does the path/name lookup, exactly
	 * like the interactive `shell|/bin/vfstest` flow that is known to work. */
	start_userland("/bin/proctest");
	/* start_userland enters user mode and does not return. */
#endif
#ifdef VM_SELFTEST_AUTOTEST
	/* verification build only: the ordinary acceptance tools, run without a
	 * keyboard.  VMware's serial port is output-only here, so the only way
	 * to run a regression set in a guest is to script it from the kernel.
	 * The script is handed to /bin/sh -c, so each tool's PASS/FAIL lines end
	 * up in the serial log next to the usual boot output.
	 * 仅验证构建：免键盘运行常规验收工具。VMware 的串口在这里只能输出，
	 * 想在 guest 里跑回归只能由内核代跑；脚本交给 /bin/sh -c，各工具的
	 * PASS/FAIL 行会和启动日志一起落进串口文件。 */
	printf("AUTOTEST: running vfstest/shmtest/mmaptest\n");
	start_userland("/bin/vfstest; /bin/shmtest; /bin/mmaptest; echo SELFTEST-DONE");
	/* start_userland enters user mode and does not return. */
#endif

	for (;;) {
		printf("bsd45> ");
		if (getline() == 0)
			continue;

		if (!strcmp(line, "help"))
			printf("help  mem  vstat  vstress  pte <addr>  map  banner  "
			    "usermode  shell  test  cd  reboot  halt\n"
			    			    "      kthread  softirq  kthreadtest  softirqtest\n"
			    "      schedtest  synctest  smpinfo  cpuinfo  smpping  ipitest\n"
			    "      smpon  smpuser  memplan\n"
			    "      ipctest  ipcinfo\n"
			    "      clock  lat  cow\n"
			    "      zcomp  zcomptest\n"
			    "      aio  aiotest\n"
			    "      wtest\n"
			    "      memplan            (128MB..24GB memory policy table)\n"
			    "      install  disk      (ISO -> virtual disk install)\n");
		else if (!strcmp(line, "mem"))
			cmd_mem();
		else if (!strcmp(line, "memplan"))
			mem_plan_table();
		else if (!strcmp(line, "vstat"))
			cmd_vstat();
		else if (!strcmp(line, "vstress"))
			cmd_vstress();
		else if (!strcmp(line, "kthread"))
			kthread_print();
		else if (!strcmp(line, "softirq"))
			softirq_print();
		else if (!strcmp(line, "schedtest")) {
			(void)sched_prio_selftest();
		}
		else if (!strcmp(line, "synctest")) {
			int n = sync_selftest();

			printf("synctest: %s\n", n ? "PASS" : "FAIL");
		}
		else if (!strcmp(line, "smpinfo"))
			smp_print();
		else if (!strcmp(line, "smpuser")) {
			/*
			 * S413: toggle AP user-thread dispatch.  `smpon` alone
			 * brings the APs up; this decides whether they also
			 * schedule user processes.
			 */
			smp_user_dispatch = !smp_user_dispatch;
			printf("smpuser: AP user dispatch %s\n",
			    smp_user_dispatch ? "on" : "off (BSP only)");
		}
		else if (!strcmp(line, "cpuinfo"))
			cpuinfo_print();
		else if (!strcmp(line, "smpping")) {
			smp_reschedule();
			printf("smpping: IPI sent to active APs\n");
		}
		else if (!strcmp(line, "smpon")) {
			int n = smp_enable();

			printf("smpon: %d CPU(s) online\n", n);
		}
		else if (!strcmp(line, "ipitest")) {
			int n = ipi_selftest();

			printf("ipitest: %s\n", n ? "PASS" : "FAIL");
		}
		else if (!strcmp(line, "ipctest")) {
			int n = ipc_selftest();

			printf("ipctest: %s\n", n ? "PASS" : "FAIL");
		}
		else if (!strcmp(line, "ipcinfo"))
			ipc_print();
		else if (!strcmp(line, "kthreadtest")) {
			unsigned n = kthread_selftest();

			printf("kthreadtest: counter %d %s\n", n,
			    n == 3 ? "PASS" : "FAIL");
		}
		else if (!strcmp(line, "softirqtest")) {
			int n = softirq_selftest();

			printf("softirqtest: count %d %s\n", n,
			    n > 0 ? "PASS" : "FAIL");
		}
		else if (!strcmp(line, "clock"))
			clock_print();
		else if (!strcmp(line, "lat"))
			clock_lat_print();
		else if (!strcmp(line, "zcomp"))
			zcomp_print();
		else if (!strcmp(line, "zcomptest")) {
			if (zcomp_selftest() != 0)
				printf("zcomptest: FAIL\n");
		}
		else if (!strcmp(line, "aio")) {
			unsigned q, d, p;

			vfs_aio_stats(&q, &d, &p);
			printf("aio: queued %d done %d pending %d\n",
			    q, d, p);
		}
		else if (!strcmp(line, "aiotest")) {
			if (vfs_aio_selftest() != 0)
				printf("aiotest: FAIL\n");
		}
		else if (!strcmp(line, "cow"))
			cow_print();
		else if (!strcmp(line, "wtest"))
			cmd_wtest();
		else if (!strncmp(line, "pte", 3))
			cmd_pte(line + 3);
		else if (!strcmp(line, "usermode"))
			start_userland("echo hello; exit 3");
		else if (!strcmp(line, "shell"))
			start_userland(0);
		else if (!strcmp(line, "cd")) {
			if (iso_mounted())
				printf("ISO9660 root: lba %d, %d bytes\n",
				    iso_root(), iso_root_size());
			else
				printf("no ISO9660 root mounted\n");
		}
		else if (!strcmp(line, "disk")) {
			if (ata_present())
				printf("ATA disk: %d sectors (%d MiB), FFS %s\n",
				    ata_capacity(), ata_capacity() / 2048,
				    ffs_mounted() ? "mounted" : "not mounted");
			else
				printf("no ATA disk detected\n");
		}
		else if (!strcmp(line, "install")) {
			if (install_to_disk() != 0)
				printf("install: FAILED (see messages above)\n");
		}
		else if (!strcmp(line, "test"))
			start_prog("/test", 0);
		else if (!strcmp(line, "map"))
			cmd_map();
		else if (!strcmp(line, "banner"))
			printf("4.5BSD-Lite2 i386 ELF kernel, KERNBASE %x\n",
			    KERNBASE);
		else if (!strcmp(line, "reboot"))
			reboot_now();
		else if (!strcmp(line, "halt"))
			for (;;)
				;
		else if (line[0])
			printf("unknown command: %s\n", line);
	}
}
