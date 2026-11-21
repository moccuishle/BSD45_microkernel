/*
 * stage1.s - El Torito no-emulation boot sector for BSD45-ELF-x86.
 *
 * Runs in 16-bit real mode at 0x7C00.  Reads the ELF kernel from a fixed
 * LBA (recorded in the manifest by mkiso.ps1), copies the 32-bit stage 2
 * to 0x90000, enables A20 and protected mode, and jumps to stage 2.
 *
 * Stage 2 must sit above the staged image (the kernel file itself is read
 * to 0x10000, so it has to end below 0x90000); the kernel is *copied* by
 * stage 2 to physical 0x101000, and the ELF-header snapshot stage 2 needs
 * lives at 0x600000, far above 1 MB.
 * stage2 位于暂存镜像之上（内核文件读到 0x10000，所以必须结束在 0x90000 以下）；
 * 内核由 stage2 搬到物理 0x101000，ELF 头快照在 1MB 以上的 0x600000。
 */

	.code16
	.text
	.globl	_start

#define	STAGE2_ADDR	0x90000
#define	STAGE2_OFF	0x200

_start:
	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	$0x7C00, %sp
	cld
	movb	%dl, boot_drive
	movb	$0x31, %al
	outb	%al, $0xE9

	/*
	 * Ask the BIOS how much RAM the machine has, while we are still in
	 * real mode: INT 15h AX=E801 returns AX = KB between 1 MB and 16 MB,
	 * BX = 64 KB blocks above 16 MB.  CMOS 0x17/0x18 cannot do this - they
	 * saturate at 64 MB - so a VMware guest with 512 MB would keep looking
	 * like a 64 MB machine and the kernel could never use the rest.
	 * The answer is left at 0x9000 (magic 0x4D454D31 "MEM1", then one
	 * 32-bit word holding AX in the low half and BX in the high half);
	 * the kernel reads it from KERNBASE+0x9000 in isa/machdep.c.  0x9000
	 * is free on both boot paths: the ISO one only uses 0x7E00.. for the
	 * appended stage-2 image, and the disk one loads stage 2 at 0x8000 for
	 * 4 KB - the kernel itself is staged at 0x10000 either way.
	 * 仍处于实模式时用 INT 15h AX=E801 问 BIOS 要内存大小：AX = 1MB~16MB 的
	 * KB 数，BX = 16MB 以上的 64KB 块数。CMOS 0x17/0x18 在 64MB 处饱和，做
	 * 不到这件事，512MB 的 VMware guest 会一直被当成 64MB 机器。结果写在
	 * 0x9000（魔数 0x4D454D31 "MEM1"，随后一个 32 位字：低半 AX、高半 BX），
	 * 内核在 isa/machdep.c 里从 KERNBASE+0x9000 读取。0x9000 在两条引导路径
	 * 上都空闲：ISO 路径只用 0x7E00 起放追加的 stage2 镜像，硬盘路径把 stage2
	 * 装到 0x8000 共 4KB；两种情况下内核都暂存在 0x10000。
	 */
	movl	$0, 0x9000		/* no answer yet */
	movw	$0xE801, %ax
	xorw	%bx, %bx
	int	$0x15
	jc	1f
	testw	%ax, %ax
	jnz	2f
	testw	%bx, %bx
	jnz	2f
	movw	%cx, %ax		/* some BIOSes answer in CX/DX */
	movw	%dx, %bx
2:
	movw	%ax, 0x9004
	movw	%bx, 0x9006
	movl	$0x4D454D31, 0x9000
1:

	movw	$msg_hello, %si
	call	puts

	/* find the manifest (linked into this image) */
	movl	manifest+4, %eax	/* kernel LBA */
	movl	%eax, dap_lba
	movl	manifest+8, %eax	/* kernel bytes */
	addl	$2047, %eax
	shrl	$11, %eax
	movl	%eax, remaining
	movw	$0x1000, bufseg

	movw	$msg_load, %si
	call	puts

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
	shlw	$7, %ax
	addw	%ax, bufseg
	jmp	read_loop
read_done:
	movb	$0x32, %al
	outb	%al, $0xE9

	movw	$msg_stage2, %si
	call	puts

	/* relocate stage 2 to 0x90000 (above the kernel image and .bss) */
	movw	manifest+12, %cx	/* stage 2 bytes */
	shrw	$1, %cx
	movw	$0x7C00+STAGE2_OFF, %si
	movw	$(STAGE2_ADDR >> 4), %ax
	movw	%ax, %es
	xorw	%di, %di
	rep	movsw

	/* A20 gate */
	inb	$0x92, %al
	orb	$0x02, %al
	andb	$0xFE, %al
	outb	%al, $0x92

	cli
	movb	$0x33, %al
	outb	%al, $0xE9
	lgdt	gdt_desc
	movl	%cr0, %eax
	orl	$1, %eax
	movl	%eax, %cr0
	/* 32-bit far jump to stage 2 */
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
	.long	0		/* kernel LBA */
	.long	0		/* kernel bytes */
	.long	0		/* stage 2 bytes */

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

msg_hello:	.asciz "BSD45-ELF-x86 boot\r\n"
msg_load:	.asciz "loading ELF kernel...\r\n"
msg_stage2:	.asciz "entering protected mode...\r\n"
msg_err:	.asciz "disk read error\r\n"

	.org	510
	.word	0xAA55
