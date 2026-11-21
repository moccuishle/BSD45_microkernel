/*
 * kernel.ld - link the BSD45 kernel as a plain ELF32 at KERNBASE + 1 MB.
 *
 * Physical layout the boot chain assumes (stage1.s / stage2.s):
 *   0x07C00      stage 1 boot sector
 *   0x10000..    BIOS stages the ELF *file* here.  INT 13h cannot write at
 *                0xA0000 or above, so the staged file must end below
 *                0x90000 (576 KB of staging space).
 *   0x90000      running stage 2 (stack at 0x9F000)
 *   0x600000     stage 2's ELF-header snapshot (above 1 MB, clear of .bss)
 *   0x101000     kernel .text (VA 0xFE101000, identity mapped by locore)
 *
 * The image itself is *copied* by stage 2 to p_vaddr - KERNBASE, so only the
 * file has to fit in low memory: .data/.bss may now run past 0xA0000.  That
 * is what lifts the old 640 KB ceiling - before S408 the kernel was linked at
 * physical 0x1000 and .bss had to end below stage 2 at 0x90000, which capped
 * every static table (page_ref could only cover 64 MB of RAM).
 *
 * The virtual address stays inside the SYSTEM window that locore maps
 * identity (VA = KERNBASE + PA), so every p_vaddr - KERNBASE computation
 * keeps working, and pmap_bootstrap() derives the first free page (and
 * virtual_avail) from the page-rounded _end that locore passes in main().
 */
ENTRY(start)
SECTIONS
{
    . = 0xFE101000;
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
