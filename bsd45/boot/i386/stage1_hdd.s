/*
 * stage1_hdd.s - MBR / hard-disk boot sector for an installed BSD45 system.
 *
 * This is the disk counterpart of stage1.s (which boots the live ISO).
 * It is embedded as a byte array in the kernel by build_kernel.ps1 and is
 * written to LBA 0 of the VMware virtual disk by the `install` command.
 *
 * Runs in 16-bit real mode at 0x7C00 with DL = BIOS boot drive (0x80).
 * Uses INT 13h AH=42h (LBA extended read) to load:
 *   - stage2 (the ELF loader) from LBA 1 straight to 0x8000;
 *   - the raw ELF kernel from <kernel_lba> (manifest) to 0x10000.
 * Then enables A20 + protected mode and far-jumps to stage2 at 0x8000.
 *
 * All sector arithmetic is 512 bytes/sector (unlike the 2048-byte CD
 * sectors used by the ISO stage1).  The manifest is patched by install.c.
 */

	.code16
	.text
	.globl	_start

#define	STAGE2_ADDR	0x8000
#define	STAGE2_LBA	1
#define	STAGE2_SECTORS	8		/* 4 KiB, enough for the ELF loader */

_start:
	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	$0x7C00, %sp
	cld
	movb	%dl, boot_drive
	movb	$0x41, %al
	outb	%al, $0xE9

	movw	$msg_hello, %si
	call	puts

	/* stage2 lives at 0x8000, below the 0x10000 staging area, so the
	 * kernel image can never reach it - but it is still loaded last so a
	 * short read of the kernel leaves the loader intact for a retry. */

	movb	$0x42, %al
	outb	%al, $0xE9
	movw	$msg_load, %si
	call	puts

	/* ---- load the ELF kernel from <kernel_lba> into 0x10000 --------- */
	movl	manifest+4, %eax	/* kernel LBA */
	movl	%eax, dap_lba
	movl	manifest+8, %eax	/* kernel byte count */
	addl	$511, %eax
	shrl	$9, %eax		/* 512-byte sectors */
	movl	%eax, remaining
	movw	$0x1000, bufseg

read_loop:
	cmpl	$0, remaining
	je	read_done
	movl	remaining, %eax
	cmpl	$16, %eax
	jbe	1f
	movl	$16, %eax
1:
	movl	%eax, chunk
	movw	%ax, dap_count
	movw	$0, dap_off
	movw	bufseg, %ax
	movw	%ax, dap_seg
	call	read_sectors

	movl	chunk, %eax
	addl	%eax, dap_lba
	subl	%eax, remaining
	shlw	$5, %ax		/* sector count * 512 / 16 (paragraphs) */
	addw	%ax, bufseg
	jmp	read_loop
read_done:
	movb	$0x43, %al
	outb	%al, $0xE9

	/* ---- load stage2 LAST, into 0x8000 ------------------------------ */
	movl	$STAGE2_LBA, dap_lba
	movw	$STAGE2_SECTORS, dap_count
	movw	$0, dap_off
	movw	$(STAGE2_ADDR >> 4), dap_seg
	call	read_sectors

	movw	$msg_stage2, %si
	call	puts

	/* ---- A20 gate --------------------------------------------------- */
	inb	$0x92, %al
	orb	$0x02, %al
	andb	$0xFE, %al
	outb	%al, $0x92

	cli
	movb	$0x44, %al
	outb	%al, $0xE9
	lgdt	gdt_desc
	movl	%cr0, %eax
	orl	$1, %eax
	movl	%eax, %cr0
	/* 32-bit far jump to stage2 (linked at 0x8000) */
	.byte	0x66, 0xEA
	.long	STAGE2_ADDR
	.word	0x08

/* ---- INT 13h extended read: dap ---- */
read_sectors:
	movw	$3, %cx		/* retries */
retry:
	movw	$dap, %si
	movb	boot_drive, %dl
	movb	$0x42, %ah
	int	$0x13
	jnc	ok
	movw	$msg_err, %si
	call	puts
	decw	%cx
	jnz	retry
	jmp	halt
ok:
	ret

puts:
	lodsb
	testb	%al, %al
	jz	1f
	movb	$0x0E, %ah
	movw	$7, %bx
	int	$0x10
	jmp	puts
1:
	ret

halt:
	cli
	hlt
	jmp	halt

.align	8
manifest:
	.long	0x464D4442	/* "BDMF" */
	.long	0		/* kernel LBA (patched: 2048) */
	.long	0		/* kernel bytes (patched) */
	.long	0		/* stage2 bytes (patched) */

.align	8
gdt:
	.quad	0
	.quad	0x00CF9A000000FFFF
	.quad	0x00CF92000000FFFF
gdt_desc:
	.word	23
	.long	gdt

.align	8
dap:
	.word	0x10
dap_count:	.word	0
dap_off:	.word	0
dap_seg:	.word	0
dap_lba:	.long	0
	.long	0
remaining:	.long	0
chunk:		.long	0
bufseg:		.word	0
boot_drive:	.byte	0

msg_hello:	.asciz "BSD45 HDD boot\r\n"
msg_load:	.asciz "loading ELF kernel from disk...\r\n"
msg_stage2:	.asciz "entering protected mode...\r\n"
msg_err:	.asciz "disk read error\r\n"

	.org	510
	.word	0xAA55
