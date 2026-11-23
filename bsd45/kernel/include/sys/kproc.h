/*
 * Copyright (c) 2026 jialiang.  All rights reserved.
 *
 * kproc.h - types for the kernel-mode process model.  A struct process owns its
 * address space, resources and a list of kernel threads (KLT); threads, not
 * processes, are the unit of scheduling, and all threads of one process share
 * its page directory.
 *
 * The runtime field definitions live directly in struct process (kprocess.h) so
 * usermode.c uses struct process directly.  This header provides the
 * supporting types for the kernel process model.
 */
#ifndef _SYS_KPROC_H_
#define _SYS_KPROC_H_

#include "sys/platform.h"		/* struct sysregs */

struct kthread;
struct vnode;

#define	KPROC_NFD	16
#define	KPROC_NMAP	8
#define	KPROC_NSIG	32
#define	KPROC_MAXCOMMLEN	16
#define	KPROC_CWDLEN	128

/* process states (kernel process model) */
#define	KP_FREE		0
#define	KP_RUN		1
#define	KP_BLOCKED	2
#define	KP_ZOMBIE	3
#define	KP_STOPPED	4

/* fd kinds */
#define	KFD_FILE	0
#define	KFD_DIR		1
#define	KFD_PIPE	2
#define	KFD_TMPFILE	3
#define	KFD_TMPDIR	4
#define	KFD_CONSOLE	5
#define	KFD_NULL	6
#define	KFD_ZERO	7
#define	KFD_MOUNTS	8
#define	KFD_PS		9
#define	KFD_VMSTAT	10

struct kfd {
	const unsigned char	*d;
	unsigned		len;
	unsigned		off;
	int			used;
	int			type;
	unsigned		lba;
	unsigned		dsize;
	unsigned		dpos;
	int			pipeidx;
	int			rw;
	int			node;
	struct vnode		*vp;
};

struct kvma {
	int		used;
	unsigned	start;
	unsigned	len;
	unsigned	off;
	unsigned	vid;
	struct vnode	*vp;
	int		prot;
	int		flags;
};

#endif /* !_SYS_KPROC_H_ */
