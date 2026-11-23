/*
 * smp_ipi.s - S412: IPI entry stubs for the local APIC vectors.
 *
 * The generic trap path expects a trapframe; an IPI only needs to save the
 * integer registers, call the C handler with its vector number, send EOI
 * (the handler does that) and return.  Three thin stubs keep the vector
 * number available without parsing the APIC ISR.
 */
	.text
	.globl	_smp_ipi_tlb
_smp_ipi_tlb:
	pushal
	pushl	$0x40
	call	_smp_handle_ipi
	addl	$4, %esp
	popal
	iret

	.globl	_smp_ipi_resched
_smp_ipi_resched:
	pushal
	pushl	$0x41
	call	_smp_handle_ipi
	addl	$4, %esp
	popal
	iret

	.globl	_smp_ipi_stop
_smp_ipi_stop:
	pushal
	pushl	$0x42
	call	_smp_handle_ipi
	addl	$4, %esp
	popal
	iret
