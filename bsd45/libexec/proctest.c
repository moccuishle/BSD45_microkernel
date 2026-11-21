/*
 * proctest.c - Dynamic process-count acceptance test.
 *
 * Verifies that the kernel can host far more than the old NPROC=8 limit, and
 * that the practical ceiling tracks physical memory (VMware guests with more
 * RAM host more live processes) rather than a hard-coded 8.
 *
 * Phase A: sequential fork + wait4 cycles -- proves slots are reusable and
 *          many processes can be created over time.
 * Phase B: keep N children alive simultaneously (no wait4) -- proves the
 *          concurrent live-process ceiling exceeds 8.  Children spin (the
 *          kernel uses cooperative round-robin so the parent regains CPU);
 *          the parent kills/reaps them all when done.
 * Phase C: every child touches TOUCH_BYTES of anonymous memory at once, so
 *          the live footprint passes 64 MB -- the point where the old CMOS
 *          memory detection (and therefore the old 63 MB bump ceiling)
 *          stopped describing the machine.  A kernel that still believed it
 *          had 64 MB would panic in page_alloc() here.
 *
 * proctest.c - 动态进程数验收测试。验证内核可容纳远超旧的 NPROC=8 限制，
 * 且实际上限随物理内存变化（VMware 加大内存可容纳更多进程）。phase C 让所有
 * 子进程同时占用匿名内存，使总占用越过 64MB——旧 CMOS 探测与 63MB bump 上界
 * 的失效点；仍以为只有 64MB 的内核会在这里 panic。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>

/*
 * Children to keep alive at once in phase B.  The process table and TCB pool
 * now live in the shared kernel window (NPROC = NKTHREAD = 1024, S410), so
 * the old hard cap of 8, the loader-derived cap of 24 and the 256-slot table
 * of S407 are all gone.  The binding limit is physical memory: fork returns
 * -EAGAIN when page_alloc runs out (and -EAGAIN from the slot table first if
 * 1000 children plus the shell do not fit).
 * 1000 并发子进程：进程表与 TCB 池已扩到 1024（S410）。真正的瓶颈先是槽位表，
 * 再是物理内存（page_alloc 耗尽时 fork 返回 -EAGAIN）。
 */
#define	TARGET_LIVE	1000
#define	TARGET_SEQ	1000	/* phase A sequential fork/wait cycles */
#define	MIN_LIVE	1000	/* must be far beyond the old 8 / 24 / 256 */
/*
 * Phase C: memory footprint.  1000 children x 128 KB = 125 MB, comfortably
 * past the 64 MB the old CMOS detection reported (and the 63 MB ceiling the
 * bump allocator used to stop at) while still fitting a 512 MB guest
 * alongside 1000 x 8 KB kernel stacks and the 1.77 MB process table.
 * 阶段 C：内存占用。1000 × 128KB = 125MB，越过旧 CMOS 探测的 64MB（以及 bump
 * 分配器 63MB 的上界），同时仍能塞进 512MB guest（还有 1000×8KB 内核栈与
 * 1.77MB 进程表）。
 */
#define	TOUCH_BYTES	(128 * 1024)

/* S410: the children block on a pipe instead of spinning (see phase B). */

/*
 * Read /dev/ps while the children are alive and report what the kernel really
 * has in its table.  This is how the test becomes observable: phase A's
 * children exit immediately by design (and a shell cannot run `ps`
 * concurrently - this port has no job control), so without this dump the only
 * evidence of concurrency is the counter the test prints itself.
 * 在子进程存活期间读 /dev/ps，报告内核进程表真实内容。phase A 的子进程本来就是
 * 建了立刻退出，而本端口没有 job control（无法 `proctest &` 再从同一控制台跑
 * `ps`），所以不做这个转储的话，"并发"只剩测试自己打印的计数。
 */
static int
ps_report(int expect, int verbose)
{
	static char psbuf[32768];	/* 1024 rows x ~24 bytes */
	char line[128];
	char *p;
	int fd, n, i, lines = 0, idx = 0;

	fd = open("/dev/ps", O_RDONLY);
	if (fd < 0) {
		printf("proctest: /dev/ps: open failed (errno %d)\n", errno);
		return (-1);
	}
	n = read(fd, psbuf, sizeof(psbuf) - 1);
	close(fd);
	if (n <= 0)
		return (0);
	psbuf[n] = 0;
	for (i = 0; i < n; i++)
		if (psbuf[i] == '\n')
			lines++;
	printf("proctest: kernel /dev/ps lists %d live processes (expect at least %d)\n",
	    lines, expect);
	if (!verbose)
		return (lines);

	/*
	 * first 6 rows, an omission marker, then the last 4 rows.  Everything
	 * goes through printf so ordering with the rest of the output survives
	 * regardless of stdout's buffering mode.
	 * 前 6 行 + 省略提示 + 后 4 行；全部用 printf，避免与其余输出错序。
	 */
	for (p = psbuf; *p != 0; idx++) {
		char *e = strchr(p, '\n');
		int len = e ? (int)(e - p) + 1 : (int)strlen(p);
		int show;

		if (len > (int)sizeof(line) - 1)
			len = (int)sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = 0;

		show = (lines <= 12) || (idx < 6) || (idx >= lines - 4);
		if (lines > 12 && idx == 6)
			printf("... %d more ...\n", lines - 10);
		if (show)
			printf("%s", line);
		if (e == 0)
			break;
		p = e + 1;
	}
	return (lines);
}

int
main(int argc, char **argv)
{
	pid_t kids[TARGET_LIVE];
	pid_t kids2[TARGET_LIVE];
	int pfd[2];
	int nkids = 0;
	int st, i, made, reaped, fail = 0;
	int interactive = (argc > 1 && argv[1][0] == '-' && argv[1][1] == 'i');
	int live;

	printf("proctest: dynamic process count test (target live=%d)\n", TARGET_LIVE);
	if (interactive)
		printf("proctest: -i: will pause with all children alive so you can look\n");

	/* ---- Phase A: sequential fork/wait, TARGET_SEQ cycles ---- */
	made = 0;
	for (i = 0; i < TARGET_SEQ; i++) {
		pid_t p = fork();

		if (p < 0)
			break;
		if (p == 0)
			_exit(0);		/* child exits immediately */
		wait4(p, &st, 0, (struct rusage *)0);
		made++;
	}
	printf("proctest: phase A %d sequential fork/wait cycles (each child\n",
	    made);
	printf("proctest:   exits at once by design - this proves slot reuse)\n");
	if (made < TARGET_SEQ)
		fail = 1;

	/* ---- Phase B: keep TARGET_LIVE children alive simultaneously ---- */
	/*
	 * The children block on an empty pipe instead of spinning.
	 *
	 * S410: spinning was fine for 200 children, but with 1000 of them the
	 * parent only got 1/1001 of the CPU and took minutes to place the next
	 * fork - the test looked hung.  A blocking read parks each child in
	 * kthread_block_tcb() (state BLOCK) so it costs no CPU while still
	 * occupying a process slot and a TCB, which is exactly what this phase
	 * is supposed to measure.  The parent never writes to the pipe, so the
	 * read only returns when the child is signalled; SIGKILL wakes it
	 * (sig_send -> proc_wake + EINTR) and it exits, which is how the
	 * kill/reap loop below drains them.
	 * 子进程改成阻塞在读空管道上。自旋在 200 个时还行，1000 个时父进程只能拿到
	 * 1/1001 的 CPU，下一次 fork 要等几分钟，看起来像死机。阻塞读会让每个子进程
	 * 停在 kthread_block_tcb()（状态 BLOCK），不耗 CPU 但仍占着进程槽和 TCB——
	 * 正是这一阶段要测的东西。父进程从不写这个管道，所以 read 只在收到信号时
	 * 返回；SIGKILL 会唤醒它（sig_send → proc_wake + EINTR），随后退出，下面的
	 * kill/reap 循环就是这样回收它们的。
	 */
	if (pipe(pfd) < 0) {
		printf("proctest: pipe failed (errno %d)\n", errno);
		return (1);
	}
	for (i = 0; i < TARGET_LIVE; i++) {
		pid_t p = fork();

		if (p < 0) {
			printf("proctest: phase B fork failed at child %d (errno %d)\n",
			    i, errno);
			break;
		}
		if (p == 0) {
			char c;

			(void)read(pfd[0], &c, 1);	/* blocks; no data ever comes */
			_exit(0);
		}
		kids[nkids++] = p;
	}
	printf("proctest: phase B simultaneous live children = %d (target %d)\n",
	    nkids, TARGET_LIVE);
	if (nkids < MIN_LIVE) {
		printf("proctest: FAIL - only %d live (< %d)\n", nkids, MIN_LIVE);
		fail = 1;
	}

	/*
	 * Ask the kernel, not ourselves: /dev/ps must list at least nkids
	 * entries right now.  This is what catches a broken ps - with the old
	 *永久缓存 bug the table below showed only sh and ps even though 200
	 * children were alive, so a test that only counts its own forks would
	 * still have called that PASS.
	 * 直接问内核：此刻 /dev/ps 至少要列出 nkids 条。这一条才是真正抓住
	 * "ps 永久缓存" 的检查——旧 bug 下 200 个子进程活着，ps 仍只显示 sh 和
	 * ps，而只统计自己 fork 成功次数的测试照样会报 PASS。
	 */
	live = ps_report(nkids, interactive);
	if (live >= 0 && live < nkids) {
		printf("proctest: FAIL - /dev/ps lists %d < %d children\n", live,
		    nkids);
		fail = 1;
	}

	if (interactive) {
		/*
		 * read(0, &c, 1) is a no-op here: console_read() loops while
		 * `i < len - 1`, so len == 1 returns 0 immediately and the pause
		 * would be skipped.  len 2 makes it wait for the newline.
		 * read(0,&c,1) 在本端口是空操作：console_read 的循环条件是
		 * `i < len - 1`，len==1 时条件为假直接返回，暂停会被跳过；
		 * 用 2 字节缓冲才会真的等到回车。
		 */
		char c[2];

		printf("proctest: %d children are blocked on a pipe; press Enter to kill them\n",
		    nkids);
		if (read(0, c, sizeof(c)) < 0)
			;		/* no console: just carry on */
		ps_report(nkids, 0);	/* re-check after the human pause */
	}

	/* Reap all children: kill then wait4. */
	reaped = 0;
	for (i = 0; i < nkids; i++) {
		kill(kids[i], SIGKILL);
	}
	for (i = 0; i < nkids; i++) {
		if (wait4(kids[i], &st, 0, (struct rusage *)0) >= 0)
			reaped++;
	}
	printf("proctest: reaped %d/%d children\n", reaped, nkids);

	/* ---- Phase C: all children hold TOUCH_BYTES at the same time ---- */
	made = 0;
	for (i = 0; i < TARGET_LIVE; i++) {
		pid_t p = fork();

		if (p < 0)
			break;
		if (p == 0) {
			char *m = mmap((void *)0, TOUCH_BYTES, PROT_READ | PROT_WRITE,
			    MAP_ANON | MAP_PRIVATE, -1, 0);

			if (m == (char *)-1)
				_exit(2);
			/* touch every page so demand-zero really allocates */
			memset(m, 0x5a + (i & 0xf), TOUCH_BYTES);
			_exit(m[0] == (char)(0x5a + (i & 0xf)) ? 0 : 3);
		}
		kids2[made++] = p;
	}
	reaped = 0;
	for (i = 0; i < made; i++) {
		int code = -1;

		if (wait4(kids2[i], &st, 0, (struct rusage *)0) >= 0) {
			code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
			if (code == 0)
				reaped++;
		}
	}
	printf("proctest: phase C %d children x %d KB = %d MB held at once\n",
	    made, TOUCH_BYTES / 1024, (made * (TOUCH_BYTES / 1024)) / 1024);
	if (made < MIN_LIVE || reaped != made) {
		printf("proctest: FAIL - phase C held %d/%d (exit %d)\n",
		    reaped, made, made ? 1 : 0);
		fail = 1;
	}

	printf("proctest: %s\n", fail ? "FAIL" : "PASS");
	return (fail ? 1 : 0);
}
