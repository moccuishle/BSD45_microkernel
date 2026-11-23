/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Rick Macklem at The University of Guelph.
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
 *	@(#)nfs_node.c	8.6 (Berkeley) 5/22/95
 */


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kprocess.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/kernel.h>
#include <sys/malloc.h>

#include <nfs/rpcv2.h>
#include <nfs/nfsproto.h>
#include <nfs/nfs.h>
#include <nfs/nfsnode.h>
#include <nfs/nfsmount.h>
#include <nfs/nqnfs.h>

LIST_HEAD(nfsnodehashhead, nfsnode) *nfsnodehashtbl;
u_long nfsnodehash;

#define TRUE	1
#define	FALSE	0

/*
 * Initialize hash links for nfsnodes
 * and build nfsnode free list.
 */
/* EN: Initializes the nfsnode hash table: warns if sizeof(struct nfsnode) is not a power of two, then allocates desiredvnodes buckets via hashinit into nfsnodehashtbl/nfsnodehash. */
/* 中文：初始化 nfsnode 哈希表：若 sizeof(struct nfsnode) 不是 2 的幂就打印告警，然后用 hashinit 按 desiredvnodes 分配桶数组，存入 nfsnodehashtbl 与 nfsnodehash。 */
void
nfs_nhinit()
{

#ifndef lint
	if ((sizeof(struct nfsnode) - 1) & sizeof(struct nfsnode))
		printf("nfs_nhinit: bad size %d\n", sizeof(struct nfsnode));
#endif /* not lint */
	nfsnodehashtbl = hashinit(desiredvnodes, M_NFSNODE, &nfsnodehash);
}

/*
 * Compute an entry in the NFS hash table structure
 */
/* EN: Sums the fhsize bytes of a file handle into a u_long checksum, which callers reduce modulo the hash size via NFSNOHASH; the hash is purely additive. */
/* 中文：把文件句柄的 fhsize 个字节累加成 u_long 校验和，调用者再用 NFSNOHASH 对桶数取模；该哈希只是简单加和。 */
u_long
nfs_hash(fhp, fhsize)
	register nfsfh_t *fhp;
	int fhsize;
{
	register u_char *fhpp;
	register u_long fhsum;
	register int i;

	fhpp = &fhp->fh_bytes[0];
	fhsum = 0;
	for (i = 0; i < fhsize; i++)
		fhsum += *fhpp++;
	return (fhsum);
}

/*
 * Look up a vnode/nfsnode by file handle.
 * Callers must check for mount points!!
 * In all cases, a pointer to a
 * nfsnode structure is returned.
 */
/* EN: Returns the nfsnode for (mount, file handle): vget's the hashed one, else allocates a vnode and nfsnode, hashes it and copies the handle; sets *npp, returns 0 or an error. */
/* 中文：取得 (挂载点, 文件句柄) 对应的 nfsnode：命中则 vget 后直接返回，否则分配 vnode 与 nfsnode、插入哈希表并复制句柄；结果写入 *npp，返回 0 或错误码。 */
int
nfs_nget(mntp, fhp, fhsize, npp)
	struct mount *mntp;
	register nfsfh_t *fhp;
	int fhsize;
	struct nfsnode **npp;
{
	struct process *p = curproc;	/* XXX */
	struct nfsnode *np;
	struct nfsnodehashhead *nhpp;
	register struct vnode *vp;
	extern int (**nfsv2_vnodeop_p)();
	struct vnode *nvp;
	int error;

	nhpp = NFSNOHASH(nfs_hash(fhp, fhsize));
loop:
	for (np = nhpp->lh_first; np != 0; np = np->n_hash.le_next) {
		if (mntp != NFSTOV(np)->v_mount || np->n_fhsize != fhsize ||
		    bcmp((caddr_t)fhp, (caddr_t)np->n_fhp, fhsize))
			continue;
		vp = NFSTOV(np);
		if (vget(vp, LK_EXCLUSIVE, p))
			goto loop;
		*npp = np;
		return(0);
	}
	error = getnewvnode(VT_NFS, mntp, nfsv2_vnodeop_p, &nvp);
	if (error) {
		*npp = 0;
		return (error);
	}
	vp = nvp;
	MALLOC(np, struct nfsnode *, sizeof *np, M_NFSNODE, M_WAITOK);
	bzero((caddr_t)np, sizeof *np);
	vp->v_data = np;
	np->n_vnode = vp;
	/*
	 * Insert the nfsnode in the hash queue for its new file handle
	 */
	LIST_INSERT_HEAD(nhpp, np, n_hash);
	if (fhsize > NFS_SMALLFH) {
		MALLOC(np->n_fhp, nfsfh_t *, fhsize, M_NFSBIGFH, M_WAITOK);
	} else
		np->n_fhp = &np->n_fh;
	bcopy((caddr_t)fhp, (caddr_t)np->n_fhp, fhsize);
	np->n_fhsize = fhsize;
	*npp = np;
	return (0);
}

/* EN: VOP_INACTIVE: flushes and removes a pending sillyrename file, freeing it, its cred and dvp reference; then keeps only persistent flags, unlocks the vnode and returns 0. */
/* 中文：VOP_INACTIVE：若有待处理的 sillyrename 文件，先刷新缓冲并调用 nfs_removeit 删除，再释放其 cred、dvp 引用和结构；随后只保留持久标志并解锁 vnode，返回 0。 */
int
nfs_inactive(ap)
	struct vop_inactive_args /* {
		struct vnode *a_vp;
		struct process *a_p;
	} */ *ap;
{
	register struct nfsnode *np;
	register struct sillyrename *sp;
	struct process *p = curproc;	/* XXX */
	extern int prtactive;

	np = VTONFS(ap->a_vp);
	if (prtactive && ap->a_vp->v_usecount != 0)
		vprint("nfs_inactive: pushing active", ap->a_vp);
	if (ap->a_vp->v_type != VDIR)
		sp = np->n_sillyrename;
	else
		sp = (struct sillyrename *)0;
	np->n_sillyrename = (struct sillyrename *)0;
	if (sp) {
		/*
		 * Remove the silly file that was rename'd earlier
		 */
		(void) nfs_vinvalbuf(ap->a_vp, 0, sp->s_cred, p, 1);
		nfs_removeit(sp);
		crfree(sp->s_cred);
		vrele(sp->s_dvp);
		FREE((caddr_t)sp, M_NFSREQ);
	}
	np->n_flag &= (NMODIFIED | NFLUSHINPROG | NFLUSHWANT | NQNFSEVICTED |
		NQNFSNONCACHE | NQNFSWRITE);
	VOP_UNLOCK(ap->a_vp, 0, ap->a_p);
	return (0);
}

/*
 * Reclaim an nfsnode so that it can be used for other purposes.
 */
/* EN: VOP_RECLAIM: unhashes the nfsnode, removes it from the NQNFS timer queue, frees dir cookies and any large fh, purges name cache, frees the nfsnode, clears v_data; returns 0. */
/* 中文：VOP_RECLAIM：把 nfsnode 从哈希表摘除、移出 NQNFS 定时队列，释放目录 cookie 与大句柄缓冲，清理名字缓存后释放 nfsnode 并清空 v_data，返回 0。 */
int
nfs_reclaim(ap)
	struct vop_reclaim_args /* {
		struct vnode *a_vp;
	} */ *ap;
{
	register struct vnode *vp = ap->a_vp;
	register struct nfsnode *np = VTONFS(vp);
	register struct nfsmount *nmp = VFSTONFS(vp->v_mount);
	register struct nfsdmap *dp, *dp2;
	extern int prtactive;

	if (prtactive && vp->v_usecount != 0)
		vprint("nfs_reclaim: pushing active", vp);

	LIST_REMOVE(np, n_hash);

	/*
	 * For nqnfs, take it off the timer queue as required.
	 */
	if ((nmp->nm_flag & NFSMNT_NQNFS) && np->n_timer.cqe_next != 0) {
		CIRCLEQ_REMOVE(&nmp->nm_timerhead, np, n_timer);
	}

	/*
	 * Free up any directory cookie structures and
	 * large file handle structures that might be associated with
	 * this nfs node.
	 */
	if (vp->v_type == VDIR) {
		dp = np->n_cookies.lh_first;
		while (dp) {
			dp2 = dp;
			dp = dp->ndm_list.le_next;
			FREE((caddr_t)dp2, M_NFSDIROFF);
		}
	}
	if (np->n_fhsize > NFS_SMALLFH) {
		FREE((caddr_t)np->n_fhp, M_NFSBIGFH);
	}

	cache_purge(vp);
	FREE(vp->v_data, M_NFSNODE);
	vp->v_data = (void *)0;
	return (0);
}

/*
 * Nfs abort op, called after namei() when a CREATE/DELETE isn't actually
 * done. Currently nothing to do.
 */
/* ARGSUSED */
/* EN: VOP_ABORTOP: frees the namei buffer cn_pnbuf (M_NAMEI) when HASBUF is set without SAVESTART, so an aborted lookup/create does not leak it; always returns 0. */
/* 中文：VOP_ABORTOP：当 cn_flags 含 HASBUF 但不含 SAVESTART 时，释放 namei 缓冲区 cn_pnbuf（M_NAMEI），避免中止的查找/创建泄漏内存；总是返回 0。 */
int
nfs_abortop(ap)
	struct vop_abortop_args /* {
		struct vnode *a_dvp;
		struct componentname *a_cnp;
	} */ *ap;
{

	if ((ap->a_cnp->cn_flags & (HASBUF | SAVESTART)) == HASBUF)
		FREE(ap->a_cnp->cn_pnbuf, M_NAMEI);
	return (0);
}
