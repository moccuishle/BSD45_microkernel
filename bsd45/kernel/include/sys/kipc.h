/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * kipc.h - 内核 IPC、阻塞通道与共享页（S300）
 * kipc.h - BSD45 kernel IPC, blocking channels and shared pages (S300).
 *
 * 实现 docs/microkernel-design.md 中缺失的三个原语：
 *   KP-4  block_on() / wakeup()   在等待通道上的通用阻塞
 *   KP-6  send() / recv()         基于端口的同步会合消息
 *   KP-7  map_shared()            共享物理页映射到目标进程
 * Implements the three missing primitives from microkernel-design.md:
 *   KP-4 generic blocking on a wait channel
 *   KP-6 synchronous rendezvous messages via ports
 *   KP-7 shared physical page mapping into a target process
 *
 * 全部为可移植 C；特权操作经 arch/ 已有接口完成，arch/ 之外无汇编（asmble.md）。
 * Pure C; privileged work goes through existing arch/ interfaces, no assembly
 * outside arch/ (asmble.md).  Named kipc.h to avoid the legacy BSD ipc.h.
 */
#ifndef _SYS_KIPC_H_
#define _SYS_KIPC_H_

#ifndef ASSEMBLER

#include "sys/sync.h"		/* struct waitq */
#include "sys/smp.h"		/* struct spinlock */

/* ------------------------------------------------------------------ */
/* KP-4: 通用阻塞通道 / generic blocking channels                      */
/* ------------------------------------------------------------------ */
#define NCHANHASH	32	/* 通道哈希桶数量 / number of channel buckets */

/* 中文：在通道 chan 上阻塞，直到 wakeup(chan) */
/* EN: Block on channel chan until wakeup(chan). */
void	block_on(void *chan);
/* 中文：唤醒通道 chan 上的所有等待者 */
/* EN: Wake all sleepers on channel chan. */
void	wakeup(void *chan);
/* 中文：唤醒通道 chan 上的一个等待者 */
/* EN: Wake one sleeper on channel chan. */
void	wakeup_one(void *chan);

/* ------------------------------------------------------------------ */
/* KP-6: 端口与消息 / ports and messages                               */
/* ------------------------------------------------------------------ */
#define NPORT		16	/* 固定端口表大小 / fixed port table size */

#define PORT_FS		0	/* 文件系统服务 / filesystem service */
#define PORT_TTY	1	/* 终端行规程服务 / tty line discipline */
#define PORT_BDEV	2	/* 块设备代理 / block-device proxy */
#define PORT_KERNEL	15	/* 内核使用的回复端口 / kernel reply port */

#define IPC_INLINE_MAX	64	/* 消息内联长度上限 / inline payload limit */

/* 中文：一条 IPC 消息；大块数据走 pa 指向的共享页 */
/* EN: An IPC message; bulk data travels via the shared page in pa. */
struct ipc_msg {
	unsigned	from;		/* 发送方 pid（内核填）/ sender pid (kernel fills) */
	unsigned	type;		/* 服务自定义类型 / service-defined type */
	unsigned	len;		/* 内联字节数 / inline byte count */
	unsigned	pa;		/* 共享页物理地址，0 表示无 / shared page PA */
	unsigned	reply_port;	/* 回复端口号，0 表示无 / reply port */
	char		inline_data[IPC_INLINE_MAX];
};

/* 中文：端口有界队列中的一个消息槽 */
/* EN: One message slot in a port's bounded queue. */
struct ipc_slot {
	struct ipc_msg	 m;
	struct ipc_slot	*next;
	int		 used;
};

/* 中文：一个端口，含属主、接收等待队列与消息链表 */
/* EN: A port: owner, receive wait queue, and message list. */
struct ipc_port {
	struct spinlock	 p_lock;	/* 保护字段的自旋锁 / field guard */
	int		 p_inited;
	unsigned	 p_owner;	/* 属主 pid，0 表示空闲 / owner pid, 0 free */
	struct waitq	 p_recvq;	/* 接收者等待队列 / receiver wait queue */
	struct ipc_slot	*p_qhead;
	struct ipc_slot	*p_qtail;
	unsigned	 p_nmsg;
};

/* 中文：注册/释放端口；收发消息；成功返回 0，失败返回负 errno */
/* EN: Register/release a port; send/recv messages; 0 or negative errno. */
int	port_register(unsigned port);
int	port_release(unsigned port);
/* 中文：查询端口属主 pid（0=无属主），供内核态 VFS 转发判断服务存活 */
/* EN: Query a port's owner pid (0 = none) for kernel-side VFS liveness checks. */
int	port_owner(unsigned port);
int	ipc_send(unsigned port, struct ipc_msg *msg);
int	ipc_recv(unsigned port, struct ipc_msg *msg);
/* 中文：进程退出时关闭其所有端口 / Close all ports owned by an exiting process. */
void	ipc_proc_exit(unsigned pid);
/* 中文：S400 会合——端口释放时给所有阻塞在该端口的用户接收者返回 -EIO。
 * 由 usermode.c 实现（它能访问 proc 表与 proc_wake），kipc.c 的 port_release
 * 与 ipc_proc_exit 调用它完成故障隔离。
 * EN: S400 rendezvous - on port release, fail all user receivers blocked on
 * that port with -EIO.  Implemented in usermode.c (has proc table + proc_wake);
 * called by kipc.c port_release / ipc_proc_exit for fault isolation. */
void	ipc_fail_waiters(unsigned port);

/* ------------------------------------------------------------------ */
/* KP-7: 共享物理页 / shared physical pages                            */
/* ------------------------------------------------------------------ */
/* 中文：把物理页 pa 映射到进程 pid 的 va；解除映射；内核窗口访问；释放页 */
/* EN: Map physical page pa into process pid at va; unmap; kernel window
 * access for the producer; free the page. */
int	map_shared(unsigned pid, unsigned va, unsigned pa, int prot);
int	unmap_shared(unsigned pid, unsigned va);
void	*shared_page_window(unsigned pa);
void	shared_page_release(unsigned pa);

/* 中文：指向 usermode.c 中静态页表帮助函数的桥接 */
/* EN: Bridges to the static page-table helpers in usermode.c. */
unsigned	proc_pd_by_pid(unsigned pid);
unsigned	proc_cur_pid(void);
void		user_map_shared(unsigned pdpa, unsigned va, unsigned pa, int prot);
void		user_unmap_shared(unsigned pdpa, unsigned va);
void		*user_scratch_window(unsigned pa);
unsigned	vfs_page_alloc(void);
void		vfs_page_free(unsigned pa);

/* 中文：诊断与自检 / diagnostics and self test */
int	ipc_selftest(void);
void	ipc_print(void);

/* 中文：syscall 编号，接在现有最大 451 之后 */
/* EN: syscall numbers, continuing after the current max of 451. */
#define SYS_IPC_PORT_REGISTER	452
#define SYS_IPC_PORT_RELEASE	453
#define SYS_IPC_SEND		454
#define SYS_IPC_RECV		455
#define SYS_IPC_MAP_SHARED	456
#define SYS_IPC_UNMAP_SHARED	457
/* 中文：分配/释放一页共享物理内存，返回物理地址（供用户态测试与服务端使用） */
/* EN: Allocate/free a shared physical page; returns PA (for tests/services). */
#define SYS_IPC_PAGE_ALLOC	458
#define SYS_IPC_PAGE_FREE	459

#endif /* !ASSEMBLER */
#endif /* _SYS_KIPC_H_ */
