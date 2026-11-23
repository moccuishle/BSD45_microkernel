/*
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
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
 *	@(#)vm_meter.c	8.7 (Berkeley) 5/10/95
 */

#include <sys/param.h>
#include <sys/kprocess.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <vm/vm.h>
#include <sys/sysctl.h>

struct	loadavg averunnable;		/* load average, of runnable procs */

int	maxslp = MAXSLP;
int	saferss = SAFERSS;

/* 中文：虚拟内存计量定时器回调：每隔 5 秒采样一次运行队列负载平均值，并在 proc0 睡眠过久时将其唤醒 */
/* EN: Virtual memory metering timer callback: sample the run-queue load average every 5 seconds and wake proc0 if it has slept too long */
void
vmmeter()
{

	if (time.tv_sec % 5 == 0)
		loadav(&averunnable);
	if (proc0.p_slptime > maxslp/2)
		wakeup((caddr_t)&proc0);
}

/*
 * Constants for averages over 1, 5, and 15 minutes
 * when sampling at 5 second intervals.
 */
fixpt_t	cexp[3] = {
	0.9200444146293232 * FSCALE,	/* exp(-1/12) */
	0.9834714538216174 * FSCALE,	/* exp(-1/60) */
	0.9944598480048967 * FSCALE,	/* exp(-1/180) */
};

/*
 * Compute a tenex style load average of a quantity on
 * 1, 5 and 15 minute intervals.
 */
/* 中文：计算并更新运行队列的负载平均值：统计可运行进程数，并用指数加权公式刷新 1/5/15 分钟平均值 */
/* EN: Compute and update the run-queue load averages: count runnable processes and refresh the 1/5/15-minute averages with an exponential weighting formula */
void
loadav(avg)
	register struct loadavg *avg;
{
	register int i, nrun;
	register struct process *p;

	for (nrun = 0, p = allproc.lh_first; p != 0; p = p->p_list.le_next) {
		switch (p->p_stat) {
		case SSLEEP:
			if (p->p_priority > PZERO || p->p_slptime != 0)
				continue;
			/* fall through */
		case SRUN:
		case SIDL:
			nrun++;
		}
	}
	for (i = 0; i < 3; i++)
		avg->ldavg[i] = (cexp[i] * avg->ldavg[i] +
			nrun * FSCALE * (FSCALE - cexp[i])) >> FSHIFT;
}

/*
 * Attributes associated with virtual memory.
 */
/* 中文：处理虚拟内存子系统的 sysctl 请求：返回负载平均值与 vmtotal 内存统计等信息 */
/* EN: Handle sysctl requests for the virtual memory subsystem: return the load average and vmtotal memory statistics */
vm_sysctl(name, namelen, oldp, oldlenp, newp, newlen, p)
	int *name;
	u_int namelen;
	void *oldp;
	size_t *oldlenp;
	void *newp;
	size_t newlen;
	struct process *p;
{
	struct vmtotal vmtotals;

	/* all sysctl names at this level are terminal */
	if (namelen != 1)
		return (ENOTDIR);		/* overloaded */

	switch (name[0]) {
	case VM_LOADAVG:
		averunnable.fscale = FSCALE;
		return (sysctl_rdstruct(oldp, oldlenp, newp, &averunnable,
		    sizeof(averunnable)));
	case VM_METER:
		vmtotal(&vmtotals);
		return (sysctl_rdstruct(oldp, oldlenp, newp, &vmtotals,
		    sizeof(vmtotals)));
	default:
		return (EOPNOTSUPP);
	}
	/* NOTREACHED */
}

/*
 * Calculate the current state of the system.
 * Done on demand from getkerninfo().
 */
/* 中文：收集系统整体虚拟内存统计信息：遍历所有进程与 vm 对象，统计常驻、共享、私有及分页中的页数并填入 vmtotal */
/* EN: Collect overall virtual memory statistics: walk all processes and vm objects, count resident, shared, private and paging pages, and fill in a vmtotal structure */
void
vmtotal(totalp)
	register struct vmtotal *totalp;
{
	register struct process *p;
	register vm_map_entry_t	entry;
	register vm_object_t object;
	register vm_map_t map;
	int paging;

	bzero(totalp, sizeof *totalp);
	/*
	 * Mark all objects as inactive.
	 */
	simple_lock(&vm_object_list_lock);
	for (object = vm_object_list.tqh_first;
	     object != NULL;
	     object = object->object_list.tqe_next)
		object->flags &= ~OBJ_ACTIVE;
	simple_unlock(&vm_object_list_lock);
	/*
	 * Calculate process statistics.
	 */
	for (p = allproc.lh_first; p != 0; p = p->p_list.le_next) {
		if (p->p_flag & P_SYSTEM)
			continue;
		switch (p->p_stat) {
		case 0:
			continue;

		case SSLEEP:
		case SSTOP:
			if (p->p_flag & P_INMEM) {
				if (p->p_priority <= PZERO)
					totalp->t_dw++;
				else if (p->p_slptime < maxslp)
					totalp->t_sl++;
			} else if (p->p_slptime < maxslp)
				totalp->t_sw++;
			if (p->p_slptime >= maxslp)
				continue;
			break;

		case SRUN:
		case SIDL:
			if (p->p_flag & P_INMEM)
				totalp->t_rq++;
			else
				totalp->t_sw++;
			if (p->p_stat == SIDL)
				continue;
			break;
		}
		/*
		 * Note active objects.
		 *
		 * XXX don't count shadow objects with no resident pages.
		 * This eliminates the forced shadows caused by MAP_PRIVATE.
		 * Right now we require that such an object completely shadow
		 * the original, to catch just those cases.
		 */
		paging = 0;
		for (map = &p->p_vmspace->vm_map, entry = map->header.next;
		     entry != &map->header; entry = entry->next) {
			if (entry->is_a_map || entry->is_sub_map ||
			    (object = entry->object.vm_object) == NULL)
				continue;
			while (object->shadow &&
			       object->resident_page_count == 0 &&
			       object->shadow_offset == 0 &&
			       object->size == object->shadow->size)
				object = object->shadow;
			object->flags |= OBJ_ACTIVE;
			paging |= object->paging_in_progress;
		}
		if (paging)
			totalp->t_pw++;
	}
	/*
	 * Calculate object memory usage statistics.
	 */
	simple_lock(&vm_object_list_lock);
	for (object = vm_object_list.tqh_first;
	     object != NULL;
	     object = object->object_list.tqe_next) {
		totalp->t_vm += num_pages(object->size);
		totalp->t_rm += object->resident_page_count;
		if (object->flags & OBJ_ACTIVE) {
			totalp->t_avm += num_pages(object->size);
			totalp->t_arm += object->resident_page_count;
		}
		if (object->ref_count > 1) {
			/* shared object */
			totalp->t_vmshr += num_pages(object->size);
			totalp->t_rmshr += object->resident_page_count;
			if (object->flags & OBJ_ACTIVE) {
				totalp->t_avmshr += num_pages(object->size);
				totalp->t_armshr += object->resident_page_count;
			}
		}
	}
	simple_unlock(&vm_object_list_lock);
	totalp->t_free = cnt.v_free_count;
}
