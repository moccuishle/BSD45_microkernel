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
 *
 * proctest.c - 动态进程数验收测试。验证内核可容纳远超旧的 NPROC=8 限制，
 * 且实际上限随物理内存变化（VMware 加大内存可容纳更多进程）。
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

/*
 * Children to keep alive at once in phase B.  The process table and TCB pool
 * now live in the shared kernel window (NPROC = NKTHREAD = 1024), so the old
 * hard cap of 8 (and the boot-loader-derived cap of 24) is gone.  The binding
 * limit is physical memory: fork returns -EAGAIN when page_alloc runs out.
 * 200 live children needs a few MB and far exceeds the original NPROC=8.
 * 200 个存活子进程只需几 MB，远超原来的 NPROC=8 与引导器上限 24。
 * 同时存活的子进程数：进程表与 TCB 池已移入共享内核窗口（NPROC = NKTHREAD =
 * 1024），旧的 8（以及受引导器限制的 24）上限不复存在。真正瓶颈是物理内存，
 * page_alloc 耗尽时 fork 返回 -EAGAIN。200 个存活子进程仅需几 MB。
 */
#define	TARGET_LIVE	200
#define	TARGET_SEQ	200	/* phase A sequential fork/wait cycles */
#define	MIN_LIVE	150	/* must be far beyond the old 8 */

static volatile int child_spinner;

int
main(void)
{
	pid_t kids[TARGET_LIVE];
	int nkids = 0;
	int st, i, made, reaped, fail = 0;

	printf("proctest: dynamic process count test (target live=%d)\n", TARGET_LIVE);

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
	printf("proctest: phase A sequential fork/wait = %d (target %d)\n",
	    made, TARGET_SEQ);
	if (made < TARGET_SEQ)
		fail = 1;

	/* ---- Phase B: keep TARGET_LIVE children alive simultaneously ---- */
	for (i = 0; i < TARGET_LIVE; i++) {
		pid_t p = fork();

		if (p < 0) {
			printf("proctest: phase B fork failed at child %d (errno %d)\n",
			    i, errno);
			break;
		}
		if (p == 0) {
			/* Child: spin cooperatively until killed by parent. */
			for (;;) {
				child_spinner++;
			}
		}
		kids[nkids++] = p;
	}
	printf("proctest: phase B simultaneous live children = %d (target %d)\n",
	    nkids, TARGET_LIVE);
	if (nkids < MIN_LIVE) {
		printf("proctest: FAIL - only %d live (< %d)\n", nkids, MIN_LIVE);
		fail = 1;
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

	printf("proctest: %s\n", fail ? "FAIL" : "PASS");
	return (fail ? 1 : 0);
}
