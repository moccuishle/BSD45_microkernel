/*
 * smp_ipi.s - S412: IPI entry stubs for the local APIC vectors.
 *
 * The generic trap path expects a trapframe; an IPI only needs to save the
 * integer registers, call the C handler with its vector number, send EOI
 * (the handler does that) and return.  Three thin stubs keep the vector
 * number available without parsing the APIC ISR.
 *
 * S414: the data segment must be switched to the kernel selector (0x10)
 * before touching any kernel global.  resume_user() loads the user data
 * selector (0x27) into ds/es *before* its iret, and that selector's limit
 * is UPT_MIN_ADDRESS (0xFDC00000) - below the kernel image at 0xFE....  An
 * IPI that lands in that window therefore runs this handler with ds=0x27,
 * and the very first DS-relative load of a kernel variable (#fe185604,
 * lapic_va) raises #GP, which the trap printer reports as "TRAP 9".
 * save ds/es, switch to the kernel data segment, restore on the way out.
 * S414：在访问任何内核全局变量前必须把数据段切到内核选择子(0x10)。
 * resume_user() 在 iret 之前就把用户数据选择子(0x27)装进 ds/es，而该段的
 * limit 是 UPT_MIN_ADDRESS(0xFDC00000)，低于 0xFE... 的内核镜像。若 IPI 正好
 * 落在这个窗口，处理程序就是带着 ds=0x27 运行的，第一条读内核变量的指令
 * （0xfe185604=lapic_va）就触发 #GP，被陷阱打印成 "TRAP 9"。
 */
	.text

/* Save ds/es, install the kernel data selector, call the C handler. */
#define	IPI_PROLOGUE(vec)		\
	pushl	%ds;			\
	pushl	%es;			\
	pushal;				\
	movw	$0x10, %ax;		\
	movw	%ax, %ds;		\
	movw	%ax, %es;		\
	pushl	$(vec);			\
	call	_smp_handle_ipi;	\
	addl	$4, %esp;		\
	popal;				\
	popl	%es;			\
	popl	%ds;			\
	iret

	.globl	_smp_ipi_tlb
	.align	4
_smp_ipi_tlb:
	IPI_PROLOGUE(0x40)

	.globl	_smp_ipi_resched
	.align	4
_smp_ipi_resched:
	IPI_PROLOGUE(0x41)

	.globl	_smp_ipi_stop
	.align	4
_smp_ipi_stop:
	IPI_PROLOGUE(0x42)
