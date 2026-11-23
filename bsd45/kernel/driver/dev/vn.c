/*
 * Copyright (c) 1988 University of Utah.
 * Copyright (c) 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * the Systems Programming Group of the University of Utah Computer
 * Science Department.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * from: Utah $Hdr: vn.c 1.13 94/04/02$
 *
 *	@(#)vn.c	8.9 (Berkeley) 5/14/95
 */

/*
 * Vnode disk driver.
 *
 * Block/character interface to a vnode.  Allows one to treat a file
 * as a disk (e.g. build a filesystem in it, mount it, etc.).
 *
 * NOTE 1: This uses the VOP_BMAP/VOP_STRATEGY interface to the vnode
 * instead of a simple VOP_RDWR.  We do this to avoid distorting the
 * local buffer cache.
 *
 * NOTE 2: There is a security issue involved with this driver.
 * Once mounted all access to the contents of the "mapped" file via
 * the special file is controlled by the permissions on the special
 * file, the protection of the mapped file is ignored (effectively,
 * by using root credentials in all transactions).
 *
 * NOTE 3: Doesn't interact with leases, should it?
 */
#include "vn.h"
#if NVN > 0

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/kprocess.h>
#include <sys/errno.h>
#include <sys/dkstat.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/file.h>
#include <sys/uio.h>

#include <miscfs/specfs/specdev.h>

#include <dev/vnioctl.h>

#ifdef DEBUG
int dovncluster = 1;
int vndebug = 0x00;
#define VDB_FOLLOW	0x01
#define VDB_INIT	0x02
#define VDB_IO		0x04
#endif

#define b_cylin	b_resid

#define	vnunit(x)	((minor(x) >> 3) & 0x7)	/* for consistency */

#define	getvnbuf()	\
	((struct buf *)malloc(sizeof(struct buf), M_DEVBUF, M_WAITOK))
#define putvnbuf(bp)	\
	free((caddr_t)(bp), M_DEVBUF)

struct vn_softc {
	int		 sc_flags;	/* flags */
	size_t		 sc_size;	/* size of vn */
	struct vnode	*sc_vp;		/* vnode */
	struct ucred	*sc_cred;	/* credentials */
	int		 sc_maxactive;	/* max # of active requests */
	struct buf	 sc_tab;	/* transfer queue */
};

/* sc_flags */
#define	VNF_ALIVE	0x01
#define VNF_INITED	0x02

#if 0	/* if you need static allocation */
struct vn_softc vn_softc[NVN];
int numvnd = NVN;
#else
struct vn_softc *vn_softc;
int numvnd;
#endif

/* EN: Allocates (M_DEVBUF, M_NOWAIT) and zeroes the num-entry vn_softc array and publishes it as vn_softc/numvnd; returns immediately for num<=0 and warns instead of failing. */
/* 中文：用 M_DEVBUF/M_NOWAIT 分配并清零 num 项 vn_softc 数组，设为全局 vn_softc 与 numvnd；num<=0 直接返回，分配失败只打印警告而不报错。 */
void
vnattach(num)
	int num;
{
	char *mem;
	register u_long size;

	if (num <= 0)
		return;
	size = num * sizeof(struct vn_softc);
	mem = malloc(size, M_DEVBUF, M_NOWAIT);
	if (mem == NULL) {
		printf("WARNING: no memory for vnode disks\n");
		return;
	}
	bzero(mem, size);
	vn_softc = (struct vn_softc *)mem;
	numvnd = num;
}

/* EN: Returns ENXIO when unit >= numvnd, else 0; open does no other setup, configuration is done by the VNIOCSET ioctl. */
/* 中文：unit 超过 numvnd 时返回 ENXIO，否则返回 0；打开时不做其他初始化，实际配置由 VNIOCSET ioctl 完成。 */
int
vnopen(dev, flags, mode, p)
	dev_t dev;
	int flags, mode;
	struct process *p;
{
	int unit = vnunit(dev);

#ifdef DEBUG
	if (vndebug & VDB_FOLLOW)
		printf("vnopen(%x, %x, %x, %x)\n", dev, flags, mode, p);
#endif
	if (unit >= numvnd)
		return(ENXIO);
	return(0);
}

/*
 * Break the request into bsize pieces and submit using VOP_BMAP/VOP_STRATEGY.
 * Note that this driver can only be used for swapping over NFS on the hp
 * since nfs_strategy on the vax cannot handle u-areas and page tables.
 */
/* EN: Requires VNF_INITED (else ENXIO) and in-range blocks; splits bp at f_iosize via VOP_BMAP on sc_vp, builds getvnbuf children (sc_cred, vniodone), then disksort/vnstart at splbio. */
/* 中文：要求 VNF_INITED（否则 ENXIO）且块号在 sc_size 内；按 f_iosize 把 bp 切片，用 VOP_BMAP 在 sc_vp 上求映射，构造带 sc_cred 和 vniodone 的 getvnbuf 子缓冲，在 splbio 下 disksort 并按 sc_maxactive 调用 vnstart。 */
void
vnstrategy(bp)
	register struct buf *bp;
{
	int unit = vnunit(bp->b_dev);
	register struct vn_softc *vn = &vn_softc[unit];
	register struct buf *nbp;
	register int bn, bsize, resid;
	register caddr_t addr;
	int sz, flags, error;
	extern void vniodone();

#ifdef DEBUG
	if (vndebug & VDB_FOLLOW)
		printf("vnstrategy(%x): unit %d\n", bp, unit);
#endif
	if ((vn->sc_flags & VNF_INITED) == 0) {
		bp->b_error = ENXIO;
		bp->b_flags |= B_ERROR;
		biodone(bp);
		return;
	}
	bn = bp->b_blkno;
	sz = howmany(bp->b_bcount, DEV_BSIZE);
	bp->b_resid = bp->b_bcount;
	if (bn < 0 || bn + sz > vn->sc_size) {
		if (bn != vn->sc_size) {
			bp->b_error = EINVAL;
			bp->b_flags |= B_ERROR;
		}
		biodone(bp);
		return;
	}
	bn = dbtob(bn);
	bsize = vn->sc_vp->v_mount->mnt_stat.f_iosize;
	addr = bp->b_data;
	flags = bp->b_flags | B_CALL;
	for (resid = bp->b_resid; resid; resid -= sz) {
		struct vnode *vp;
		daddr_t nbn;
		int off, s, nra;

		nra = 0;
		error = VOP_BMAP(vn->sc_vp, bn / bsize, &vp, &nbn, &nra);
		if (error == 0 && (long)nbn == -1)
			error = EIO;
#ifdef DEBUG
		if (!dovncluster)
			nra = 0;
#endif

		if (off = bn % bsize)
			sz = bsize - off;
		else
			sz = (1 + nra) * bsize;
		if (resid < sz)
			sz = resid;
#ifdef DEBUG
		if (vndebug & VDB_IO)
			printf("vnstrategy: vp %x/%x bn %x/%x sz %x\n",
			       vn->sc_vp, vp, bn, nbn, sz);
#endif

		nbp = getvnbuf();
		nbp->b_flags = flags;
		nbp->b_bcount = sz;
		nbp->b_bufsize = bp->b_bufsize;
		nbp->b_error = 0;
		if (vp->v_type == VBLK || vp->v_type == VCHR)
			nbp->b_dev = vp->v_rdev;
		else
			nbp->b_dev = NODEV;
		nbp->b_data = addr;
		nbp->b_blkno = nbn + btodb(off);
		nbp->b_proc = bp->b_proc;
		nbp->b_iodone = vniodone;
		nbp->b_vp = vp;
		nbp->b_pfcent = (int) bp;	/* XXX */
		nbp->b_rcred = vn->sc_cred;	/* XXX crdup? */
		nbp->b_wcred = vn->sc_cred;	/* XXX crdup? */
		nbp->b_dirtyoff = bp->b_dirtyoff;
		nbp->b_dirtyend = bp->b_dirtyend;
		nbp->b_validoff = bp->b_validoff;
		nbp->b_validend = bp->b_validend;
		/*
		 * If there was an error or a hole in the file...punt.
		 * Note that we deal with this after the nbp allocation.
		 * This ensures that we properly clean up any operations
		 * that we have already fired off.
		 *
		 * XXX we could deal with holes here but it would be
		 * a hassle (in the write case).
		 */
		if (error) {
			nbp->b_error = error;
			nbp->b_flags |= B_ERROR;
			bp->b_resid -= (resid - sz);
			biodone(nbp);
			return;
		}
		/*
		 * Just sort by block number
		 */
		nbp->b_cylin = nbp->b_blkno;
		s = splbio();
		disksort(&vn->sc_tab, nbp);
		if (vn->sc_tab.b_active < vn->sc_maxactive) {
			vn->sc_tab.b_active++;
			vnstart(vn);
		}
		splx(s);
		bn += sz;
		addr += sz;
	}
}

/*
 * Feed requests sequentially.
 * We do it this way to keep from flooding NFS servers if we are connected
 * to an NFS file.  This places the burden on the client rather than the
 * server.
 */
/* EN: Pops the head buffer off sc_tab (b_actf) before submitting so the lower layer may reuse the links, bumps v_numoutput for writes, then calls VOP_STRATEGY. */
/* 中文：在下发前先从 sc_tab 的 b_actf 摘下缓冲区（避免下层复用链域），写请求时递增 v_numoutput，然后调用 VOP_STRATEGY。 */
vnstart(vn)
	register struct vn_softc *vn;
{
	register struct buf *bp;

	/*
	 * Dequeue now since lower level strategy routine might
	 * queue using same links
	 */
	bp = vn->sc_tab.b_actf;
	vn->sc_tab.b_actf = bp->b_actf;
#ifdef DEBUG
	if (vndebug & VDB_IO)
		printf("vnstart(%d): bp %x vp %x blkno %x addr %x cnt %x\n",
		       vn-vn_softc, bp, bp->b_vp, bp->b_blkno, bp->b_data,
		       bp->b_bcount);
#endif
	if ((bp->b_flags & B_READ) == 0)
		bp->b_vp->v_numoutput++;
	VOP_STRATEGY(bp);
}

/* EN: Child iodone at splbio: forwards errors to the parent bp, subtracts bcount from b_resid, frees the child buf, biodones the parent at resid 0, else vnstarts next/drops b_active. */
/* 中文：子缓冲完成回调，在 splbio 下运行：把错误复制给父 bp，从父 b_resid 减去 b_bcount，putvnbuf 释放子缓冲，resid 为 0 时 biodone 父缓冲，再 vnstart 下一个缓冲或递减 sc_tab.b_active。 */
void
vniodone(bp)
	register struct buf *bp;
{
	register struct buf *pbp = (struct buf *)bp->b_pfcent;	/* XXX */
	register struct vn_softc *vn = &vn_softc[vnunit(pbp->b_dev)];
	int s;

	s = splbio();
#ifdef DEBUG
	if (vndebug & VDB_IO)
		printf("vniodone(%d): bp %x vp %x blkno %x addr %x cnt %x\n",
		       vn-vn_softc, bp, bp->b_vp, bp->b_blkno, bp->b_data,
		       bp->b_bcount);
#endif
	if (bp->b_error) {
#ifdef DEBUG
		if (vndebug & VDB_IO)
			printf("vniodone: bp %x error %d\n", bp, bp->b_error);
#endif
		pbp->b_flags |= B_ERROR;
		pbp->b_error = biowait(bp);
	}
	pbp->b_resid -= bp->b_bcount;
	putvnbuf(bp);
	if (pbp->b_resid == 0) {
#ifdef DEBUG
		if (vndebug & VDB_IO)
			printf("vniodone: pbp %x iodone\n", pbp);
#endif
		biodone(pbp);
	}
	if (vn->sc_tab.b_actf)
		vnstart(vn);
	else
		vn->sc_tab.b_active--;
	splx(s);
}

/* EN: Thin physio() wrapper passing vnstrategy, B_READ and minphys to move a read uio through vnode-disk buffers; returns physio's error. */
/* 中文：对 physio 的薄封装，传入 vnstrategy、B_READ 和 minphys，把读 uio 经 vnode 磁盘缓冲搬运，返回 physio 的错误码。 */
vnread(dev, uio, flags, p)
	dev_t dev;
	struct uio *uio;
	int flags;
	struct process *p;
{

#ifdef DEBUG
	if (vndebug & VDB_FOLLOW)
		printf("vnread(%x, %x, %x, %x)\n", dev, uio, flags, p);
#endif
	return(physio(vnstrategy, NULL, dev, B_READ, minphys, uio));
}

/* EN: Thin physio() wrapper passing vnstrategy, B_WRITE and minphys to push a write uio through vnode-disk buffers; returns physio's error. */
/* 中文：对 physio 的薄封装，传入 vnstrategy、B_WRITE 和 minphys，把写 uio 经 vnode 磁盘缓冲下发，返回 physio 的错误码。 */
vnwrite(dev, uio, flags, p)
	dev_t dev;
	struct uio *uio;
	int flags;
	struct process *p;
{

#ifdef DEBUG
	if (vndebug & VDB_FOLLOW)
		printf("vnwrite(%x, %x, %x, %x)\n", dev, uio, flags, p);
#endif
	return(physio(vnstrategy, NULL, dev, B_WRITE, minphys, uio));
}

/* ARGSUSED */
/* EN: suser-guarded: VNIOCSET (EBUSY if inited) opens vio->vn_file r/w, sets sc_size by GETATTR, vnsetcred/throttles, returns vn_size, marks VNF_INITED; VNIOCCLR clears; else ENOTTY. */
/* 中文：经 suser 检查：VNIOCSET 在已初始化时返回 EBUSY，否则读写方式 vn_open 指定文件，用 GETATTR 取大小（btodb，会截断）设 sc_size，调 vnsetcred 和 vnthrottle，回填 vn_size 并置 VNF_INITED；VNIOCCLR 调 vnclear；其他命令返回 ENOTTY。 */
vnioctl(dev, cmd, data, flag, p)
	dev_t dev;
	u_long cmd;
	caddr_t data;
	int flag;
	struct process *p;
{
	int unit = vnunit(dev);
	register struct vn_softc *vn;
	struct vn_ioctl *vio;
	struct vattr vattr;
	struct nameidata nd;
	int error;

#ifdef DEBUG
	if (vndebug & VDB_FOLLOW)
		printf("vnioctl(%x, %x, %x, %x, %x): unit %d\n",
		       dev, cmd, data, flag, p, unit);
#endif
	error = suser(p->p_ucred, &p->p_acflag);
	if (error)
		return (error);
	if (unit >= numvnd)
		return (ENXIO);

	vn = &vn_softc[unit];
	vio = (struct vn_ioctl *)data;
	switch (cmd) {

	case VNIOCSET:
		if (vn->sc_flags & VNF_INITED)
			return(EBUSY);
		/*
		 * Always open for read and write.
		 * This is probably bogus, but it lets vn_open()
		 * weed out directories, sockets, etc. so we don't
		 * have to worry about them.
		 */
		NDINIT(&nd, LOOKUP, FOLLOW, UIO_USERSPACE, vio->vn_file, p);
		if (error = vn_open(&nd, FREAD|FWRITE, 0))
			return(error);
		if (error = VOP_GETATTR(nd.ni_vp, &vattr, p->p_ucred, p)) {
			VOP_UNLOCK(nd.ni_vp, 0, p);
			(void) vn_close(nd.ni_vp, FREAD|FWRITE, p->p_ucred, p);
			return(error);
		}
		VOP_UNLOCK(nd.ni_vp, 0, p);
		vn->sc_vp = nd.ni_vp;
		vn->sc_size = btodb(vattr.va_size);	/* note truncation */
		if (error = vnsetcred(vn, p->p_ucred)) {
			(void) vn_close(nd.ni_vp, FREAD|FWRITE, p->p_ucred, p);
			return(error);
		}
		vnthrottle(vn, vn->sc_vp);
		vio->vn_size = dbtob(vn->sc_size);
		vn->sc_flags |= VNF_INITED;
#ifdef DEBUG
		if (vndebug & VDB_INIT)
			printf("vnioctl: SET vp %x size %x\n",
			       vn->sc_vp, vn->sc_size);
#endif
		break;

	case VNIOCCLR:
		if ((vn->sc_flags & VNF_INITED) == 0)
			return(ENXIO);
		vnclear(vn);
#ifdef DEBUG
		if (vndebug & VDB_INIT)
			printf("vnioctl: CLRed\n");
#endif
		break;

	default:
		return(ENOTTY);
	}
	return(0);
}

/*
 * Duplicate the current processes' credentials.  Since we are called only
 * as the result of a SET ioctl and only root can do that, any future access
 * to this "disk" is essentially as root.  Note that credentials may change
 * if some other uid can write directly to the mapped file (NFS).
 */
/* EN: crdup()s cred into sc_cred, reads one DEV_BSIZE sector at offset 0 via VOP_READ under the new credentials (NFS kludge), frees the temp buf and returns the error. */
/* 中文：用 crdup 把 cred 复制到 sc_cred，再以新凭证在偏移 0 处 VOP_READ 读一个 DEV_BSIZE 扇区（为 NFS 建立凭证的变通做法），释放临时缓冲区并返回 VOP_READ 的错误码。 */
vnsetcred(vn, cred)
	register struct vn_softc *vn;
	struct ucred *cred;
{
	struct uio auio;
	struct iovec aiov;
	char *tmpbuf;
	int error;

	vn->sc_cred = crdup(cred);
	tmpbuf = malloc(DEV_BSIZE, M_TEMP, M_WAITOK);

	/* XXX: Horrible kludge to establish credentials for NFS */
	aiov.iov_base = tmpbuf;
	aiov.iov_len = min(DEV_BSIZE, dbtob(vn->sc_size));
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = 0;
	auio.uio_rw = UIO_READ;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_resid = aiov.iov_len;
	error = VOP_READ(vn->sc_vp, &auio, 0, vn->sc_cred);

	free(tmpbuf, M_TEMP);
	return (error);
}

/*
 * Set maxactive based on FS type
 */
/* EN: Sets sc_maxactive to 2 for NFS vnodes (v_op == nfsv2_vnodeop_p) and 8 otherwise, clamping the result to at least 1. */
/* 中文：若 vnode 为 NFS（v_op == nfsv2_vnodeop_p）则把 sc_maxactive 设为 2，否则设为 8，并保证不小于 1。 */
vnthrottle(vn, vp)
	register struct vn_softc *vn;
	struct vnode *vp;
{
	extern int (**nfsv2_vnodeop_p)();

	if (vp->v_op == nfsv2_vnodeop_p)
		vn->sc_maxactive = 2;
	else
		vn->sc_maxactive = 8;

	if (vn->sc_maxactive < 1)
		vn->sc_maxactive = 1;
}

/* EN: Shutdown hook that walks every vn_softc and vnclear()s each unit still marked VNF_INITED, closing its file and dropping credentials. */
/* 中文：关机钩子：遍历所有 vn_softc，对仍置有 VNF_INITED 的单元调用 vnclear，关闭其底层文件并释放凭证。 */
vnshutdown()
{
	register struct vn_softc *vn;

	for (vn = &vn_softc[0]; vn < &vn_softc[numvnd]; vn++)
		if (vn->sc_flags & VNF_INITED)
			vnclear(vn);
}

/* EN: Clears VNF_INITED, panics on a null sc_vp, vn_close()s the vnode with FREAD|FWRITE and sc_cred, crfree()s the credentials, then zeroes sc_vp, sc_cred and sc_size. */
/* 中文：清除 VNF_INITED；若 sc_vp 为空则 panic；以 FREAD|FWRITE 和 sc_cred 调用 vn_close 关闭 vnode，crfree 释放凭证，随后把 sc_vp、sc_cred、sc_size 清零。 */
vnclear(vn)
	register struct vn_softc *vn;
{
	register struct vnode *vp = vn->sc_vp;
	struct process *p = curproc;		/* XXX */

#ifdef DEBUG
	if (vndebug & VDB_FOLLOW)
		printf("vnclear(%x): vp %x\n", vp);
#endif
	vn->sc_flags &= ~VNF_INITED;
	if (vp == (struct vnode *)0)
		panic("vnioctl: null vp");
	(void) vn_close(vp, FREAD|FWRITE, vn->sc_cred, p);
	crfree(vn->sc_cred);
	vn->sc_vp = (struct vnode *)0;
	vn->sc_cred = (struct ucred *)0;
	vn->sc_size = 0;
}

/* EN: Returns the vnode disk size in DEV_BSIZE blocks, or -1 when unit >= numvnd or the unit is not VNF_INITED. */
/* 中文：返回以 DEV_BSIZE 块为单位的 vnode 磁盘大小；unit 超过 numvnd 或未置 VNF_INITED 时返回 -1。 */
vnsize(dev)
	dev_t dev;
{
	int unit = vnunit(dev);
	register struct vn_softc *vn = &vn_softc[unit];

	if (unit >= numvnd || (vn->sc_flags & VNF_INITED) == 0)
		return(-1);
	return(vn->sc_size);
}

/* EN: Dump hook that always returns ENXIO, so a vnode disk cannot be used as a dump device. */
/* 中文：转储钩子，固定返回 ENXIO，表示 vnode 磁盘不能用作 dump 设备。 */
vndump(dev)
{
	return(ENXIO);
}
#endif
