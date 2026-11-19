/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * kipc.c - 内核 IPC 原语：KP-4 阻塞/唤醒、KP-6 消息收发、KP-7 共享页映射（S300）
 * kipc.c - Kernel IPC primitives: KP-4 block/wakeup, KP-6 send/recv,
 *          KP-7 shared page mapping (S300).
 *
 * 本文件基于已有的同步层（sync.c 的 waitq）与 usermode.c 的页表桥接函数实现，
 * 全部为可移植 C，不含任何汇编（遵守 asmble.md：汇编只能在 arch/ 下）。
 * Built on the existing sync layer (waitq in sync.c) and page-table bridges
 * in usermode.c.  Pure C, no assembly (asmble.md: asm only under arch/).
 *
 * 默认不激活：没有服务注册端口、没有 syscall 调用时，现有测试行为完全不变。
 * Inert by default: existing tests are unchanged until a port is registered
 * or a syscall actually invokes these primitives.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/param.h>	/* NBPG / PGOFSET */

#include "sys/smp.h"
#include "sys/sync.h"
#include "sys/kipc.h"

extern void printf(const char *, ...);

/* ------------------------------------------------------------------ */
/* KP-4: 通用阻塞通道 / generic blocking channels                      */
/* ------------------------------------------------------------------ */
static struct waitq chanq[NCHANHASH];
static int chanq_inited = 0;

/* 中文：初始化通道哈希表，每个桶是一个 waitq */
/* EN: Initialize the channel hash table; each bucket is a waitq. */
static void
chanq_init(void)
{
	int i;

	if (chanq_inited)
		return;
	for (i = 0; i < NCHANHASH; i++)
		waitq_init(&chanq[i], "chanq");
	chanq_inited = 1;
}

/* 中文：把任意通道地址哈希到固定的 waitq 桶 */
/* EN: Hash an arbitrary channel address to a fixed waitq bucket. */
static struct waitq *
chan_hash(void *chan)
{

	chanq_init();
	return (&chanq[((unsigned)chan >> 2) & (NCHANHASH - 1)]);
}

/* 中文：在通道 chan 上阻塞当前线程，直到 wakeup(chan) 唤醒 */
/* EN: Block the current thread on channel chan until wakeup(chan). */
void
block_on(void *chan)
{

	if (chan == 0)
		return;
	waitq_sleep(chan_hash(chan));
}

/* 中文：唤醒阻塞在通道 chan 上的所有线程 */
/* EN: Wake all threads blocked on channel chan. */
void
wakeup(void *chan)
{

	if (chan == 0)
		return;
	waitq_wakeall(chan_hash(chan));
}

/* 中文：唤醒阻塞在通道 chan 上的一个线程 */
/* EN: Wake one thread blocked on channel chan. */
void
wakeup_one(void *chan)
{

	if (chan == 0)
		return;
	waitq_wakeone(chan_hash(chan));
}

/* ------------------------------------------------------------------ */
/* KP-6: 端口与消息 / ports and messages                               */
/* ------------------------------------------------------------------ */
static struct ipc_port ports[NPORT];
static struct ipc_slot slots[NPORT * 4];	/* 每端口 4 个有界槽位 */
static int ports_inited = 0;

/* 中文：初始化端口表与消息槽位池（一次性） */
/* EN: Initialize the port table and the bounded message-slot pool (once). */
static void
ports_init(void)
{
	int i;

	if (ports_inited)
		return;
	for (i = 0; i < NPORT; i++) {
		spin_init(&ports[i].p_lock);
		ports[i].p_inited = 1;
		ports[i].p_owner = 0;
		ports[i].p_qhead = 0;
		ports[i].p_qtail = 0;
		ports[i].p_nmsg = 0;
		waitq_init(&ports[i].p_recvq, "portrecv");
	}
	for (i = 0; i < NPORT * 4; i++)
		slots[i].used = 0;
	ports_inited = 1;
}

/*
 * 中文：查询端口属主 pid（0 表示无属主/服务已退出）。内核态 VFS 转发在发送
 * 请求前用它判断服务是否存活，避免向已死服务端口发请求后永久阻塞。
 * EN: Return a port's owner pid (0 = no owner / service gone).  Kernel-side VFS
 * forwarding checks it before sending so a dead service yields EIO instead of
 * a permanent block.
 */
int
port_owner(unsigned port)
{

	if (port >= NPORT)
		return (-EINVAL);
	ports_init();
	return ((int)ports[port].p_owner);
}

/* 中文：为当前进程注册占用一个端口；已被其他进程占用返回 -EBUSY */
/* EN: Claim a port for the current process; -EBUSY if another owns it. */
int
port_register(unsigned port)
{
	unsigned pid;

	if (port >= NPORT)
		return (-EINVAL);
	ports_init();
	pid = proc_cur_pid();
	spin_lock(&ports[port].p_lock);
	if (ports[port].p_owner != 0 && ports[port].p_owner != pid) {
		spin_unlock(&ports[port].p_lock);
		return (-EBUSY);
	}
	ports[port].p_owner = pid ? pid : 1;	/* 内核上下文用 pid 1 占位 */
	spin_unlock(&ports[port].p_lock);
	return (0);
}

/* 中文：释放端口，唤醒所有在该端口接收上等待的线程（故障隔离基石） */
/* EN: Release a port and wake all receivers waiting on it (fault isolation). */
int
port_release(unsigned port)
{
	int i;

	if (port >= NPORT)
		return (-EINVAL);
	ports_init();
	spin_lock(&ports[port].p_lock);
	ports[port].p_owner = 0;
	/* 回收以本端口为回复端口的消息槽位 */
	for (i = 0; i < NPORT * 4; i++)
		if (slots[i].used && slots[i].m.reply_port == port)
			slots[i].used = 0;
	spin_unlock(&ports[port].p_lock);
	waitq_wakeall(&ports[port].p_recvq);
	/* 中文：S400 故障隔离——唤醒所有阻塞在该端口的用户接收者并返回 -EIO */
	/* EN: S400 fault isolation - fail all user receivers blocked on this port */
	ipc_fail_waiters(port);
	return (0);
}

/* 中文：从有界槽位池分配一个消息槽，池满返回 0 */
/* EN: Allocate a message slot from the bounded pool; 0 if exhausted. */
static struct ipc_slot *
slot_alloc(void)
{
	int i;

	for (i = 0; i < NPORT * 4; i++)
		if (!slots[i].used) {
			slots[i].used = 1;
			slots[i].next = 0;
			return (&slots[i]);
		}
	return (0);
}

/*
 * 中文：会合式发送——把消息入队并唤醒等待的接收者；内核填写发送方 pid。
 * send 本身不阻塞，调用方如需回复可在 reply_port 上再 recv，
 * 这样单线程调用者不会在 send 内死锁。
 * EN: Rendezvous send - enqueue the message and wake a waiting receiver;
 * the kernel stamps the sender pid.  send itself never blocks; a caller
 * expecting a reply can recv on reply_port, avoiding self-deadlock.
 */
int
ipc_send(unsigned port, struct ipc_msg *msg)
{
	struct ipc_port *p;
	struct ipc_slot *s;
	unsigned pid;

	if (port >= NPORT || msg == 0)
		return (-EINVAL);
	if (msg->len > IPC_INLINE_MAX)
		return (-EINVAL);
	ports_init();
	p = &ports[port];
	pid = proc_cur_pid();

	s = slot_alloc();
	if (s == 0)
		return (-EAGAIN);
	s->m = *msg;
	s->m.from = pid ? pid : 1;	/* 内核填发送方 pid */

	spin_lock(&p->p_lock);
	if (p->p_qtail != 0)
		p->p_qtail->next = s;
	else
		p->p_qhead = s;
	p->p_qtail = s;
	p->p_nmsg++;
	spin_unlock(&p->p_lock);

	waitq_wakeone(&p->p_recvq);	/* 会合：唤醒一个接收者 */
	return (0);
}

/*
 * 中文：接收一条消息（纯非阻塞原语）。队列空返回 -EAGAIN，端口无属主返回
 * -EIO。真正的阻塞由 usermode.c 的 SYS_IPC_RECV syscall 层完成：它在收到
 * EAGAIN 后保存 regs、登记 uk_ipc_waiting 并 proc_sched()，由 send / 端口
 * 释放路径投递并唤醒。分层让"队列操作"与"上下文切换"各司其职，语义清晰。
 * EN: Receive one message (purely non-blocking).  Empty queue -> -EAGAIN,
 * no owner -> -EIO.  Real blocking lives in the SYS_IPC_RECV syscall layer in
 * usermode.c: on EAGAIN it saves regs, records uk_ipc_waiting and calls
 * proc_sched(); the send / port-release path delivers and wakes.  The split
 * keeps queue handling and context switching cleanly separated.
 */
int
ipc_recv(unsigned port, struct ipc_msg *msg)
{
	struct ipc_port *p;
	struct ipc_slot *s;

	if (port >= NPORT || msg == 0)
		return (-EINVAL);
	ports_init();
	p = &ports[port];

	spin_lock(&p->p_lock);
	s = p->p_qhead;
	if (s != 0) {
		p->p_qhead = s->next;
		if (p->p_qhead == 0)
			p->p_qtail = 0;
		p->p_nmsg--;
		spin_unlock(&p->p_lock);
		*msg = s->m;
		s->used = 0;
		return (0);
	}
	if (p->p_owner == 0) {
		spin_unlock(&p->p_lock);
		return (-EIO);
	}
	spin_unlock(&p->p_lock);
	return (-EAGAIN);
}

/* 中文：进程退出时关闭其所有端口并唤醒等待者 */
/* EN: On process exit, close all its ports and wake the waiters. */
void
ipc_proc_exit(unsigned pid)
{
	int i;

	ports_init();
	for (i = 0; i < NPORT; i++)
		if (ports[i].p_owner == pid)
			port_release((unsigned)i);
}

/* ------------------------------------------------------------------ */
/* KP-7: 共享物理页 / shared physical pages                            */
/* ------------------------------------------------------------------ */
/*
 * 中文：把物理页 pa 映射到进程 pid 的用户虚地址 va。
 * pa 由生产者用 vfs_page_alloc 分配；消费者读取后发 ACK，
 * 生产者收到 ACK 才释放（谁分配谁释放 + ACK 协议）。
 * EN: Map physical page pa into process pid at user VA va.  pa is allocated
 * by the producer via vfs_page_alloc; the consumer ACKs after reading, and
 * only then does the producer free it (allocatee-frees + ACK protocol).
 */
int
map_shared(unsigned pid, unsigned va, unsigned pa, int prot)
{
	unsigned pdpa;

	if (pid == 0 || pa == 0 || (va & PGOFSET) != 0)
		return (-EINVAL);
	pdpa = proc_pd_by_pid(pid);
	if (pdpa == 0)
		return (-ESRCH);
	user_map_shared(pdpa, va, pa, prot);
	return (0);
}

/* 中文：解除进程 pid 中用户虚地址 va 的共享页映射 */
/* EN: Unmap the shared page at user VA va in process pid. */
int
unmap_shared(unsigned pid, unsigned va)
{
	unsigned pdpa;

	if (pid == 0 || (va & PGOFSET) != 0)
		return (-EINVAL);
	pdpa = proc_pd_by_pid(pid);
	if (pdpa == 0)
		return (-ESRCH);
	user_unmap_shared(pdpa, va);
	return (0);
}

/*
 * 中文：把物理页映射到内核临时窗口，供生产者在发送前填充数据；
 * 下次调用会复用同一窗口，故调用方须先发送再覆盖。
 * EN: Map a physical page into a kernel scratch window so the producer can
 * fill it before sending.  The window is reused on the next call, so the
 * caller must send before overwriting it.
 */
void *
shared_page_window(unsigned pa)
{

	/* 中文：经 usermode.c 桥接把物理页映射到内核 scratch 窗口。
	 * EN: Bridge through usermode.c to map the physical page into the
	 * kernel scratch window (map_page is static there). */
	return (user_scratch_window(pa));
}

/* 中文：生产者释放共享物理页（vfs_page_free 的包装） */
/* EN: Producer frees a shared physical page (wrapper over vfs_page_free). */
void
shared_page_release(unsigned pa)
{

	if (pa != 0)
		vfs_page_free(pa);
}

/* ------------------------------------------------------------------ */
/* 诊断与自检 / diagnostics and self test                              */
/* ------------------------------------------------------------------ */
/*
 * 中文：自检 KP-4/6/7 的非阻塞路径——通道哈希稳定、空队列唤醒安全、
 * 端口注册/收发/释放、关闭端口 recv 返回 EIO、共享页参数校验。
 * 不在 monitor 上下文真正阻塞，避免挂死。
 * EN: Self-test the non-blocking paths of KP-4/6/7: stable channel hash,
 * safe wake of empty queues, port register/send/recv/release, EIO on a
 * closed port, and shared-page argument validation.  It never blocks in the
 * monitor context, so it cannot hang.
 */
int
ipc_selftest(void)
{
	struct ipc_msg m, out;
	struct waitq *wq;
	int ok = 1;

	ports_init();
	chanq_init();

	wq = chan_hash((void *)0x1234);
	if (chan_hash((void *)0x1234) != wq)
		ok = 0;
	wakeup((void *)0x1234);
	wakeup_one((void *)0x1234);

	if (port_register(7) != 0)
		ok = 0;
	m.type = 0xABCD;
	m.len = 5;
	m.pa = 0;
	m.reply_port = 0;
	bcopy("hello", m.inline_data, 6);
	if (ipc_send(7, &m) != 0)
		ok = 0;
	if (ipc_recv(7, &out) != 0)
		ok = 0;
	if (out.type != 0xABCD || out.len != 5 ||
	    bcmp(out.inline_data, "hello", 5) != 0)
		ok = 0;
	if (port_release(7) != 0)
		ok = 0;
	if (ipc_recv(7, &out) != -EIO)
		ok = 0;

	/* 中文：非法参数必须被拒绝 */
	/* EN: invalid arguments must be rejected */
	if (map_shared(0, 0x1000, 0x200000, 0) != -EINVAL)
		ok = 0;
	if (unmap_shared(0, 0x1000) != -EINVAL)
		ok = 0;

	/*
	 * 中文：KP-7 物理页通路自检——分配页、经内核窗口写入读回、释放。
	 * 这里刻意不修改用户页表：monitor 运行在 init 进程上下文，随便挑一个
	 * 用户 VA 可能撞上已有映射，map_shared 会覆盖它而破坏系统。真实的用户态
	 * 映射由用户测试程序在专属地址上验证（见 ipctest 用户态用例）。
	 * EN: KP-7 physical-page path self-test - allocate a page, write/read
	 * via the kernel window, then free.  We deliberately do not touch the
	 * user page table here: the monitor runs in the init process context and
	 * an arbitrary user VA may collide with a live mapping, so map_shared
	 * would clobber it.  Real user-space mapping is covered by the userland
	 * test on a dedicated address.
	 */
	{
		unsigned pa = vfs_page_alloc();
		char *win;

		if (pa == 0) {
			ok = 0;
		} else {
			win = (char *)shared_page_window(pa);
			if (win == 0) {
				ok = 0;
			} else {
				win[0] = 0x5A;
				win = (char *)shared_page_window(pa);
				if (win == 0 || win[0] != 0x5A)
					ok = 0;
			}
			shared_page_release(pa);
		}
	}

	return (ok);
}

/* 中文：打印当前端口占用情况与消息数（monitor ipcinfo 命令） */
/* EN: Print current port ownership and message counts (monitor ipcinfo). */
void
ipc_print(void)
{
	int i, used = 0;

	ports_init();
	for (i = 0; i < NPORT; i++)
		if (ports[i].p_owner != 0)
			used++;
	printf("ipc: ports %d/%d in use\n", used, NPORT);
	for (i = 0; i < NPORT; i++)
		if (ports[i].p_owner != 0)
			printf("ipc: port %d owner=%u msgs=%u\n",
			    i, ports[i].p_owner, ports[i].p_nmsg);
}
