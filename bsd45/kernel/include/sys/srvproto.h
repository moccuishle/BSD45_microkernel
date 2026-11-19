/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * srvproto.h - kernel-side constants for the user-space fs_server protocol,
 *              used by the VFS transparent-forwarding layer (KVFS).
 * srvproto.h - 内核侧 fs_server 协议常量，供 VFS 透明转发层（KVFS）使用。
 *
 * 中文：这些常量必须与用户态 libc/include/srv.h 保持一致。消息布局直接复用
 * struct ipc_msg（5×u32 + inline_data[64] = 84 字节），args 在 inline_data
 * 前 8 字节。内核不 include 用户态 srv.h，故在此镜像关键常量。
 * EN: These constants MUST match user-space libc/include/srv.h.  The message
 * layout reuses struct ipc_msg (5×u32 + inline_data[64] = 84 bytes); args live
 * in the first 8 inline bytes.  The kernel does not include user srv.h, so the
 * key constants are mirrored here.
 */
#ifndef _SYS_SRVPROTO_H_
#define _SYS_SRVPROTO_H_

#define SRV_PORT_KERNEL		0	/* 请求端口 / request port */
#define SRV_REPLY_PORT_KERNEL	1	/* 回复端口 / reply port */

#define SRV_OPEN_KERNEL		1
#define SRV_READ_KERNEL		2
#define SRV_WRITE_KERNEL	3
#define SRV_CLOSE_KERNEL	4
#define SRV_STAT_KERNEL		5
#define SRV_SHUTDOWN_KERNEL	6

#define SRV_DATA_MAX_KERNEL	56	/* inline_data 数据区上限 / payload cap */

/* inline_data 前 8 字节参数块布局 / arg block at first 8 inline bytes */
struct srv_args_kernel {
	unsigned	handle;
	unsigned	arg;
};

/* 虚拟 fd 基址：内核 fd 用 0..NFD-1，/srv/ 虚拟 fd 从 0x4000 起，避免冲突。
 * Virtual fd base: kernel fds are 0..NFD-1; /srv/ fds start at 0x4000. */
#define KVFS_FD_BASE		0x4000
#define KVFS_FD_MAX		8

#endif /* _SYS_SRVPROTO_H_ */
