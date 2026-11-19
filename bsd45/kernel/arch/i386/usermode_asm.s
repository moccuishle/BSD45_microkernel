/*
 * usermode_asm.s - ring 3 entry, int 0x80 stub and context resume.
 */

	.text

	.globl	_enter_user
	.align	4
_enter_user:
	movl	4(%esp), %eax		/* page directory */
	movl	8(%esp), %ecx		/* entry */
	movl	12(%esp), %edx		/* user esp */
	movzwl	__udatasel, %esi
	movzwl	__ucodesel, %edi
	pushl	%esi			/* user ss */
	pushl	%edx			/* user esp */
	pushfl
	popl	%ebx
	orl	$0x200, %ebx		/* IF=1: let the PIT preempt user mode */
	pushl	%ebx
	pushl	%edi			/* user cs */
	pushl	%ecx			/* eip */
	movw	%si, %ds
	movw	%si, %es
	movw	%si, %fs
	movw	%si, %gs
	movl	%eax, %cr3
	iret

	.globl	_syscall_entry
	.align	4
_syscall_entry:
	pushal
	push	%es
	push	%ds
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	pushl	%esp
	call	_syscall_dispatch
	addl	$4, %esp
	pop	%ds
	pop	%es
	popal
	iret

/*
 * void resume_user(unsigned pd, struct sysregs *r)
 *
 * Frame layout after the pushes:
 *   0 eip  4 cs  8 eflags  12 uesp  16 ss  20 user eax  24 user edi
 *   28 user esi
 */
	.globl	_resume_user
	.align	4
_resume_user:
	movl	4(%esp), %eax		/* page directory */
	movl	8(%esp), %esi		/* saved context */
	pushl	12(%esi)		/* user esi */
	pushl	8(%esi)			/* user edi */
	pushl	36(%esi)		/* user eax */
	pushl	56(%esi)		/* ss */
	pushl	52(%esi)		/* user esp */
	pushl	48(%esi)		/* eflags */
	pushl	44(%esi)		/* cs */
	pushl	40(%esi)		/* eip */
	/*
	 * 中文：先用 dx 作为临时设置段选择子，**之后再**加载用户 edx。否则
	 * "movw __udatasel,%dx" 会把刚恢复的用户 edx 低 16 位改写成段选择子，
	 * 破坏依赖 edx 传递常量/参数的代码（如 GCC 内联 syscall 用
	 * "mov %edx,%ebx" 传递 SRV_PORT=0，导致端口号变成段选择子 0x27）。
	 * EN: Load user edx AFTER setting the segments.  Using dx as a scratch
	 * register for __udatasel would otherwise clobber the restored user edx
	 * (e.g. GCC's "mov %edx,%ebx" passing SRV_PORT=0), turning the port
	 * number into the selector 0x27.
	 */
	movw	__udatasel, %dx
	movw	%dx, %ds
	movw	%dx, %es
	movw	%dx, %fs
	movw	%dx, %gs
	movl	28(%esi), %edx		/* user edx, loaded after segment setup */
	movl	24(%esi), %ebx
	movl	32(%esi), %ecx
	movl	16(%esi), %ebp
	movl	24(%esp), %edi		/* user edi */
	movl	28(%esp), %esi		/* user esi */
	testl	%eax, %eax		/* pd == 0 means same address space: keep CR3 */
	jz	1f
	movl	%eax, %cr3
1:
	movl	20(%esp), %eax		/* user eax */
	iret

/*
 * IRQ0 (PIT) entry.  Builds exactly a struct sysregs on the kernel stack:
 *   ds es edi esi ebp esp ebx edx ecx eax  eip cs eflags [uesp ss]
 * clock_irq() may rewrite the frame (and CR3) to resume another thread;
 * the iret below then returns straight to it.
 */
	.globl	_irq0_entry
	.align	4
_irq0_entry:
	pushal
	push	%es
	push	%ds
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	pushl	%esp
	call	_clock_irq
	addl	$4, %esp
	pop	%ds
	pop	%es
	popal
	iret
