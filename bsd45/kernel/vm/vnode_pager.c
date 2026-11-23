/*
 * Copyright (c) 1990 University of Utah.
 * Copyright (c) 1991, 1993
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
 *	@(#)vnode_pager.c	8.10 (Berkeley) 5/14/95
 */

/*
 * Page to/from files (vnodes).
 *
 * TODO:
 *	pageouts
 *	fix credential use (uses current process credentials now)
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kprocess.h>
#include <sys/malloc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/mount.h>

#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vnode_pager.h>

struct pagerlst	vnode_pager_list;	/* list of managed vnodes */

#ifdef DEBUG
int	vpagerdebug = 0x00;
#define	VDB_FOLLOW	0x01
#define VDB_INIT	0x02
#define VDB_IO		0x04
#define VDB_FAIL	0x08
#define VDB_ALLOC	0x10
#define VDB_SIZE	0x20
#endif

static vm_pager_t	 vnode_pager_alloc
			    __P((caddr_t, vm_size_t, vm_prot_t, vm_offset_t));
static void		 vnode_pager_cluster
			    __P((vm_pager_t, vm_offset_t,
				 vm_offset_t *, vm_offset_t *));
static void		 vnode_pager_dealloc __P((vm_pager_t));
static int		 vnode_pager_getpage
			    __P((vm_pager_t, vm_page_t *, int, boolean_t));
static boolean_t	 vnode_pager_haspage __P((vm_pager_t, vm_offset_t));
static void		 vnode_pager_init __P((void));
static int		 vnode_pager_io
			    __P((vn_pager_t, vm_page_t *, int,
				 boolean_t, enum uio_rw));
static boolean_t	 vnode_pager_putpage
			    __P((vm_pager_t, vm_page_t *, int, boolean_t));

struct pagerops vnodepagerops = {
	vnode_pager_init,
	vnode_pager_alloc,
	vnode_pager_dealloc,
	vnode_pager_getpage,
	vnode_pager_putpage,
	vnode_pager_haspage,
	vnode_pager_cluster
};

/* 中文：初始化 vnode 分页器：建立受管理分页器链表的头 */
/* EN: Initialize the vnode pager by setting up the head of the managed-pager list */
static void
vnode_pager_init()
{
#ifdef DEBUG
	if (vpagerdebug & VDB_FOLLOW)
		printf("vnode_pager_init()\n");
#endif
	TAILQ_INIT(&vnode_pager_list);
}

/*
 * Allocate (or lookup) pager for a vnode.
 * Handle is a vnode pointer.
 */
/* 中文：为 vnode 分配或查找分页器：新建 pager 与 vn_pager 结构及对应 vm_object，或查找已存在的对象 */
/* EN: Allocate or look up a pager for a vnode: create the pager and vn_pager structures plus a vm_object, or find the existing object */
static vm_pager_t
vnode_pager_alloc(handle, size, prot, foff)
	caddr_t handle;
	vm_size_t size;
	vm_prot_t prot;
	vm_offset_t foff;
{
	register vm_pager_t pager;
	register vn_pager_t vnp;
	vm_object_t object;
	struct vattr vattr;
	struct vnode *vp;
	struct process *p = curproc;	/* XXX */

#ifdef DEBUG
	if (vpagerdebug & (VDB_FOLLOW|VDB_ALLOC))
		printf("vnode_pager_alloc(%x, %x, %x)\n", handle, size, prot);
#endif
	/*
	 * Pageout to vnode, no can do yet.
	 */
	if (handle == NULL)
		return(NULL);

	/*
	 * Vnodes keep a pointer to any associated pager so no need to
	 * lookup with vm_pager_lookup.
	 */
	vp = (struct vnode *)handle;
	pager = (vm_pager_t)vp->v_vmdata;
	if (pager == NULL) {
		/*
		 * Allocate pager structures
		 */
		pager = (vm_pager_t)malloc(sizeof *pager, M_VMPAGER, M_WAITOK);
		if (pager == NULL)
			return(NULL);
		vnp = (vn_pager_t)malloc(sizeof *vnp, M_VMPGDATA, M_WAITOK);
		if (vnp == NULL) {
			free((caddr_t)pager, M_VMPAGER);
			return(NULL);
		}
		/*
		 * And an object of the appropriate size
		 */
		if (VOP_GETATTR(vp, &vattr, p->p_ucred, p) == 0) {
			object = vm_object_allocate(round_page(vattr.va_size));
			vm_object_enter(object, pager);
			vm_object_setpager(object, pager, 0, TRUE);
		} else {
			free((caddr_t)vnp, M_VMPGDATA);
			free((caddr_t)pager, M_VMPAGER);
			return(NULL);
		}
		/*
		 * Hold a reference to the vnode and initialize pager data.
		 */
		VREF(vp);
		vnp->vnp_flags = 0;
		vnp->vnp_vp = vp;
		vnp->vnp_size = vattr.va_size;
		TAILQ_INSERT_TAIL(&vnode_pager_list, pager, pg_list);
		pager->pg_handle = handle;
		pager->pg_type = PG_VNODE;
		pager->pg_flags = 0;
		pager->pg_ops = &vnodepagerops;
		pager->pg_data = vnp;
		vp->v_vmdata = (caddr_t)pager;
	} else {
		/*
		 * vm_object_lookup() will remove the object from the
		 * cache if found and also gain a reference to the object.
		 */
		object = vm_object_lookup(pager);
#ifdef DEBUG
		vnp = (vn_pager_t)pager->pg_data;
#endif
	}
#ifdef DEBUG
	if (vpagerdebug & VDB_ALLOC)
		printf("vnode_pager_setup: vp %x sz %x pager %x object %x\n",
		       vp, vnp->vnp_size, pager, object);
#endif
	return(pager);
}

/* 中文：释放 vnode 分页器：断开 vnode 关联、释放引用、从链表移除并释放相关结构 */
/* EN: Deallocate a vnode pager: detach the vnode, release its reference, remove it from the list and free the structures */
static void
vnode_pager_dealloc(pager)
	vm_pager_t pager;
{
	register vn_pager_t vnp = (vn_pager_t)pager->pg_data;
	register struct vnode *vp;
#ifdef NOTDEF
	struct process *p = curproc;		/* XXX */
#endif

#ifdef DEBUG
	if (vpagerdebug & VDB_FOLLOW)
		printf("vnode_pager_dealloc(%x)\n", pager);
#endif
	if (vp = vnp->vnp_vp) {
		vp->v_vmdata = NULL;
		vp->v_flag &= ~VTEXT;
#if NOTDEF
		/* can hang if done at reboot on NFS FS */
		(void) VOP_FSYNC(vp, p->p_ucred, p);
#endif
		vrele(vp);
	}
	TAILQ_REMOVE(&vnode_pager_list, pager, pg_list);
	free((caddr_t)vnp, M_VMPGDATA);
	free((caddr_t)pager, M_VMPAGER);
}

/* 中文：从 vnode 读入页面：以 UIO_READ 调用 vnode_pager_io 把文件数据读到内存页 */
/* EN: Read pages in from the vnode by calling vnode_pager_io with UIO_READ */
static int
vnode_pager_getpage(pager, mlist, npages, sync)
	vm_pager_t pager;
	vm_page_t *mlist;
	int npages;
	boolean_t sync;
{

#ifdef DEBUG
	if (vpagerdebug & VDB_FOLLOW)
		printf("vnode_pager_getpage(%x, %x, %x, %x)\n",
		       pager, mlist, npages, sync);
#endif
	return(vnode_pager_io((vn_pager_t)pager->pg_data,
			      mlist, npages, sync, UIO_READ));
}

/* 中文：把页面写回 vnode：以 UIO_WRITE 调用 vnode_pager_io，成功后将页标记为干净并清除修改位 */
/* EN: Write pages back to the vnode via vnode_pager_io with UIO_WRITE, marking them clean on success */
static boolean_t
vnode_pager_putpage(pager, mlist, npages, sync)
	vm_pager_t pager;
	vm_page_t *mlist;
	int npages;
	boolean_t sync;
{
	int err;

#ifdef DEBUG
	if (vpagerdebug & VDB_FOLLOW)
		printf("vnode_pager_putpage(%x, %x, %x, %x)\n",
		       pager, mlist, npages, sync);
#endif
	if (pager == NULL)
		return (FALSE);			/* ??? */
	err = vnode_pager_io((vn_pager_t)pager->pg_data,
			     mlist, npages, sync, UIO_WRITE);
	/*
	 * If the operation was successful, mark the pages clean.
	 */
	if (err == VM_PAGER_OK) {
		while (npages--) {
			(*mlist)->flags |= PG_CLEAN;
			pmap_clear_modify(VM_PAGE_TO_PHYS(*mlist));
			mlist++;
		}
	}
	return(err);
}

/* 中文：判断 vnode 在给定偏移处是否有数据页：先查偏移是否越界，再用 VOP_BMAP 确定磁盘块是否存在 */
/* EN: Determine whether the vnode has a page at a given offset by checking bounds and using VOP_BMAP to see if a disk block exists */
static boolean_t
vnode_pager_haspage(pager, offset)
	vm_pager_t pager;
	vm_offset_t offset;
{
	struct process *p = curproc;	/* XXX */
	vn_pager_t vnp = (vn_pager_t)pager->pg_data;
	daddr_t bn;
	int err;

#ifdef DEBUG
	if (vpagerdebug & VDB_FOLLOW)
		printf("vnode_pager_haspage(%x, %x)\n", pager, offset);
#endif

	/*
	 * Offset beyond end of file, do not have the page
	 * Lock the vnode first to make sure we have the most recent
	 * version of the size.
	 */
	vn_lock(vnp->vnp_vp, LK_EXCLUSIVE | LK_RETRY, p);
	if (offset >= vnp->vnp_size) {
		VOP_UNLOCK(vnp->vnp_vp, 0, p);
#ifdef DEBUG
		if (vpagerdebug & (VDB_FAIL|VDB_SIZE))
			printf("vnode_pager_haspage: pg %x, off %x, size %x\n",
			       pager, offset, vnp->vnp_size);
#endif
		return(FALSE);
	}

	/*
	 * Read the index to find the disk block to read
	 * from.  If there is no block, report that we don't
	 * have this data.
	 *
	 * Assumes that the vnode has whole page or nothing.
	 */
	err = VOP_BMAP(vnp->vnp_vp,
		       offset / vnp->vnp_vp->v_mount->mnt_stat.f_iosize,
		       (struct vnode **)0, &bn, NULL);
	VOP_UNLOCK(vnp->vnp_vp, 0, p);
	if (err) {
#ifdef DEBUG
		if (vpagerdebug & VDB_FAIL)
			printf("vnode_pager_haspage: BMAP err %d, pg %x, off %x\n",
			       err, pager, offset);
#endif
		return(TRUE);
	}
	return((long)bn < 0 ? FALSE : TRUE);
}

/* 中文：计算给定偏移所在页簇的起止偏移范围，供 VM 系统聚类读操作 */
/* EN: Compute the low/high offset range of the page cluster containing a given offset, for VM read clustering */
static void
vnode_pager_cluster(pager, offset, loffset, hoffset)
	vm_pager_t	pager;
	vm_offset_t	offset;
	vm_offset_t	*loffset;
	vm_offset_t	*hoffset;
{
	vn_pager_t vnp = (vn_pager_t)pager->pg_data;
	vm_offset_t loff, hoff;

#ifdef DEBUG
	if (vpagerdebug & VDB_FOLLOW)
		printf("vnode_pager_cluster(%x, %x) ", pager, offset);
#endif
	loff = offset;
	if (loff >= vnp->vnp_size)
		panic("vnode_pager_cluster: bad offset");
	/*
	 * XXX could use VOP_BMAP to get maxcontig value
	 */
	hoff = loff + MAXBSIZE;
	if (hoff > round_page(vnp->vnp_size))
		hoff = round_page(vnp->vnp_size);

	*loffset = loff;
	*hoffset = hoff;
#ifdef DEBUG
	if (vpagerdebug & VDB_FOLLOW)
		printf("returns [%x-%x]\n", loff, hoff);
#endif
}

/*
 * (XXX)
 * Lets the VM system know about a change in size for a file.
 * If this vnode is mapped into some address space (i.e. we have a pager
 * for it) we adjust our own internal size and flush any cached pages in
 * the associated object that are affected by the size change.
 *
 * Note: this routine may be invoked as a result of a pager put
 * operation (possibly at object termination time), so we must be careful.
 */
/* 中文：通知 VM 系统文件大小变化：调整内部记录的大小，并在文件缩小时丢弃新 EOF 之外的缓存页 */
/* EN: Inform the VM system that a file's size changed: adjust the recorded size and discard cached pages beyond the new EOF when the file shrinks */
void
vnode_pager_setsize(vp, nsize)
	struct vnode *vp;
	u_long nsize;
{
	register vn_pager_t vnp;
	register vm_object_t object;
	vm_pager_t pager;

	/*
	 * Not a mapped vnode
	 */
	if (vp == NULL || vp->v_type != VREG || vp->v_vmdata == NULL)
		return;
	/*
	 * Hasn't changed size
	 */
	pager = (vm_pager_t)vp->v_vmdata;
	vnp = (vn_pager_t)pager->pg_data;
	if (nsize == vnp->vnp_size)
		return;
	/*
	 * No object.
	 * This can happen during object termination since
	 * vm_object_page_clean is called after the object
	 * has been removed from the hash table, and clean
	 * may cause vnode write operations which can wind
	 * up back here.
	 */
	object = vm_object_lookup(pager);
	if (object == NULL)
		return;

#ifdef DEBUG
	if (vpagerdebug & (VDB_FOLLOW|VDB_SIZE))
		printf("vnode_pager_setsize: vp %x obj %x osz %d nsz %d\n",
		       vp, object, vnp->vnp_size, nsize);
#endif
	/*
	 * File has shrunk.
	 * Toss any cached pages beyond the new EOF.
	 */
	if (nsize < vnp->vnp_size) {
		vm_object_lock(object);
		vm_object_page_remove(object,
				      (vm_offset_t)nsize, vnp->vnp_size);
		vm_object_unlock(object);
	}
	vnp->vnp_size = (vm_offset_t)nsize;
	vm_object_deallocate(object);
}

/* 中文：卸载文件系统时对该挂载点上的所有 vnode 取消缓存并释放其分页器 */
/* EN: On unmount, uncache and release the pagers of all vnodes belonging to the given mount */
void
vnode_pager_umount(mp)
	register struct mount *mp;
{
	struct process *p = curproc;	/* XXX */
	vm_pager_t pager, npager;
	struct vnode *vp;

	for (pager = vnode_pager_list.tqh_first; pager != NULL; pager = npager){
		/*
		 * Save the next pointer now since uncaching may
		 * terminate the object and render pager invalid
		 */
		npager = pager->pg_list.tqe_next;
		vp = ((vn_pager_t)pager->pg_data)->vnp_vp;
		if (mp == (struct mount *)0 || vp->v_mount == mp) {
			vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, p);
			(void) vnode_pager_uncache(vp);
			VOP_UNLOCK(vp, 0, p);
		}
	}
}

/*
 * Remove vnode associated object from the object cache.
 *
 * XXX unlock the vnode if it is currently locked.
 * We must do this since uncaching the object may result in its
 * destruction which may initiate paging activity which may necessitate
 * re-locking the vnode.
 */
/* 中文：把 vnode 关联的对象从对象缓存中移除；必要时先解锁 vnode 以便对象销毁时的分页操作能重新加锁 */
/* EN: Remove a vnode's associated object from the object cache, unlocking the vnode if needed so paging during destruction can re-lock it */
boolean_t
vnode_pager_uncache(vp)
	register struct vnode *vp;
{
	struct process *p = curproc;	/* XXX */
	vm_object_t object;
	boolean_t uncached;
	vm_pager_t pager;

	/*
	 * Not a mapped vnode
	 */
	if (vp->v_type != VREG || (pager = (vm_pager_t)vp->v_vmdata) == NULL)
		return (TRUE);
#ifdef DEBUG
	if (!VOP_ISLOCKED(vp)) {
		extern int (**nfsv2_vnodeop_p)();

		if (vp->v_op != nfsv2_vnodeop_p)
			panic("vnode_pager_uncache: vnode not locked!");
	}
#endif
	/*
	 * Must use vm_object_lookup() as it actually removes
	 * the object from the cache list.
	 */
	object = vm_object_lookup(pager);
	if (object) {
		uncached = (object->ref_count <= 1);
		VOP_UNLOCK(vp, 0, p);
		pager_cache(object, FALSE);
		vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, p);
	} else
		uncached = TRUE;
	return(uncached);
}

/* 中文：vnode 分页的核心 I/O：把内存页映射到内核虚地址，经 VOP_READ/VOP_WRITE 在文件与页之间传输数据 */
/* EN: Core I/O routine for vnode paging: map pages into kernel virtual space and transfer data between the file and pages via VOP_READ/VOP_WRITE */
static int
vnode_pager_io(vnp, mlist, npages, sync, rw)
	register vn_pager_t vnp;
	vm_page_t *mlist;
	int npages;
	boolean_t sync;
	enum uio_rw rw;
{
	struct uio auio;
	struct iovec aiov;
	vm_offset_t kva, foff;
	int error, size;
	struct process *p = curproc;		/* XXX */

	/* XXX */
	vm_page_t m;
	if (npages != 1)
		panic("vnode_pager_io: cannot handle multiple pages");
	m = *mlist;
	/* XXX */

#ifdef DEBUG
	if (vpagerdebug & VDB_FOLLOW)
		printf("vnode_pager_io(%x, %x, %c): vnode %x\n",
		       vnp, m, rw == UIO_READ ? 'R' : 'W', vnp->vnp_vp);
#endif
	foff = m->offset + m->object->paging_offset;
	/*
	 * Allocate a kernel virtual address and initialize so that
	 * we can use VOP_READ/WRITE routines.
	 */
	kva = vm_pager_map_pages(mlist, npages, sync);
	if (kva == NULL)
		return(VM_PAGER_AGAIN);
	/*
	 * After all of the potentially blocking operations have been
	 * performed, we can do the size checks:
	 *	read beyond EOF (returns error)
	 *	short read
	 */
	vn_lock(vnp->vnp_vp, LK_EXCLUSIVE | LK_RETRY, p);
	if (foff >= vnp->vnp_size) {
		VOP_UNLOCK(vnp->vnp_vp, 0, p);
		vm_pager_unmap_pages(kva, npages);
#ifdef DEBUG
		if (vpagerdebug & VDB_SIZE)
			printf("vnode_pager_io: vp %x, off %d size %d\n",
			       vnp->vnp_vp, foff, vnp->vnp_size);
#endif
		return(VM_PAGER_BAD);
	}
	if (foff + PAGE_SIZE > vnp->vnp_size)
		size = vnp->vnp_size - foff;
	else
		size = PAGE_SIZE;
	aiov.iov_base = (caddr_t)kva;
	aiov.iov_len = size;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_offset = foff;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = rw;
	auio.uio_resid = size;
	auio.uio_procp = (struct process *)0;
#ifdef DEBUG
	if (vpagerdebug & VDB_IO)
		printf("vnode_pager_io: vp %x kva %x foff %x size %x",
		       vnp->vnp_vp, kva, foff, size);
#endif
	if (rw == UIO_READ)
		error = VOP_READ(vnp->vnp_vp, &auio, 0, p->p_ucred);
	else
		error = VOP_WRITE(vnp->vnp_vp, &auio, 0, p->p_ucred);
	VOP_UNLOCK(vnp->vnp_vp, 0, p);
#ifdef DEBUG
	if (vpagerdebug & VDB_IO) {
		if (error || auio.uio_resid)
			printf(" returns error %x, resid %x",
			       error, auio.uio_resid);
		printf("\n");
	}
#endif
	if (!error) {
		register int count = size - auio.uio_resid;

		if (count == 0)
			error = EINVAL;
		else if (count != PAGE_SIZE && rw == UIO_READ)
			bzero((void *)(kva + count), PAGE_SIZE - count);
	}
	vm_pager_unmap_pages(kva, npages);
	return (error ? VM_PAGER_ERROR : VM_PAGER_OK);
}
