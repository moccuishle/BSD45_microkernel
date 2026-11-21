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
	 * Read every field from struct sysregs BEFORE loading the user data
	 * selector into DS.  The proc table now lives at 0xFE340000, above
	 * the user data segment limit (btoc(UPT_MIN_ADDRESS) -> 0xFDC00000);
	 * VMware enforces that limit in ring 0 too, so any DS-relative read
	 * after "mov %dx,%ds" raises #GP (reported as TRAP 9).  Use %di as
	 * the selector scratch so user edx survives for the final restore.
	 */
	movl	28(%esi), %edx		/* user edx */
	movl	24(%esi), %ebx
	movl	32(%esi), %ecx
	movl	16(%esi), %ebp
	movw	__udatasel, %di
	movw	%di, %ds
	movw	%di, %es
	movw	%di, %fs
	movw	%di, %gs
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
