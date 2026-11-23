/*
 * ap_tramp.s - AP startup trampoline for BSD45 i386 SMP (S412).
 *
 * This file is linked as a RAW binary at physical 0x7000 (not as part of
 * the kernel image), because the AP starts in real mode where absolute
 * addresses are physical.  The BSP copies the blob to 0x7000, patches the
 * data block at AP_TRAMP_DATA_OFF, then sends INIT-SIPI-SIPI.
 *
 * Flow: real mode -> load BSP GDT (physical base) -> protected mode ->
 * load the kernel page directory -> enable paging -> set ESP to a kernel
 * stack -> call the kernel C entry through the patched _ap_entry pointer.
 */

	.code16
	.globl	_ap_tramp_start
_ap_tramp_start:
	cli
	cld
	xorl	%eax, %eax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss

	/* GDT pseudo-descriptor lives in the data block at 0x7000+0x300. */
	lgdt	0x7300

	/* enter protected mode, paging still off */
	movl	%cr0, %eax
	orl	$0x1, %eax
	movl	%eax, %cr0

	/* far jump to the 32-bit stub: selector 0x08 */
	.byte	0x66, 0xea
	.long	_ap_prot32
	.word	0x0008

	.align	4, 0x90
_ap_prot32:
	.code32
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	%ax, %fs
	movw	%ax, %gs

	/* kernel page directory (physical, patched at 0x7306) */
	movl	0x7306, %eax
	movl	%eax, %cr3

	/* enable paging | protected mode */
	movl	%cr0, %eax
	orl	$0x80000001, %eax
	movl	%eax, %cr0

	/* AP kernel stack top (VA, patched at 0x730a) */
	movl	0x730a, %esp

	/* ap_start(cpu_id) through the patched entry pointer at 0x7312 */
	pushl	0x730e
	call	*0x7312

1:	hlt
	jmp	1b

	.org	0x200
	.globl	_ap_gdt
_ap_gdt:
	.quad	0			/* null */
	.word	0xffff, 0x0000		/* code: base 0, limit 4G */
	.byte	0x00, 0x9a, 0xcf, 0x00
	.word	0xffff, 0x0000		/* data: base 0, limit 4G */
	.byte	0x00, 0x92, 0xcf, 0x00

/*
 * Data block.  Offsets are part of the ABI with smp.c:
 *   0x7300  gdtr: 2-byte limit + 4-byte physical base (points at 0x7200)
 *   0x7306  page directory physical address
 *   0x730a  AP stack top (virtual)
 *   0x730e  cpu id
 *   0x7312  ap_start() virtual address
 */
	.org	0x300
	.globl	_ap_gdtr
_ap_gdtr:	.word	23
		.long	0x7200
	.globl	_ap_pd
_ap_pd:		.long	0
	.globl	_ap_stack
_ap_stack:	.long	0
	.globl	_ap_cpuid
_ap_cpuid:	.long	0
	.globl	_ap_entry
_ap_entry:	.long	0

	.globl	_ap_tramp_end
_ap_tramp_end:
