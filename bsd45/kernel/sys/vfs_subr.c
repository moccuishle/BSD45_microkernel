/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
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
 *	@(#)vfs_subr.c	8.31 (Berkeley) 5/26/95
 */

/*
 * External virtual filesystem routines
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kprocess.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/stat.h>
#include <sys/namei.h>
#include <sys/ucred.h>
#include <sys/buf.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/domain.h>
#include <sys/mbuf.h>

#include <vm/vm.h>
#include <sys/sysctl.h>

#include <miscfs/specfs/specdev.h>

enum vtype iftovt_tab[16] = {
	VNON, VFIFO, VCHR, VNON, VDIR, VNON, VBLK, VNON,
	VREG, VNON, VLNK, VNON, VSOCK, VNON, VNON, VBAD,
};
int	vttoif_tab[9] = {
	0, S_IFREG, S_IFDIR, S_IFBLK, S_IFCHR, S_IFLNK,
	S_IFSOCK, S_IFIFO, S_IFMT,
};

/*
 * Insq/Remq for the vnode usage lists.
 */
#define	bufinsvn(bp, dp)	LIST_INSERT_HEAD(dp, bp, b_vnbufs)
#define	bufremvn(bp) {							\
	LIST_REMOVE(bp, b_vnbufs);					\
	(bp)->b_vnbufs.le_next = NOLIST;				\
}
TAILQ_HEAD(freelst, vnode) vnode_free_list;	/* vnode free list */
struct mntlist mountlist;			/* mounted filesystem list */
struct simplelock mountlist_slock;
static struct simplelock mntid_slock;
struct simplelock mntvnode_slock;
struct simplelock vnode_free_list_slock;
static struct simplelock spechash_slock;

/*
 * Initialize the vnode management data structures.
 */
/* 中文：初始化 vnode 管理数据结构：各简单锁、空闲 vnode 链表和挂载点链表 */
/* EN: Initialize vnode management data structures: the simple locks, free vnode list and mount-point list */
void
vntblinit()
{

	simple_lock_init(&mntvnode_slock);
	simple_lock_init(&mntid_slock);
	simple_lock_init(&spechash_slock);
	TAILQ_INIT(&vnode_free_list);
	simple_lock_init(&vnode_free_list_slock);
	CIRCLEQ_INIT(&mountlist);
}

/*
 * Mark a mount point as busy. Used to synchronize access and to delay
 * unmounting. Interlock is not released on failure.
 */
/* 中文：把文件系统标记为忙以防止卸载：加共享锁，若已在卸载则按 LK_NOWAIT 返回或睡眠等待 */
/* EN: Mark a filesystem busy to delay unmounting: take a shared lock, or under LK_NOWAIT return, or sleep if already being unmounted */
int
vfs_busy(mp, flags, interlkp, p)
	struct mount *mp;
	int flags;
	struct simplelock *interlkp;
	struct process *p;
{
	int lkflags;

	if (mp->mnt_flag & MNT_UNMOUNT) {
		if (flags & LK_NOWAIT)
			return (ENOENT);
		mp->mnt_flag |= MNT_MWAIT;
		if (interlkp)
			simple_unlock(interlkp);
		/*
		 * Since all busy locks are shared except the exclusive
		 * lock granted when unmounting, the only place that a
		 * wakeup needs to be done is at the release of the
		 * exclusive lock at the end of dounmount.
		 */
		sleep((caddr_t)mp, PVFS);
		if (interlkp)
			simple_lock(interlkp);
		return (ENOENT);
	}
	lkflags = LK_SHARED;
	if (interlkp)
		lkflags |= LK_INTERLOCK;
	if (lockmgr(&mp->mnt_lock, lkflags, interlkp, p))
		panic("vfs_busy: unexpected lock failure");
	return (0);
}

/*
 * Free a busy filesystem.
 */
/* 中文：释放由 vfs_busy 获取的文件系统忙锁 */
/* EN: Release the busy lock on a filesystem previously acquired by vfs_busy */
void
vfs_unbusy(mp, p)
	struct mount *mp;
	struct process *p;
{

	lockmgr(&mp->mnt_lock, LK_RELEASE, NULL, p);
}

/*
 * Lookup a filesystem type, and if found allocate and initialize
 * a mount structure for it.
 *
 * Devname is usually updated by mount(8) after booting.
 */
/* 中文：按文件系统类型名查找 vfsconf 并分配、初始化一个 mount 结构 */
/* EN: Look up a vfsconf by filesystem type name and allocate and initialize a mount structure */
int
vfs_rootmountalloc(fstypename, devname, mpp)
	char *fstypename;
	char *devname;
	struct mount **mpp;
{
	struct process *p = curproc;	/* XXX */
	struct vfsconf *vfsp;
	struct mount *mp;

	for (vfsp = vfsconf; vfsp; vfsp = vfsp->vfc_next)
		if (!strcmp(vfsp->vfc_name, fstypename))
			break;
	if (vfsp == NULL)
		return (ENODEV);
	mp = malloc((u_long)sizeof(struct mount), M_MOUNT, M_WAITOK);
	bzero((char *)mp, (u_long)sizeof(struct mount));
	lockinit(&mp->mnt_lock, PVFS, "vfslock", 0, 0);
	(void)vfs_busy(mp, LK_NOWAIT, 0, p);
	LIST_INIT(&mp->mnt_vnodelist);
	mp->mnt_vfc = vfsp;
	mp->mnt_op = vfsp->vfc_vfsops;
	mp->mnt_flag = MNT_RDONLY;
	mp->mnt_vnodecovered = NULLVP;
	vfsp->vfc_refcount++;
	mp->mnt_stat.f_type = vfsp->vfc_typenum;
	mp->mnt_flag |= vfsp->vfc_flags & MNT_VISFLAGMASK;
	strncpy(mp->mnt_stat.f_fstypename, vfsp->vfc_name, MFSNAMELEN);
	mp->mnt_stat.f_mntonname[0] = '/';
	(void) copystr(devname, mp->mnt_stat.f_mntfromname, MNAMELEN - 1, 0);
	*mpp = mp;
	return (0);
}

/*
 * Find an appropriate filesystem to use for the root. If a filesystem
 * has not been preselected, walk through the list of known filesystems
 * trying those that have mountroot routines, and try them until one
 * works or we have tried them all.
 */
/* 中文：挂载根文件系统：优先用预设的 mountroot，否则遍历已知文件系统依次尝试其 mountroot 例程 */
/* EN: Mount the root filesystem: use a preset mountroot if available, else try each known filesystem's mountroot routine */
int
vfs_mountroot()
{
	struct vfsconf *vfsp;
	extern int (*mountroot)(void);
	int error;

	if (mountroot != NULL)
		return ((*mountroot)());
	for (vfsp = vfsconf; vfsp; vfsp = vfsp->vfc_next) {
		if (vfsp->vfc_mountroot == NULL)
			continue;
		if ((error = (*vfsp->vfc_mountroot)()) == 0)
			return (0);
		printf("%s_mountroot failed: %d\n", vfsp->vfc_name, error);
	}
	return (ENODEV);
}

/*
 * Lookup a mount point by filesystem identifier.
 */
/* 中文：按文件系统标识符 fsid 在挂载链表中查找对应的 mount */
/* EN: Look up a mount by filesystem identifier fsid in the mount list */
struct mount *
vfs_getvfs(fsid)
	fsid_t *fsid;
{
	register struct mount *mp;

	simple_lock(&mountlist_slock);
	for (mp = mountlist.cqh_first; mp != (void *)&mountlist;
	     mp = mp->mnt_list.cqe_next) {
		if (mp->mnt_stat.f_fsid.val[0] == fsid->val[0] &&
		    mp->mnt_stat.f_fsid.val[1] == fsid->val[1]) {
			simple_unlock(&mountlist_slock);
			return (mp);
		}
	}
	simple_unlock(&mountlist_slock);
	return ((struct mount *)0);
}

/*
 * Get a new unique fsid
 */
/* 中文：为文件系统分配一个新的唯一 fsid，确保不与现有挂载点冲突 */
/* EN: Assign a new unique filesystem id (fsid) that does not collide with existing mounts */
void
vfs_getnewfsid(mp)
	struct mount *mp;
{
static u_short xxxfs_mntid;

	fsid_t tfsid;
	int mtype;

	simple_lock(&mntid_slock);
	mtype = mp->mnt_vfc->vfc_typenum;
	mp->mnt_stat.f_fsid.val[0] = makedev(nblkdev + mtype, 0);
	mp->mnt_stat.f_fsid.val[1] = mtype;
	if (xxxfs_mntid == 0)
		++xxxfs_mntid;
	tfsid.val[0] = makedev(nblkdev + mtype, xxxfs_mntid);
	tfsid.val[1] = mtype;
	if (mountlist.cqh_first != (void *)&mountlist) {
		while (vfs_getvfs(&tfsid)) {
			tfsid.val[0]++;
			xxxfs_mntid++;
		}
	}
	mp->mnt_stat.f_fsid.val[0] = tfsid.val[0];
	simple_unlock(&mntid_slock);
}

/*
 * Set vnode attributes to VNOVAL
 */
/* 中文：把 vattr 结构中的所有属性字段设为 VNOVAL（未指定值） */
/* EN: Set all attributes in a vattr structure to VNOVAL (unspecified) */
void
vattr_null(vap)
	register struct vattr *vap;
{

	vap->va_type = VNON;
	vap->va_size = vap->va_bytes = VNOVAL;
	vap->va_mode = vap->va_nlink = vap->va_uid = vap->va_gid =
		vap->va_fsid = vap->va_fileid =
		vap->va_blocksize = vap->va_rdev =
		vap->va_atime.ts_sec = vap->va_atime.ts_nsec =
		vap->va_mtime.ts_sec = vap->va_mtime.ts_nsec =
		vap->va_ctime.ts_sec = vap->va_ctime.ts_nsec =
		vap->va_flags = vap->va_gen = VNOVAL;
	vap->va_vaflags = 0;
}

/*
 * Routines having to do with the management of the vnode table.
 */
extern int (**dead_vnodeop_p)();
static void vclean __P((struct vnode *vp, int flag, struct process *p));
extern void vgonel __P((struct vnode *vp, struct process *p));
long numvnodes;
extern struct vattr va_null;

/*
 * Return the next vnode from the free list.
 */
/* 中文：从空闲链表取一个或新分配一个 vnode，初始化后挂入指定挂载点并返回 */
/* EN: Get a vnode from the free list or allocate a new one, initialize it and place it on the given mount */
int
getnewvnode(tag, mp, vops, vpp)
	enum vtagtype tag;
	struct mount *mp;
	int (**vops)();
	struct vnode **vpp;
{
	struct process *p = curproc;	/* XXX */
	struct vnode *vp;
	int s;
	int cnt;

top:
	simple_lock(&vnode_free_list_slock);
	if ((vnode_free_list.tqh_first == NULL &&
	     numvnodes < 2 * desiredvnodes) ||
	    numvnodes < desiredvnodes) {
		simple_unlock(&vnode_free_list_slock);
		vp = (struct vnode *)malloc((u_long)sizeof *vp,
		    M_VNODE, M_WAITOK);
		bzero((char *)vp, sizeof *vp);
		numvnodes++;
	} else {
		for (vp = vnode_free_list.tqh_first;
				vp != NULLVP; vp = vp->v_freelist.tqe_next) {
			if (simple_lock_try(&vp->v_interlock))
				break;
		}
		/*
		 * Unless this is a bad time of the month, at most
		 * the first NCPUS items on the free list are
		 * locked, so this is close enough to being empty.
		 */
		if (vp == NULLVP) {
			simple_unlock(&vnode_free_list_slock);
			tablefull("vnode");
			*vpp = 0;
			return (ENFILE);
		}
		if (vp->v_usecount)
			panic("free vnode isn't");
		TAILQ_REMOVE(&vnode_free_list, vp, v_freelist);
		/* see comment on why 0xdeadb is set at end of vgone (below) */
		vp->v_freelist.tqe_prev = (struct vnode **)0xdeadb;
		simple_unlock(&vnode_free_list_slock);
		vp->v_lease = NULL;
		if (vp->v_type != VBAD)
			vgonel(vp, p);
		else
			simple_unlock(&vp->v_interlock);
#ifdef DIAGNOSTIC
		if (vp->v_data)
			panic("cleaned vnode isn't");
		s = splbio();
		if (vp->v_numoutput)
			panic("Clean vnode has pending I/O's");
		splx(s);
#endif
		vp->v_flag = 0;
		vp->v_lastr = 0;
		vp->v_ralen = 0;
		vp->v_maxra = 0;
		vp->v_lastw = 0;
		vp->v_lasta = 0;
		vp->v_cstart = 0;
		vp->v_clen = 0;
		vp->v_socket = 0;
	}
	vp->v_type = VNON;
	cache_purge(vp);
	vp->v_tag = tag;
	vp->v_op = vops;
	insmntque(vp, mp);
	*vpp = vp;
	vp->v_usecount = 1;
	vp->v_data = 0;
	return (0);
}

/*
 * Move a vnode from one mount queue to another.
 */
/* 中文：把一个 vnode 从原挂载队列移到新挂载点的 vnode 队列 */
/* EN: Move a vnode from its old mount queue to another mount point's vnode queue */
void
insmntque(vp, mp)
	struct vnode *vp;
	struct mount *mp;
{

	simple_lock(&mntvnode_slock);
	/*
	 * Delete from old mount point vnode list, if on one.
	 */
	if (vp->v_mount != NULL)
		LIST_REMOVE(vp, v_mntvnodes);
	/*
	 * Insert into list of vnodes for the new mount point, if available.
	 */
	if ((vp->v_mount = mp) != NULL)
		LIST_INSERT_HEAD(&mp->mnt_vnodelist, vp, v_mntvnodes);
	simple_unlock(&mntvnode_slock);
}

/*
 * Update outstanding I/O count and do wakeup if requested.
 */
/* 中文：减少 vnode 的未完成写 I/O 计数，降为 0 时清 VBWAIT 并唤醒等待者 */
/* EN: Decrement a vnode's outstanding output count and wake waiters clearing VBWAIT when it reaches zero */
void
vwakeup(bp)
	register struct buf *bp;
{
	register struct vnode *vp;

	bp->b_flags &= ~B_WRITEINPROG;
	if (vp = bp->b_vp) {
		if (--vp->v_numoutput < 0)
			panic("vwakeup: neg numoutput");
		if ((vp->v_flag & VBWAIT) && vp->v_numoutput <= 0) {
			if (vp->v_numoutput < 0)
				panic("vwakeup: neg numoutput 2");
			vp->v_flag &= ~VBWAIT;
			wakeup((caddr_t)&vp->v_numoutput);
		}
	}
}

/*
 * Flush out and invalidate all buffers associated with a vnode.
 * Called with the underlying object locked.
 */
/* 中文：冲刷并作废与某 vnode 关联的所有缓冲区，V_SAVE 时先 fsync 保存脏数据 */
/* EN: Flush and invalidate all buffers associated with a vnode, fsyncing dirty data first when V_SAVE is set */
int
vinvalbuf(vp, flags, cred, p, slpflag, slptimeo)
	register struct vnode *vp;
	int flags;
	struct ucred *cred;
	struct process *p;
	int slpflag, slptimeo;
{
	register struct buf *bp;
	struct buf *nbp, *blist;
	int s, error;

	if (flags & V_SAVE) {
		if (error = VOP_FSYNC(vp, cred, MNT_WAIT, p))
			return (error);
		if (vp->v_dirtyblkhd.lh_first != NULL)
			panic("vinvalbuf: dirty bufs");
	}
	for (;;) {
		if ((blist = vp->v_cleanblkhd.lh_first) && flags & V_SAVEMETA)
			while (blist && blist->b_lblkno < 0)
				blist = blist->b_vnbufs.le_next;
		if (!blist && (blist = vp->v_dirtyblkhd.lh_first) &&
		    (flags & V_SAVEMETA))
			while (blist && blist->b_lblkno < 0)
				blist = blist->b_vnbufs.le_next;
		if (!blist)
			break;

		for (bp = blist; bp; bp = nbp) {
			nbp = bp->b_vnbufs.le_next;
			if (flags & V_SAVEMETA && bp->b_lblkno < 0)
				continue;
			s = splbio();
			if (bp->b_flags & B_BUSY) {
				bp->b_flags |= B_WANTED;
				error = tsleep((caddr_t)bp,
					slpflag | (PRIBIO + 1), "vinvalbuf",
					slptimeo);
				splx(s);
				if (error)
					return (error);
				break;
			}
			bremfree(bp);
			bp->b_flags |= B_BUSY;
			splx(s);
			/*
			 * XXX Since there are no node locks for NFS, I believe
			 * there is a slight chance that a delayed write will
			 * occur while sleeping just above, so check for it.
			 */
			if ((bp->b_flags & B_DELWRI) && (flags & V_SAVE)) {
				(void) VOP_BWRITE(bp);
				break;
			}
			bp->b_flags |= B_INVAL;
			brelse(bp);
		}
	}
	if (!(flags & V_SAVEMETA) &&
	    (vp->v_dirtyblkhd.lh_first || vp->v_cleanblkhd.lh_first))
		panic("vinvalbuf: flush failed");
	return (0);
}

/*
 * Associate a buffer with a vnode.
 */
/* 中文：把一个缓冲区关联到 vnode：记录 b_vp 并加入该 vnode 的缓冲区链表 */
/* EN: Associate a buffer with a vnode: record b_vp and add it to the vnode's buffer list */
void
bgetvp(vp, bp)
	register struct vnode *vp;
	register struct buf *bp;
{

	if (bp->b_vp)
		panic("bgetvp: not free");
	VHOLD(vp);
	bp->b_vp = vp;
	if (vp->v_type == VBLK || vp->v_type == VCHR)
		bp->b_dev = vp->v_rdev;
	else
		bp->b_dev = NODEV;
	/*
	 * Insert onto list for new vnode.
	 */
	bufinsvn(bp, &vp->v_cleanblkhd);
}

/*
 * Disassociate a buffer from a vnode.
 */
/* 中文：解除缓冲区与 vnode 的关联：从链表移除并释放 vnode 引用 */
/* EN: Disassociate a buffer from its vnode: remove it from the list and release the vnode reference */
void
brelvp(bp)
	register struct buf *bp;
{
	struct vnode *vp;

	if (bp->b_vp == (struct vnode *) 0)
		panic("brelvp: NULL");
	/*
	 * Delete from old vnode list, if on one.
	 */
	if (bp->b_vnbufs.le_next != NOLIST)
		bufremvn(bp);
	vp = bp->b_vp;
	bp->b_vp = (struct vnode *) 0;
	HOLDRELE(vp);
}

/*
 * Reassign a buffer from one vnode to another.
 * Used to assign file specific control information
 * (indirect blocks) to the vnode to which they belong.
 */
/* 中文：把缓冲区重新归属到另一个 vnode，按脏/净状态挂入相应链表 */
/* EN: Reassign a buffer to a different vnode, placing it on the dirty or clean list accordingly */
void
reassignbuf(bp, newvp)
	register struct buf *bp;
	register struct vnode *newvp;
{
	register struct buflists *listheadp;

	if (newvp == NULL) {
		printf("reassignbuf: NULL");
		return;
	}
	/*
	 * Delete from old vnode list, if on one.
	 */
	if (bp->b_vnbufs.le_next != NOLIST)
		bufremvn(bp);
	/*
	 * If dirty, put on list of dirty buffers;
	 * otherwise insert onto list of clean buffers.
	 */
	if (bp->b_flags & B_DELWRI)
		listheadp = &newvp->v_dirtyblkhd;
	else
		listheadp = &newvp->v_cleanblkhd;
	bufinsvn(bp, listheadp);
}

/*
 * Create a vnode for a block device.
 * Used for root filesystem, argdev, and swap areas.
 * Also used for memory file system special devices.
 */
/* 中文：为块设备创建一个 vnode（用于根文件系统、argdev 和交换区） */
/* EN: Create a vnode for a block device (used for root filesystem, argdev and swap areas) */
int
bdevvp(dev, vpp)
	dev_t dev;
	struct vnode **vpp;
{
	register struct vnode *vp;
	struct vnode *nvp;
	int error;

	if (dev == NODEV) {
		*vpp = NULLVP;
		return (ENODEV);
	}
	error = getnewvnode(VT_NON, (struct mount *)0, spec_vnodeop_p, &nvp);
	if (error) {
		*vpp = NULLVP;
		return (error);
	}
	vp = nvp;
	vp->v_type = VBLK;
	if (nvp = checkalias(vp, dev, (struct mount *)0)) {
		vput(vp);
		vp = nvp;
	}
	*vpp = vp;
	return (0);
}

/*
 * Check to see if the new vnode represents a special device
 * for which we already have a vnode (either because of
 * bdevvp() or because of a different vnode representing
 * the same block device). If such an alias exists, deallocate
 * the existing contents and return the aliased vnode. The
 * caller is responsible for filling it with its new contents.
 */
/* 中文：检查新 vnode 是否表示已有 vnode 的特殊设备别名，若是则回收旧内容并返回别名 vnode */
/* EN: Check whether a new vnode aliases an existing special-device vnode, reclaiming the old contents and returning the alias if so */
struct vnode *
checkalias(nvp, nvp_rdev, mp)
	register struct vnode *nvp;
	dev_t nvp_rdev;
	struct mount *mp;
{
	struct process *p = curproc;	/* XXX */
	struct vnode *vp;
	struct vnode **vpp;

	if (nvp->v_type != VBLK && nvp->v_type != VCHR)
		return (NULLVP);

	vpp = &speclisth[SPECHASH(nvp_rdev)];
loop:
	simple_lock(&spechash_slock);
	for (vp = *vpp; vp; vp = vp->v_specnext) {
		if (nvp_rdev != vp->v_rdev || nvp->v_type != vp->v_type)
			continue;
		/*
		 * Alias, but not in use, so flush it out.
		 */
		simple_lock(&vp->v_interlock);
		if (vp->v_usecount == 0) {
			simple_unlock(&spechash_slock);
			vgonel(vp, p);
			goto loop;
		}
		if (vget(vp, LK_EXCLUSIVE | LK_INTERLOCK, p)) {
			simple_unlock(&spechash_slock);
			goto loop;
		}
		break;
	}
	if (vp == NULL || vp->v_tag != VT_NON) {
		MALLOC(nvp->v_specinfo, struct specinfo *,
			sizeof(struct specinfo), M_VNODE, M_WAITOK);
		nvp->v_rdev = nvp_rdev;
		nvp->v_hashchain = vpp;
		nvp->v_specnext = *vpp;
		nvp->v_specflags = 0;
		simple_unlock(&spechash_slock);
		*vpp = nvp;
		if (vp != NULLVP) {
			nvp->v_flag |= VALIASED;
			vp->v_flag |= VALIASED;
			vput(vp);
		}
		return (NULLVP);
	}
	simple_unlock(&spechash_slock);
	VOP_UNLOCK(vp, 0, p);
	simple_lock(&vp->v_interlock);
	vclean(vp, 0, p);
	vp->v_op = nvp->v_op;
	vp->v_tag = nvp->v_tag;
	nvp->v_type = VNON;
	insmntque(vp, mp);
	return (vp);
}

/*
 * Grab a particular vnode from the free list, increment its
 * reference count and lock it. The vnode lock bit is set the
 * vnode is being eliminated in vgone. The process is awakened
 * when the transition is completed, and an error returned to
 * indicate that the vnode is no longer usable (possibly having
 * been changed to a new file system type).
 */
/* 中文：从空闲链表获取指定 vnode 并加锁，若该 vnode 正被清理则等待并返回 ENOENT */
/* EN: Grab a vnode, increment its use count and lock it; if being vgone'd, wait then return ENOENT */
int
vget(vp, flags, p)
	struct vnode *vp;
	int flags;
	struct process *p;
{
	int error;

	/*
	 * If the vnode is in the process of being cleaned out for
	 * another use, we wait for the cleaning to finish and then
	 * return failure. Cleaning is determined by checking that
	 * the VXLOCK flag is set.
	 */
	if ((flags & LK_INTERLOCK) == 0)
		simple_lock(&vp->v_interlock);
	if (vp->v_flag & VXLOCK) {
		vp->v_flag |= VXWANT;
		simple_unlock(&vp->v_interlock);
		tsleep((caddr_t)vp, PINOD, "vget", 0);
		return (ENOENT);
	}
	if (vp->v_usecount == 0) {
		simple_lock(&vnode_free_list_slock);
		TAILQ_REMOVE(&vnode_free_list, vp, v_freelist);
		simple_unlock(&vnode_free_list_slock);
	}
	vp->v_usecount++;
	if (flags & LK_TYPE_MASK) {
		if (error = vn_lock(vp, flags | LK_INTERLOCK, p))
			vrele(vp);
		return (error);
	}
	simple_unlock(&vp->v_interlock);
	return (0);
}

/*
 * Stubs to use when there is no locking to be done on the underlying object.
 * A minimal shared lock is necessary to ensure that the underlying object
 * is not revoked while an operation is in progress. So, an active shared
 * count is maintained in an auxillary vnode lock structure.
 */
/* 中文：无底层锁时的 VOP_LOCK 桩函数，仅清除 interlock 并返回成功 */
/* EN: VOP_LOCK stub for objects with no backing lock, clearing the interlock and returning success */
int
vop_nolock(ap)
	struct vop_lock_args /* {
		struct vnode *a_vp;
		int a_flags;
		struct process *a_p;
	} */ *ap;
{
#ifdef notyet
	/*
	 * This code cannot be used until all the non-locking filesystems
	 * (notably NFS) are converted to properly lock and release nodes.
	 * Also, certain vnode operations change the locking state within
	 * the operation (create, mknod, remove, link, rename, mkdir, rmdir,
	 * and symlink). Ideally these operations should not change the
	 * lock state, but should be changed to let the caller of the
	 * function unlock them. Otherwise all intermediate vnode layers
	 * (such as union, umapfs, etc) must catch these functions to do
	 * the necessary locking at their layer. Note that the inactive
	 * and lookup operations also change their lock state, but this 
	 * cannot be avoided, so these two operations will always need
	 * to be handled in intermediate layers.
	 */
	struct vnode *vp = ap->a_vp;
	int vnflags, flags = ap->a_flags;

	if (vp->v_vnlock == NULL) {
		if ((flags & LK_TYPE_MASK) == LK_DRAIN)
			return (0);
		MALLOC(vp->v_vnlock, struct lock *, sizeof(struct lock),
		    M_VNODE, M_WAITOK);
		lockinit(vp->v_vnlock, PVFS, "vnlock", 0, 0);
	}
	switch (flags & LK_TYPE_MASK) {
	case LK_DRAIN:
		vnflags = LK_DRAIN;
		break;
	case LK_EXCLUSIVE:
	case LK_SHARED:
		vnflags = LK_SHARED;
		break;
	case LK_UPGRADE:
	case LK_EXCLUPGRADE:
	case LK_DOWNGRADE:
		return (0);
	case LK_RELEASE:
	default:
		panic("vop_nolock: bad operation %d", flags & LK_TYPE_MASK);
	}
	if (flags & LK_INTERLOCK)
		vnflags |= LK_INTERLOCK;
	return(lockmgr(vp->v_vnlock, vnflags, &vp->v_interlock, ap->a_p));
#else /* for now */
	/*
	 * Since we are not using the lock manager, we must clear
	 * the interlock here.
	 */
	if (ap->a_flags & LK_INTERLOCK)
		simple_unlock(&ap->a_vp->v_interlock);
	return (0);
#endif
}

/*
 * Decrement the active use count.
 */
/* 中文：无底层锁时的 VOP_UNLOCK 桩函数，释放辅助 vnode 锁并返回结果 */
/* EN: VOP_UNLOCK stub for objects with no backing lock, releasing the auxiliary vnode lock */
int
vop_nounlock(ap)
	struct vop_unlock_args /* {
		struct vnode *a_vp;
		int a_flags;
		struct process *a_p;
	} */ *ap;
{
	struct vnode *vp = ap->a_vp;

	if (vp->v_vnlock == NULL)
		return (0);
	return (lockmgr(vp->v_vnlock, LK_RELEASE, NULL, ap->a_p));
}

/*
 * Return whether or not the node is in use.
 */
/* 中文：VOP_ISLOCKED 桩函数，返回辅助 vnode 锁是否被持有 */
/* EN: VOP_ISLOCKED stub that returns whether the auxiliary vnode lock is held */
int
vop_noislocked(ap)
	struct vop_islocked_args /* {
		struct vnode *a_vp;
	} */ *ap;
{
	struct vnode *vp = ap->a_vp;

	if (vp->v_vnlock == NULL)
		return (0);
	return (lockstatus(vp->v_vnlock));
}

/*
 * Vnode reference.
 */
/* 中文：增加 vnode 的引用计数 */
/* EN: Increment the reference count of a vnode */
void
vref(vp)
	struct vnode *vp;
{

	simple_lock(&vp->v_interlock);
	if (vp->v_usecount <= 0)
		panic("vref used where vget required");
	vp->v_usecount++;
	simple_unlock(&vp->v_interlock);
}

/*
 * vput(), just unlock and vrele()
 */
/* 中文：释放对 vnode 的引用并解锁，引用数归零时调用 VOP_INACTIVE 并放入空闲链表 */
/* EN: Unlock and release a vnode reference, calling VOP_INACTIVE and freeing it when the count reaches zero */
void
vput(vp)
	struct vnode *vp;
{
	struct process *p = curproc;	/* XXX */

#ifdef DIGANOSTIC
	if (vp == NULL)
		panic("vput: null vp");
#endif
	simple_lock(&vp->v_interlock);
	vp->v_usecount--;
	if (vp->v_usecount > 0) {
		simple_unlock(&vp->v_interlock);
		VOP_UNLOCK(vp, 0, p);
		return;
	}
#ifdef DIAGNOSTIC
	if (vp->v_usecount < 0 || vp->v_writecount != 0) {
		vprint("vput: bad ref count", vp);
		panic("vput: ref cnt");
	}
#endif
	/*
	 * insert at tail of LRU list
	 */
	simple_lock(&vnode_free_list_slock);
	TAILQ_INSERT_TAIL(&vnode_free_list, vp, v_freelist);
	simple_unlock(&vnode_free_list_slock);
	simple_unlock(&vp->v_interlock);
	VOP_INACTIVE(vp, p);
}

/*
 * Vnode release.
 * If count drops to zero, call inactive routine and return to freelist.
 */
/* 中文：释放对 vnode 的引用，引用数归零时调用 VOP_INACTIVE 并放入空闲链表 */
/* EN: Release a vnode reference, calling VOP_INACTIVE and placing it on the free list when the count reaches zero */
void
vrele(vp)
	struct vnode *vp;
{
	struct process *p = curproc;	/* XXX */

#ifdef DIAGNOSTIC
	if (vp == NULL)
		panic("vrele: null vp");
#endif
	simple_lock(&vp->v_interlock);
	vp->v_usecount--;
	if (vp->v_usecount > 0) {
		simple_unlock(&vp->v_interlock);
		return;
	}
#ifdef DIAGNOSTIC
	if (vp->v_usecount < 0 || vp->v_writecount != 0) {
		vprint("vrele: bad ref count", vp);
		panic("vrele: ref cnt");
	}
#endif
	/*
	 * insert at tail of LRU list
	 */
	simple_lock(&vnode_free_list_slock);
	TAILQ_INSERT_TAIL(&vnode_free_list, vp, v_freelist);
	simple_unlock(&vnode_free_list_slock);
	if (vn_lock(vp, LK_EXCLUSIVE | LK_INTERLOCK, p) == 0)
		VOP_INACTIVE(vp, p);
}

#ifdef DIAGNOSTIC
/*
 * Page or buffer structure gets a reference.
 */
/* 中文：为页或缓冲区结构增加一个 hold 引用计数（DIAGNOSTIC 下启用） */
/* EN: Add a hold reference to a page or buffer structure (enabled under DIAGNOSTIC) */
void
vhold(vp)
	register struct vnode *vp;
{

	simple_lock(&vp->v_interlock);
	vp->v_holdcnt++;
	simple_unlock(&vp->v_interlock);
}

/*
 * Page or buffer structure frees a reference.
 */
/* 中文：释放页或缓冲区结构的一个 hold 引用计数（DIAGNOSTIC 下启用） */
/* EN: Release a hold reference on a page or buffer structure (enabled under DIAGNOSTIC) */
void
holdrele(vp)
	register struct vnode *vp;
{

	simple_lock(&vp->v_interlock);
	if (vp->v_holdcnt <= 0)
		panic("holdrele: holdcnt");
	vp->v_holdcnt--;
	simple_unlock(&vp->v_interlock);
}
#endif /* DIAGNOSTIC */

/*
 * Remove any vnodes in the vnode table belonging to mount point mp.
 *
 * If MNT_NOFORCE is specified, there should not be any active ones,
 * return error if any are found (nb: this is a user error, not a
 * system error). If MNT_FORCE is specified, detach any active vnodes
 * that are found.
 */
#ifdef DIAGNOSTIC
int busyprt = 0;	/* print out busy vnodes */
struct ctldebug debug1 = { "busyprt", &busyprt };
#endif

/* 中文：清除挂载点上属于它的所有 vnode，按标志可跳过系统 vnode 或强制关闭活动 vnode，忙则返回 EBUSY */
/* EN: Flush all vnodes belonging to a mount, skipping system vnodes or force-closing active ones per flags; returns EBUSY if any are busy */
int
vflush(mp, skipvp, flags)
	struct mount *mp;
	struct vnode *skipvp;
	int flags;
{
	struct process *p = curproc;	/* XXX */
	struct vnode *vp, *nvp;
	int busy = 0;

	simple_lock(&mntvnode_slock);
loop:
	for (vp = mp->mnt_vnodelist.lh_first; vp; vp = nvp) {
		if (vp->v_mount != mp)
			goto loop;
		nvp = vp->v_mntvnodes.le_next;
		/*
		 * Skip over a selected vnode.
		 */
		if (vp == skipvp)
			continue;

		simple_lock(&vp->v_interlock);
		/*
		 * Skip over a vnodes marked VSYSTEM.
		 */
		if ((flags & SKIPSYSTEM) && (vp->v_flag & VSYSTEM)) {
			simple_unlock(&vp->v_interlock);
			continue;
		}
		/*
		 * If WRITECLOSE is set, only flush out regular file
		 * vnodes open for writing.
		 */
		if ((flags & WRITECLOSE) &&
		    (vp->v_writecount == 0 || vp->v_type != VREG)) {
			simple_unlock(&vp->v_interlock);
			continue;
		}
		/*
		 * With v_usecount == 0, all we need to do is clear
		 * out the vnode data structures and we are done.
		 */
		if (vp->v_usecount == 0) {
			simple_unlock(&mntvnode_slock);
			vgonel(vp, p);
			simple_lock(&mntvnode_slock);
			continue;
		}
		/*
		 * If FORCECLOSE is set, forcibly close the vnode.
		 * For block or character devices, revert to an
		 * anonymous device. For all other files, just kill them.
		 */
		if (flags & FORCECLOSE) {
			simple_unlock(&mntvnode_slock);
			if (vp->v_type != VBLK && vp->v_type != VCHR) {
				vgonel(vp, p);
			} else {
				vclean(vp, 0, p);
				vp->v_op = spec_vnodeop_p;
				insmntque(vp, (struct mount *)0);
			}
			simple_lock(&mntvnode_slock);
			continue;
		}
#ifdef DIAGNOSTIC
		if (busyprt)
			vprint("vflush: busy vnode", vp);
#endif
		simple_unlock(&vp->v_interlock);
		busy++;
	}
	simple_unlock(&mntvnode_slock);
	if (busy)
		return (EBUSY);
	return (0);
}

/*
 * Disassociate the underlying file system from a vnode.
 * The vnode interlock is held on entry.
 */
/* 中文：解除 vnode 与底层文件系统的关联：冲刷缓冲、关闭、回收并清空特定数据 */
/* EN: Disassociate a vnode from its underlying filesystem: flush buffers, close, reclaim and clear filesystem-specific data */
static void
vclean(vp, flags, p)
	struct vnode *vp;
	int flags;
	struct process *p;
{
	int active;

	/*
	 * Check to see if the vnode is in use.
	 * If so we have to reference it before we clean it out
	 * so that its count cannot fall to zero and generate a
	 * race against ourselves to recycle it.
	 */
	if (active = vp->v_usecount)
		vp->v_usecount++;
	/*
	 * Prevent the vnode from being recycled or
	 * brought into use while we clean it out.
	 */
	if (vp->v_flag & VXLOCK)
		panic("vclean: deadlock");
	vp->v_flag |= VXLOCK;
	/*
	 * Even if the count is zero, the VOP_INACTIVE routine may still
	 * have the object locked while it cleans it out. The VOP_LOCK
	 * ensures that the VOP_INACTIVE routine is done with its work.
	 * For active vnodes, it ensures that no other activity can
	 * occur while the underlying object is being cleaned out.
	 */
	VOP_LOCK(vp, LK_DRAIN | LK_INTERLOCK, p);
	/*
	 * Clean out any buffers associated with the vnode.
	 */
	if (flags & DOCLOSE)
		vinvalbuf(vp, V_SAVE, NOCRED, p, 0, 0);
	/*
	 * If purging an active vnode, it must be closed and
	 * deactivated before being reclaimed. Note that the
	 * VOP_INACTIVE will unlock the vnode.
	 */
	if (active) {
		if (flags & DOCLOSE)
			VOP_CLOSE(vp, IO_NDELAY, NOCRED, p);
		VOP_INACTIVE(vp, p);
	} else {
		/*
		 * Any other processes trying to obtain this lock must first
		 * wait for VXLOCK to clear, then call the new lock operation.
		 */
		VOP_UNLOCK(vp, 0, p);
	}
	/*
	 * Reclaim the vnode.
	 */
	if (VOP_RECLAIM(vp, p))
		panic("vclean: cannot reclaim");
	if (active)
		vrele(vp);
	cache_purge(vp);
	if (vp->v_vnlock) {
		if ((vp->v_vnlock->lk_flags & LK_DRAINED) == 0)
			vprint("vclean: lock not drained", vp);
		FREE(vp->v_vnlock, M_VNODE);
		vp->v_vnlock = NULL;
	}

	/*
	 * Done with purge, notify sleepers of the grim news.
	 */
	vp->v_op = dead_vnodeop_p;
	vp->v_tag = VT_NON;
	vp->v_flag &= ~VXLOCK;
	if (vp->v_flag & VXWANT) {
		vp->v_flag &= ~VXWANT;
		wakeup((caddr_t)vp);
	}
}

/*
 * Eliminate all activity associated with  the requested vnode
 * and with all vnodes aliased to the requested vnode.
 */
/* 中文：撤销 vnode 及其所有别名 vnode 上的一切活动，供 revoke 使用 */
/* EN: Eliminate all activity on a vnode and all its aliased vnodes, used by revoke */
int
vop_revoke(ap)
	struct vop_revoke_args /* {
		struct vnode *a_vp;
		int a_flags;
	} */ *ap;
{
	struct vnode *vp, *vq;
	struct process *p = curproc;	/* XXX */

#ifdef DIAGNOSTIC
	if ((ap->a_flags & REVOKEALL) == 0)
		panic("vop_revoke");
#endif

	vp = ap->a_vp;
	simple_lock(&vp->v_interlock);

	if (vp->v_flag & VALIASED) {
		/*
		 * If a vgone (or vclean) is already in progress,
		 * wait until it is done and return.
		 */
		if (vp->v_flag & VXLOCK) {
			vp->v_flag |= VXWANT;
			simple_unlock(&vp->v_interlock);
			tsleep((caddr_t)vp, PINOD, "vop_revokeall", 0);
			return (0);
		}
		/*
		 * Ensure that vp will not be vgone'd while we
		 * are eliminating its aliases.
		 */
		vp->v_flag |= VXLOCK;
		simple_unlock(&vp->v_interlock);
		while (vp->v_flag & VALIASED) {
			simple_lock(&spechash_slock);
			for (vq = *vp->v_hashchain; vq; vq = vq->v_specnext) {
				if (vq->v_rdev != vp->v_rdev ||
				    vq->v_type != vp->v_type || vp == vq)
					continue;
				simple_unlock(&spechash_slock);
				vgone(vq);
				break;
			}
			if (vq == NULLVP)
				simple_unlock(&spechash_slock);
		}
		/*
		 * Remove the lock so that vgone below will
		 * really eliminate the vnode after which time
		 * vgone will awaken any sleepers.
		 */
		simple_lock(&vp->v_interlock);
		vp->v_flag &= ~VXLOCK;
	}
	vgonel(vp, p);
	return (0);
}

/*
 * Recycle an unused vnode to the front of the free list.
 * Release the passed interlock if the vnode will be recycled.
 */
/* 中文：若 vnode 引用数为 0 则销毁它并移到空闲链表头，供回收使用 */
/* EN: Recycle an unused vnode to the head of the free list if its use count is zero */
int
vrecycle(vp, inter_lkp, p)
	struct vnode *vp;
	struct simplelock *inter_lkp;
	struct process *p;
{

	simple_lock(&vp->v_interlock);
	if (vp->v_usecount == 0) {
		if (inter_lkp)
			simple_unlock(inter_lkp);
		vgonel(vp, p);
		return (1);
	}
	simple_unlock(&vp->v_interlock);
	return (0);
}

/*
 * Eliminate all activity associated with a vnode
 * in preparation for reuse.
 */
/* 中文：消除 vnode 上的所有活动以准备复用（加 interlock 后调用 vgonel） */
/* EN: Eliminate all activity on a vnode in preparation for reuse (takes interlock then calls vgonel) */
void
vgone(vp)
	struct vnode *vp;
{
	struct process *p = curproc;	/* XXX */

	simple_lock(&vp->v_interlock);
	vgonel(vp, p);
}

/*
 * vgone, with the vp interlock held.
 */
/* 中文：在持有 vnode interlock 的前提下消除其所有活动并准备复用 */
/* EN: Eliminate all activity on a vnode and prepare it for reuse, with the vnode interlock held */
void
vgonel(vp, p)
	struct vnode *vp;
	struct process *p;
{
	struct vnode *vq;
	struct vnode *vx;

	/*
	 * If a vgone (or vclean) is already in progress,
	 * wait until it is done and return.
	 */
	if (vp->v_flag & VXLOCK) {
		vp->v_flag |= VXWANT;
		simple_unlock(&vp->v_interlock);
		tsleep((caddr_t)vp, PINOD, "vgone", 0);
		return;
	}
	/*
	 * Clean out the filesystem specific data.
	 */
	vclean(vp, DOCLOSE, p);
	/*
	 * Delete from old mount point vnode list, if on one.
	 */
	if (vp->v_mount != NULL)
		insmntque(vp, (struct mount *)0);
	/*
	 * If special device, remove it from special device alias list
	 * if it is on one.
	 */
	if ((vp->v_type == VBLK || vp->v_type == VCHR) && vp->v_specinfo != 0) {
		simple_lock(&spechash_slock);
		if (*vp->v_hashchain == vp) {
			*vp->v_hashchain = vp->v_specnext;
		} else {
			for (vq = *vp->v_hashchain; vq; vq = vq->v_specnext) {
				if (vq->v_specnext != vp)
					continue;
				vq->v_specnext = vp->v_specnext;
				break;
			}
			if (vq == NULL)
				panic("missing bdev");
		}
		if (vp->v_flag & VALIASED) {
			vx = NULL;
			for (vq = *vp->v_hashchain; vq; vq = vq->v_specnext) {
				if (vq->v_rdev != vp->v_rdev ||
				    vq->v_type != vp->v_type)
					continue;
				if (vx)
					break;
				vx = vq;
			}
			if (vx == NULL)
				panic("missing alias");
			if (vq == NULL)
				vx->v_flag &= ~VALIASED;
			vp->v_flag &= ~VALIASED;
		}
		simple_unlock(&spechash_slock);
		FREE(vp->v_specinfo, M_VNODE);
		vp->v_specinfo = NULL;
	}
	/*
	 * If it is on the freelist and not already at the head,
	 * move it to the head of the list. The test of the back
	 * pointer and the reference count of zero is because
	 * it will be removed from the free list by getnewvnode,
	 * but will not have its reference count incremented until
	 * after calling vgone. If the reference count were
	 * incremented first, vgone would (incorrectly) try to
	 * close the previous instance of the underlying object.
	 * So, the back pointer is explicitly set to `0xdeadb' in
	 * getnewvnode after removing it from the freelist to ensure
	 * that we do not try to move it here.
	 */
	if (vp->v_usecount == 0) {
		simple_lock(&vnode_free_list_slock);
		if ((vp->v_freelist.tqe_prev != (struct vnode **)0xdeadb) &&
		    vnode_free_list.tqh_first != vp) {
			TAILQ_REMOVE(&vnode_free_list, vp, v_freelist);
			TAILQ_INSERT_HEAD(&vnode_free_list, vp, v_freelist);
		}
		simple_unlock(&vnode_free_list_slock);
	}
	vp->v_type = VBAD;
}

/*
 * Lookup a vnode by device number.
 */
/* 中文：按设备号和类型在特殊设备哈希链中查找对应的 vnode */
/* EN: Look up a vnode by device number and type in the special-device hash chains */
int
vfinddev(dev, type, vpp)
	dev_t dev;
	enum vtype type;
	struct vnode **vpp;
{
	struct vnode *vp;
	int rc = 0;

	simple_lock(&spechash_slock);
	for (vp = speclisth[SPECHASH(dev)]; vp; vp = vp->v_specnext) {
		if (dev != vp->v_rdev || type != vp->v_type)
			continue;
		*vpp = vp;
		rc = 1;
		break;
	}
	simple_unlock(&spechash_slock);
	return (rc);
}

/*
 * Calculate the total number of references to a special device.
 */
/* 中文：统计某个特殊设备（含别名）所有 vnode 的引用计数总和 */
/* EN: Count the total references across all vnodes (including aliases) for a special device */
int
vcount(vp)
	struct vnode *vp;
{
	struct vnode *vq, *vnext;
	int count;

loop:
	if ((vp->v_flag & VALIASED) == 0)
		return (vp->v_usecount);
	simple_lock(&spechash_slock);
	for (count = 0, vq = *vp->v_hashchain; vq; vq = vnext) {
		vnext = vq->v_specnext;
		if (vq->v_rdev != vp->v_rdev || vq->v_type != vp->v_type)
			continue;
		/*
		 * Alias, but not in use, so flush it out.
		 */
		if (vq->v_usecount == 0 && vq != vp) {
			simple_unlock(&spechash_slock);
			vgone(vq);
			goto loop;
		}
		count += vq->v_usecount;
	}
	simple_unlock(&spechash_slock);
	return (count);
}

/*
 * Print out a description of a vnode.
 */
static char *typename[] =
   { "VNON", "VREG", "VDIR", "VBLK", "VCHR", "VLNK", "VSOCK", "VFIFO", "VBAD" };

/* 中文：打印某个 vnode 的描述信息（类型、引用计数、标志等）供调试 */
/* EN: Print a description of a vnode (type, reference counts, flags, etc.) for debugging */
void
vprint(label, vp)
	char *label;
	register struct vnode *vp;
{
	char buf[64];

	if (label != NULL)
		printf("%s: ", label);
	printf("type %s, usecount %d, writecount %d, refcount %d,",
		typename[vp->v_type], vp->v_usecount, vp->v_writecount,
		vp->v_holdcnt);
	buf[0] = '\0';
	if (vp->v_flag & VROOT)
		strcat(buf, "|VROOT");
	if (vp->v_flag & VTEXT)
		strcat(buf, "|VTEXT");
	if (vp->v_flag & VSYSTEM)
		strcat(buf, "|VSYSTEM");
	if (vp->v_flag & VXLOCK)
		strcat(buf, "|VXLOCK");
	if (vp->v_flag & VXWANT)
		strcat(buf, "|VXWANT");
	if (vp->v_flag & VBWAIT)
		strcat(buf, "|VBWAIT");
	if (vp->v_flag & VALIASED)
		strcat(buf, "|VALIASED");
	if (buf[0] != '\0')
		printf(" flags (%s)", &buf[1]);
	if (vp->v_data == NULL) {
		printf("\n");
	} else {
		printf("\n\t");
		VOP_PRINT(vp);
	}
}

#ifdef DEBUG
/*
 * List all of the locked vnodes in the system.
 * Called when debugging the kernel.
 */
/* 中文：列出系统中所有被锁定的 vnode（DEBUG 下供内核调试使用） */
/* EN: List all locked vnodes in the system (used for kernel debugging under DEBUG) */
void
printlockedvnodes()
{
	struct process *p = curproc;	/* XXX */
	struct mount *mp, *nmp;
	struct vnode *vp;

	printf("Locked vnodes\n");
	simple_lock(&mountlist_slock);
	for (mp = mountlist.cqh_first; mp != (void *)&mountlist; mp = nmp) {
		if (vfs_busy(mp, LK_NOWAIT, &mountlist_slock, p)) {
			nmp = mp->mnt_list.cqe_next;
			continue;
		}
		for (vp = mp->mnt_vnodelist.lh_first;
		     vp != NULL;
		     vp = vp->v_mntvnodes.le_next) {
			if (VOP_ISLOCKED(vp))
				vprint((char *)0, vp);
		}
		simple_lock(&mountlist_slock);
		nmp = mp->mnt_list.cqe_next;
		vfs_unbusy(mp, p);
	}
	simple_unlock(&mountlist_slock);
}
#endif

/*
 * Top level filesystem related information gathering.
 */
/* 中文：VFS 通用 sysctl：返回最大类型号、文件系统配置，并把 FS 特有请求转发给对应文件系统的 sysctl */
/* EN: Generic VFS sysctl: return the max type number and filesystem config, forwarding FS-specific requests to the filesystem's sysctl */
int
vfs_sysctl(name, namelen, oldp, oldlenp, newp, newlen, p)
	int *name;
	u_int namelen;
	void *oldp;
	size_t *oldlenp;
	void *newp;
	size_t newlen;
	struct process *p;
{
	struct ctldebug *cdp;
	struct vfsconf *vfsp;

	/* all sysctl names at this level are at least name and field */
	if (namelen < 2)
		return (ENOTDIR);		/* overloaded */
	if (name[0] != VFS_GENERIC) {
		for (vfsp = vfsconf; vfsp; vfsp = vfsp->vfc_next)
			if (vfsp->vfc_typenum == name[0])
				break;
		if (vfsp == NULL)
			return (EOPNOTSUPP);
		return ((*vfsp->vfc_vfsops->vfs_sysctl)(&name[1], namelen - 1,
		    oldp, oldlenp, newp, newlen, p));
	}
	switch (name[1]) {
	case VFS_MAXTYPENUM:
		return (sysctl_rdint(oldp, oldlenp, newp, maxvfsconf));
	case VFS_CONF:
		if (namelen < 3)
			return (ENOTDIR);	/* overloaded */
		for (vfsp = vfsconf; vfsp; vfsp = vfsp->vfc_next)
			if (vfsp->vfc_typenum == name[2])
				break;
		if (vfsp == NULL)
			return (EOPNOTSUPP);
		return (sysctl_rdstruct(oldp, oldlenp, newp, vfsp,
		    sizeof(struct vfsconf)));
	}
	return (EOPNOTSUPP);
}

int kinfo_vdebug = 1;
int kinfo_vgetfailed;
#define KINFO_VNODESLOP	10
/*
 * Dump vnode list (via sysctl).
 * Copyout address of vnode followed by vnode.
 */
/* ARGSUSED */
/* 中文：通过 sysctl 导出所有 vnode 的指针和结构体副本；where 为 NULL 时只回填所需缓冲区大小 */
/* EN: Export all vnodes' pointers and struct copies via sysctl; when where is NULL, only return the required buffer size */
int
sysctl_vnode(where, sizep, p)
	char *where;
	size_t *sizep;
	struct process *p;
{
	struct mount *mp, *nmp;
	struct vnode *nvp, *vp;
	char *bp = where, *savebp;
	char *ewhere;
	int error;

#define VPTRSZ	sizeof (struct vnode *)
#define VNODESZ	sizeof (struct vnode)
	if (where == NULL) {
		*sizep = (numvnodes + KINFO_VNODESLOP) * (VPTRSZ + VNODESZ);
		return (0);
	}
	ewhere = where + *sizep;
		
	simple_lock(&mountlist_slock);
	for (mp = mountlist.cqh_first; mp != (void *)&mountlist; mp = nmp) {
		if (vfs_busy(mp, LK_NOWAIT, &mountlist_slock, p)) {
			nmp = mp->mnt_list.cqe_next;
			continue;
		}
		savebp = bp;
again:
		simple_lock(&mntvnode_slock);
		for (vp = mp->mnt_vnodelist.lh_first;
		     vp != NULL;
		     vp = nvp) {
			/*
			 * Check that the vp is still associated with
			 * this filesystem.  RACE: could have been
			 * recycled onto the same filesystem.
			 */
			if (vp->v_mount != mp) {
				simple_unlock(&mntvnode_slock);
				if (kinfo_vdebug)
					printf("kinfo: vp changed\n");
				bp = savebp;
				goto again;
			}
			nvp = vp->v_mntvnodes.le_next;
			if (bp + VPTRSZ + VNODESZ > ewhere) {
				simple_unlock(&mntvnode_slock);
				*sizep = bp - where;
				return (ENOMEM);
			}
			simple_unlock(&mntvnode_slock);
			if ((error = copyout((caddr_t)&vp, bp, VPTRSZ)) ||
			   (error = copyout((caddr_t)vp, bp + VPTRSZ, VNODESZ)))
				return (error);
			bp += VPTRSZ + VNODESZ;
			simple_lock(&mntvnode_slock);
		}
		simple_unlock(&mntvnode_slock);
		simple_lock(&mountlist_slock);
		nmp = mp->mnt_list.cqe_next;
		vfs_unbusy(mp, p);
	}
	simple_unlock(&mountlist_slock);

	*sizep = bp - where;
	return (0);
}

/*
 * Check to see if a filesystem is mounted on a block device.
 */
/* 中文：检查某块设备 vnode 是否被挂载（自身或其别名设置了 SI_MOUNTEDON），忙则返回 EBUSY */
/* EN: Check whether a block-device vnode is mounted on (itself or an alias has SI_MOUNTEDON), returning EBUSY if so */
int
vfs_mountedon(vp)
	struct vnode *vp;
{
	struct vnode *vq;
	int error = 0;

	if (vp->v_specflags & SI_MOUNTEDON)
		return (EBUSY);
	if (vp->v_flag & VALIASED) {
		simple_lock(&spechash_slock);
		for (vq = *vp->v_hashchain; vq; vq = vq->v_specnext) {
			if (vq->v_rdev != vp->v_rdev ||
			    vq->v_type != vp->v_type)
				continue;
			if (vq->v_specflags & SI_MOUNTEDON) {
				error = EBUSY;
				break;
			}
		}
		simple_unlock(&spechash_slock);
	}
	return (error);
}

/*
 * Unmount all filesystems. The list is traversed in reverse order
 * of mounting to avoid dependencies.
 */
/* 中文：卸载所有文件系统，按挂载的逆序遍历以避免依赖冲突 */
/* EN: Unmount all filesystems, traversing in reverse mount order to avoid dependency conflicts */
void
vfs_unmountall()
{
	struct mount *mp, *nmp;
	struct process *p = curproc;	/* XXX */

	/*
	 * Since this only runs when rebooting, it is not interlocked.
	 */
	for (mp = mountlist.cqh_last; mp != (void *)&mountlist; mp = nmp) {
		nmp = mp->mnt_list.cqe_prev;
		(void) dounmount(mp, MNT_FORCE, p);
	}
}

/*
 * Build hash lists of net addresses and hang them off the mount point.
 * Called by ufs_mount() to set up the lists of export addresses.
 */
/* 中文：根据 export_args 建立网络地址的哈希链表并挂到挂载点上，无地址时设置默认导出 */
/* EN: Build hash lists of export addresses from export_args and hang them off the mount, setting default export when no address is given */
static int
vfs_hang_addrlist(mp, nep, argp)
	struct mount *mp;
	struct netexport *nep;
	struct export_args *argp;
{
	register struct netcred *np;
	register struct radix_node_head *rnh;
	register int i;
	struct radix_node *rn;
	struct sockaddr *saddr, *smask = 0;
	struct domain *dom;
	int error;

	if (argp->ex_addrlen == 0) {
		if (mp->mnt_flag & MNT_DEFEXPORTED)
			return (EPERM);
		np = &nep->ne_defexported;
		np->netc_exflags = argp->ex_flags;
		np->netc_anon = argp->ex_anon;
		np->netc_anon.cr_ref = 1;
		mp->mnt_flag |= MNT_DEFEXPORTED;
		return (0);
	}
	i = sizeof(struct netcred) + argp->ex_addrlen + argp->ex_masklen;
	np = (struct netcred *)malloc(i, M_NETADDR, M_WAITOK);
	bzero((caddr_t)np, i);
	saddr = (struct sockaddr *)(np + 1);
	if (error = copyin(argp->ex_addr, (caddr_t)saddr, argp->ex_addrlen))
		goto out;
	if (saddr->sa_len > argp->ex_addrlen)
		saddr->sa_len = argp->ex_addrlen;
	if (argp->ex_masklen) {
		smask = (struct sockaddr *)((caddr_t)saddr + argp->ex_addrlen);
		error = copyin(argp->ex_addr, (caddr_t)smask, argp->ex_masklen);
		if (error)
			goto out;
		if (smask->sa_len > argp->ex_masklen)
			smask->sa_len = argp->ex_masklen;
	}
	i = saddr->sa_family;
	if ((rnh = nep->ne_rtable[i]) == 0) {
		/*
		 * Seems silly to initialize every AF when most are not
		 * used, do so on demand here
		 */
		for (dom = domains; dom; dom = dom->dom_next)
			if (dom->dom_family == i && dom->dom_rtattach) {
				dom->dom_rtattach((void **)&nep->ne_rtable[i],
					dom->dom_rtoffset);
				break;
			}
		if ((rnh = nep->ne_rtable[i]) == 0) {
			error = ENOBUFS;
			goto out;
		}
	}
	rn = (*rnh->rnh_addaddr)((caddr_t)saddr, (caddr_t)smask, rnh,
		np->netc_rnodes);
	if (rn == 0) {
		/*
		 * One of the reasons that rnh_addaddr may fail is that
		 * the entry already exists. To check for this case, we
		 * look up the entry to see if it is there. If so, we
		 * do not need to make a new entry but do return success.
		 */
		free(np, M_NETADDR);
		rn = (*rnh->rnh_matchaddr)((caddr_t)saddr, rnh);
		if (rn != 0 && (rn->rn_flags & RNF_ROOT) == 0 &&
		    ((struct netcred *)rn)->netc_exflags == argp->ex_flags &&
		    !bcmp((caddr_t)&((struct netcred *)rn)->netc_anon,
			    (caddr_t)&argp->ex_anon, sizeof(struct ucred)))
			return (0);
		return (EPERM);
	}
	np->netc_exflags = argp->ex_flags;
	np->netc_anon = argp->ex_anon;
	np->netc_anon.cr_ref = 1;
	return (0);
out:
	free(np, M_NETADDR);
	return (error);
}

/* 中文：从 radix 树中删除一个网络凭证节点并释放其内存（walktree 回调） */
/* EN: Delete one network-credential node from the radix tree and free it (walktree callback) */
/* ARGSUSED */
static int
vfs_free_netcred(rn, w)
	struct radix_node *rn;
	caddr_t w;
{
	register struct radix_node_head *rnh = (struct radix_node_head *)w;

	(*rnh->rnh_deladdr)(rn->rn_key, rn->rn_mask, rnh);
	free((caddr_t)rn, M_NETADDR);
	return (0);
}

/*
 * Free the net address hash lists that are hanging off the mount points.
 */
/* 中文：释放挂在挂载点上的所有网络地址哈希链表 */
/* EN: Free all network-address hash lists hanging off a mount point */
static void
vfs_free_addrlist(nep)
	struct netexport *nep;
{
	register int i;
	register struct radix_node_head *rnh;

	for (i = 0; i <= AF_MAX; i++)
		if (rnh = nep->ne_rtable[i]) {
			(*rnh->rnh_walktree)(rnh, vfs_free_netcred,
			    (caddr_t)rnh);
			free((caddr_t)rnh, M_RTABLE);
			nep->ne_rtable[i] = 0;
		}
}

/* 中文：根据标志设置或删除文件系统的 NFS 导出属性，更新导出地址链表 */
/* EN: Set or clear a filesystem's NFS export attributes per flags, updating the export address lists */
int
vfs_export(mp, nep, argp)
	struct mount *mp;
	struct netexport *nep;
	struct export_args *argp;
{
	int error;

	if (argp->ex_flags & MNT_DELEXPORT) {
		vfs_free_addrlist(nep);
		mp->mnt_flag &= ~(MNT_EXPORTED | MNT_DEFEXPORTED);
	}
	if (argp->ex_flags & MNT_EXPORTED) {
		if (error = vfs_hang_addrlist(mp, nep, argp))
			return (error);
		mp->mnt_flag |= MNT_EXPORTED;
	}
	return (0);
}

/* 中文：在导出列表中查找与请求地址匹配的网络凭证，未匹配时使用默认导出 */
/* EN: Look up a network credential matching the request address in the export list, falling back to the default export */
struct netcred *
vfs_export_lookup(mp, nep, nam)
	register struct mount *mp;
	struct netexport *nep;
	struct mbuf *nam;
{
	register struct netcred *np;
	register struct radix_node_head *rnh;
	struct sockaddr *saddr;

	np = NULL;
	if (mp->mnt_flag & MNT_EXPORTED) {
		/*
		 * Lookup in the export list first.
		 */
		if (nam != NULL) {
			saddr = mtod(nam, struct sockaddr *);
			rnh = nep->ne_rtable[saddr->sa_family];
			if (rnh != NULL) {
				np = (struct netcred *)
					(*rnh->rnh_matchaddr)((caddr_t)saddr,
							      rnh);
				if (np && np->netc_rnodes->rn_flags & RNF_ROOT)
					np = NULL;
			}
		}
		/*
		 * If no address match, use the default if it exists.
		 */
		if (np == NULL && mp->mnt_flag & MNT_DEFEXPORTED)
			np = &nep->ne_defexported;
	}
	return (np);
}
