/*
 * Copyright (c) 1982, 1986, 1991, 1993
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
 *	@(#)kern_subr.c	8.4 (Berkeley) 2/14/95
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kprocess.h>
#include <sys/malloc.h>
#include <sys/queue.h>

/* 中文：在内核缓冲区 cp 与 uio 描述的iovec 之间传输 n 字节，按用户/内核地址空间执行 copyin/copyout 或 bcopy 并更新 uio 状态 */
/* EN: Move n bytes between a kernel buffer cp and the iovecs described by uio, using copyin/copyout or bcopy per address space and updating uio state */
int
uiomove(cp, n, uio)
	register caddr_t cp;
	register int n;
	register struct uio *uio;
{
	register struct iovec *iov;
	u_int cnt;
	int error = 0;

#ifdef DIAGNOSTIC
	if (uio->uio_rw != UIO_READ && uio->uio_rw != UIO_WRITE)
		panic("uiomove: mode");
	if (uio->uio_segflg == UIO_USERSPACE && uio->uio_procp != curproc)
		panic("uiomove proc");
#endif
	while (n > 0 && uio->uio_resid) {
		iov = uio->uio_iov;
		cnt = iov->iov_len;
		if (cnt == 0) {
			uio->uio_iov++;
			uio->uio_iovcnt--;
			continue;
		}
		if (cnt > n)
			cnt = n;
		switch (uio->uio_segflg) {

		case UIO_USERSPACE:
		case UIO_USERISPACE:
			if (uio->uio_rw == UIO_READ)
				error = copyout(cp, iov->iov_base, cnt);
			else
				error = copyin(iov->iov_base, cp, cnt);
			if (error)
				return (error);
			break;

		case UIO_SYSSPACE:
			if (uio->uio_rw == UIO_READ)
				bcopy((caddr_t)cp, iov->iov_base, cnt);
			else
				bcopy(iov->iov_base, (caddr_t)cp, cnt);
			break;
		}
		iov->iov_base += cnt;
		iov->iov_len -= cnt;
		uio->uio_resid -= cnt;
		uio->uio_offset += cnt;
		cp += cnt;
		n -= cnt;
	}
	return (error);
}

/*
 * Give next character to user as result of read.
 */
/* 中文：把单个字符 c 作为读结果写入 uio 指向的用户/内核缓冲区， advancing uio 状态，返回 0 或 EFAULT */
/* EN: Write a single character c as a read result into uio's user/kernel buffer, advancing uio state and returning 0 or EFAULT */
int
ureadc(c, uio)
	register int c;
	register struct uio *uio;
{
	register struct iovec *iov;

	if (uio->uio_resid <= 0)
		panic("ureadc: non-positive resid");
again:
	if (uio->uio_iovcnt <= 0)
		panic("ureadc: non-positive iovcnt");
	iov = uio->uio_iov;
	if (iov->iov_len <= 0) {
		uio->uio_iovcnt--;
		uio->uio_iov++;
		goto again;
	}
	switch (uio->uio_segflg) {

	case UIO_USERSPACE:
		if (subyte(iov->iov_base, c) < 0)
			return (EFAULT);
		break;

	case UIO_SYSSPACE:
		*iov->iov_base = c;
		break;

	case UIO_USERISPACE:
		if (suibyte(iov->iov_base, c) < 0)
			return (EFAULT);
		break;
	}
	iov->iov_base++;
	iov->iov_len--;
	uio->uio_resid--;
	uio->uio_offset++;
	return (0);
}

#ifdef vax	/* unused except by ct.c, other oddities XXX */
/*
 * Get next character written in by user from uio.
 */
/* 中文：从 uio 指向的用户/内核缓冲区读取单个字符作为写入内容，推进 uio 状态，返回该字符或 -1（仅 vax） */
/* EN: Read a single character from uio's user/kernel buffer as written input, advancing uio state and returning the character or -1 (vax only) */
int
uwritec(uio)
	struct uio *uio;
{
	register struct iovec *iov;
	register int c;

	if (uio->uio_resid <= 0)
		return (-1);
again:
	if (uio->uio_iovcnt <= 0)
		panic("uwritec: non-positive iovcnt");
	iov = uio->uio_iov;
	if (iov->iov_len == 0) {
		uio->uio_iov++;
		if (--uio->uio_iovcnt == 0)
			return (-1);
		goto again;
	}
	switch (uio->uio_segflg) {

	case UIO_USERSPACE:
		c = fubyte(iov->iov_base);
		break;

	case UIO_SYSSPACE:
		c = *(u_char *) iov->iov_base;
		break;

	case UIO_USERISPACE:
		c = fuibyte(iov->iov_base);
		break;
	}
	if (c < 0)
		return (-1);
	iov->iov_base++;
	iov->iov_len--;
	uio->uio_resid--;
	uio->uio_offset++;
	return (c);
}
#endif /* vax */

/*
 * General routine to allocate a hash table.
 */
/* 中文：为预期 elements 个元素分配并初始化一个哈希表，表大小为大于等于 elements 的最小 2 的幂，返回掩码写入 *hashmask */
/* EN: Allocate and initialize a hash table sized to the smallest power of two >= elements, returning the table and writing its mask to *hashmask */
void *
hashinit(elements, type, hashmask)
	int elements, type;
	u_long *hashmask;
{
	long hashsize;
	LIST_HEAD(generic, generic) *hashtbl;
	int i;

	if (elements <= 0)
		panic("hashinit: bad cnt");
	for (hashsize = 1; hashsize <= elements; hashsize <<= 1)
		continue;
	hashsize >>= 1;
	hashtbl = malloc((u_long)hashsize * sizeof(*hashtbl), type, M_WAITOK);
	for (i = 0; i < hashsize; i++)
		LIST_INIT(&hashtbl[i]);
	*hashmask = hashsize - 1;
	return (hashtbl);
}
