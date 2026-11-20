/*
 * kernel.ld - link the BSD45 kernel as a plain ELF32 at KERNBASE.
 *
 * Physical layout the boot chain assumes (stage1.s / stage2.s):
 *   0x006000     stage 2's ELF-header snapshot (written while loading)
 *   0x07C00      stage 1 boot sector
 *   0x10000..    BIOS stages the ELF image here.  INT 13h cannot write at
 *                0xA0000 or above, so the image must end below 0x90000
 *                (576 KB of staging space).
 *   0x90000      running stage 2 (stack at 0x9F000)
 *   0xFE001000   kernel .text (phys 0x1000, identity mapped by locore)
 *
 * Every file-backed section therefore has to end below 0x90000.  That is
 * affordable because the process table and the TCB pool live in their own
 * kernel VA windows (PROC_TABLE_VA / KTHREAD_TABLE_VA), so .bss holds only
 * small statics.  The virtual address stays inside the 12 MB SYSTEM window
 * that locore maps identity (VA = KERNBASE + PA), so every
 * p_vaddr - KERNBASE computation keeps working.
 */
ENTRY(start)
SECTIONS
{
    . = 0xFE001000;
    .text : {
        *(.text*)
    }
    _etext = .;
    . = ALIGN(4096);
    .data : {
        *(.data*)
        *(.rodata*)
        *(.rdata*)
    }
    _edata = .;
    . = ALIGN(4096);
    .bss : {
        *(.bss*)
        *(COMMON)
    }
    _end = .;
}
