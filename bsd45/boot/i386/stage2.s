/*
 * stage2.s - 32-bit protected mode ELF loader, linked at 0x90000.
 *
 * The loader itself sits above the kernel image and its .bss, and the
 * ELF-header snapshot it needs while copying segments lives at 0x300000
 * (above 1 MB), so growing kernel tables can never collide with either.
 * 加载器本身位于内核镜像与 .bss 之上，装载期间需要的 ELF 头快照放在
 * 1MB 以上的 0x300000，内核表再长也不会撞到这两者。
 *
 * Parses the ELF32 image staged at 0x10000 by stage 1, copies every
 * PT_LOAD segment to its physical address (p_vaddr - KERNBASE), zeroes
 * bss, and enters the 4.4BSD locore entry point.
 */

	.code32
	.text
	.globl	_start32
_start32:
	cli
	/*
	 * Load the flat selectors before touching memory: stage 1 hands over
	 * with ds/es still holding real-mode 0, which is the null descriptor
	 * here, so the first memory reference would raise #GP.
	 */
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss
	movl	$0x9F000, %esp

	movb	$0x34, %al
	outb	%al, $0xE9

	/*
	 * Early serial checkpoint.  QEMU's -debugcon catches 0xE9, but VMware
	 * has no debugcon; its file-backed 16550 does work, so every stage-2
	 * checkpoint is mirrored to COM1 as well.  This is the only way to see
	 * anything before the kernel console exists on a VMware guest.
	 * 32-bit flat mode, so the port programming is a plain outb sequence.
	 */
	movw	$0x3F9, %dx
	xorl	%eax, %eax
	outb	%al, %dx		/* IER = 0 */
	movw	$0x3FB, %dx
	movb	$0x80, %al
	outb	%al, %dx		/* LCR = DLAB */
	movw	$0x3F8, %dx
	movb	$0x01, %al
	outb	%al, %dx		/* divisor low = 1 (115200) */
	movw	$0x3F9, %dx
	xorl	%eax, %eax
	outb	%al, %dx		/* divisor high = 0 */
	movw	$0x3FB, %dx
	movb	$0x03, %al
	outb	%al, %dx		/* 8N1, DLAB off */
	movw	$0x3FA, %dx
	movb	$0xC7, %al
	outb	%al, %dx		/* FIFO on, clear */
	movw	$0x3FC, %dx
	movb	$0x03, %al
	outb	%al, %dx		/* DTR | RTS */
	movl	$s2msg, %esi
	call	sputs

	/* snapshot the ELF header: the copy in the staging area is overwritten
	 * while segments are copied.  It lives at 0x300000, above 1 MB, where
	 * neither the kernel image nor its .bss can ever reach it. */
	movl	$0x10000, %esi
	movl	$0x300000, %edi
	movl	$1024, %ecx
	cld
	rep	movsl

	movb	$0x61, %al
	outb	%al, $0xE9
	cmpl	$0x464C457F, 0x300000
	jne	bad

	movl	0x30001C, %ebx		/* e_phoff */
	addl	$0x300000, %ebx
	movzwl	0x30002C, %ecx		/* e_phnum */
	movl	0x300018, %ebp		/* e_entry */
	subl	$0xFE000000, %ebp	/* virtual -> physical */

ph_loop:
	testl	%ecx, %ecx
	jz	ph_done
	cmpl	$1, (%ebx)
	jne	ph_next

	movl	4(%ebx), %esi		/* p_offset */
	addl	$0x10000, %esi
	movl	8(%ebx), %edi		/* p_vaddr */
	subl	$0xFE000000, %edi
	movl	0x10(%ebx), %edx	/* p_filesz */
	pushl	%ecx
	movl	%edx, %ecx
	rep	movsb

	movl	0x14(%ebx), %ecx	/* p_memsz */
	subl	%edx, %ecx
	xorl	%eax, %eax
	rep	stosb
	popl	%ecx

ph_next:
	addl	$32, %ebx
	decl	%ecx
	jmp	ph_loop
ph_done:

	movb	$0x35, %al
	outb	%al, $0xE9
	movl	$s2done, %esi
	call	sputs
	/* "ELF kernel loaded" on the VGA text buffer */
	movl	$0xB8000, %edi
	movl	$0x07420742, %eax
	movl	%eax, 0(%edi)

	movl	$0x9F000, %esp
	pushl	$0		/* cyloffset */
	pushl	$0		/* unit */
	pushl	$0		/* bootdev */
	pushl	$0		/* howto */
	pushl	$0		/* return address */
	jmp	*%ebp

bad:
	movb	$0x45, %al
	outb	%al, $0xE9
	movw	$0x3F8, %dx
	movl	$s2bad, %esi
	call	sputs
	movl	$0xB8000, %edi
	movl	$0x0C420C42, %eax
	movl	%eax, 0(%edi)
	cli
1:	hlt
	jmp	1b

/*
 * sputs - polled COM1 output of the NUL-terminated string at %esi.
 * Only %eax, %edx and %esi are clobbered, so it is safe inside the
 * PHDR loop-free parts of the loader.
 */
sputs:
	movw	$0x3F8, %dx
1:	movb	(%esi), %al
	testb	%al, %al
	jz	2f
	outb	%al, %dx
	incl	%esi
	jmp	1b
2:	ret

	/* all strings live in .text: the build keeps only that section */
s2msg:	.asciz	"S2: protected mode, ELF staged\r\n"
s2done:	.asciz	"S2: ELF loaded, jumping to kernel\r\n"
s2bad:	.asciz	"S2: bad ELF magic, halted\r\n"
