/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * vfstest.c - S402 acceptance: VFS transparent forwarding to user-space
 *             fs_server via ordinary open/read/write/close on /srv/ paths.
 * vfstest.c - S402 验收：通过普通 open/read/write/close 访问 /srv/ 路径，
 *             VFS 透明转发到用户态 fs_server。
 *
 * 中文：与 S400 srvtest 不同，这里用标准 libc open/read/write/close（不调用
 * srv_* API），证明 /srv/ 转发对用户完全透明。libc 检测 /srv/ 前缀后经 IPC
 * 发给用户态 fs_server（虚拟 fd 用 0x4000 基址），像普通 syscall 一样返回
 * fd/字节数；内核 VFS 零改动。同时验证服务退出后 /srv/ 请求返回 EIO
 * （故障隔离）与重启恢复。
 * EN: Unlike S400 srvtest, this uses standard libc open/read/write/close (no
 * srv_* API), proving the /srv/ forwarding is fully transparent to callers.
 * libc detects the /srv/ prefix and sends IPC to the user-space fs_server
 * (virtual fds based at 0x4000), returning fd/count like a normal syscall;
 * the kernel VFS is untouched.  Also verifies EIO after service exit and
 * restart recovery.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <srv.h>

static int
fail(const char *what)
{
	printf("vfs: %s FAILED\n", what);
	return (1);
}

static void
delay(unsigned long n)
{
	volatile unsigned long i;

	for (i = 0; i < n; i++)
		;
}

/* 启动 fs_server（复用 S400 服务，端口 0/1）*/
static int
start_server(void)
{
	int pid;
	char *av[2], *ev[1];

	pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		av[0] = "/bin/fssrv";
		av[1] = 0;
		ev[0] = 0;
		execve("/bin/fssrv", av, ev);
		_exit(127);
	}
	return (pid);
}

/* 受控关停 fs_server：发 SRV_SHUTDOWN（直接 IPC，不经过 VFS）*/
#define SYS_SEND 454
static int
syscall4(int n, int a, int b, int c, int d)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r)
	    : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
	return (r);
}
static void
shutdown_server(void)
{
	struct srv_msg req;

	memset(&req, 0, sizeof(req));
	req.type = SRV_SHUTDOWN;
	req.len = sizeof(struct srv_args);
	syscall4(SYS_SEND, SRV_PORT, (int)&req, 0, 0);
}

int
main(void)
{
	int srv, bad = 0, st, fd;
	char buf[64];

	srv = start_server();
	if (srv < 0)
		return (fail("fork server"));
	delay(8000000ul);	/* 等服务 fork/exec + 注册两个端口 / wait for service */

	/* ---- 1: 透明 open + write + read（标准 libc API）---- */
	fd = open("/srv/vfstest", 0);
	printf("vfs: open fd=%d\n", fd);
	if (fd < 0) {
		bad += fail("open /srv/");
	} else {
		int nw, nr;

		nw = write(fd, "transparent", 11);
		printf("vfs: write n=%d\n", nw);
		if (nw != 11)
			bad += fail("write /srv/");
		/* 中文：fs_server 的 CLOSE 语义是删除文件（tmpfs close=unlink），
		 * 故用同一个 fd 直接读回——SRV_READ 每次从偏移 0 返回全部数据。
		 * EN: fs_server's CLOSE deletes the file (tmpfs close=unlink), so read
		 * back through the SAME fd — SRV_READ always returns data from offset 0. */
		memset(buf, 0, sizeof(buf));
		nr = read(fd, buf, 11);
		printf("vfs: read n=%d buf=[%s]\n", nr, buf);
		if (nr != 11 || strcmp(buf, "transparent") != 0)
			bad += fail("read /srv/");
		close(fd);
	}
	if (bad == 0)
		printf("vfs: transparent roundtrip ok\n");

	/* ---- 2: 故障隔离——服务退出后 open /srv/ 返回 EIO ---- */
	shutdown_server();
	wait4(srv, &st, 0, 0);
	delay(1000000ul);
	errno = 0;
	fd = open("/srv/aftercrash", 0);
	if (fd != -1 || errno != EIO) {		/* libc: -1 + errno=EIO */
		printf("vfs: aftercrash fd=%d errno=%d\n", fd, errno);
		bad += fail("crash -> EIO");
	} else {
		printf("vfs: fault isolation ok (EIO)\n");
	}

	/* ---- 3: 重启恢复 ---- */
	srv = start_server();
	if (srv < 0)
		return (fail("restart fork"));
	delay(6000000ul);
	fd = open("/srv/again", 1);
	if (fd < 0) {
		bad += fail("open after restart");
	} else {
		write(fd, "ok", 2);
		close(fd);
	}
	shutdown_server();
	wait4(srv, &st, 0, 0);
	printf("vfs: restart ok\n");

	/* ---- 4: close 完整转发验证——服务端槽位确实被释放 ---- */
	/* 中文：fs_server 的 SRV_CLOSE 会释放服务端文件槽位 files[].used=0，
	 * 服务端最多 NFILES=16 个槽位。若内核 close 是完整转发（把 SRV_CLOSE
	 * 发给 fs_server），每次 open/close 循环都会复用同一个服务端槽位，20 次
	 * 不同文件名的 open 全部成功；若 close 只是旧的"本地假释放"，服务端槽位
	 * 不回收，第 17 次 open 会因 16 个槽位占满返回 -ENOSPC(28)。以此明确
	 * 判定 close 是否真正走到内核态转发，而非本地释放 fd。 */
	{
		static const char digits[] = "0123456789abcdefghij";
		char cpath[] = "/srv/closeX";
		int i, cd = 0, cfd;

		srv = start_server();	/* 干净服务，槽位从 0 开始 */
		if (srv < 0)
			return (fail("close-proof restart"));
		delay(6000000ul);

		for (i = 0; i < 20; i++) {
			cpath[10] = digits[i];
			cfd = open(cpath, 0);
			if (cfd < 0) {
				printf("vfs: close-proof open %d errno=%d\n", i, errno);
				cd += fail("close forward (server slot leak)");
				break;
			}
			write(cfd, "x", 1);
			close(cfd);
		}
		if (cd == 0)
			printf("vfs: close forward ok (20 open/close, no slot leak)\n");
		bad += cd;

		shutdown_server();
		wait4(srv, &st, 0, 0);
	}

	printf("vfs: %s\n", bad ? "FAIL" : "PASS");
	return (bad ? 1 : 0);
}
