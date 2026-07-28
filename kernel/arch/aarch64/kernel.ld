/* aarch64 link script (ADR-034). QEMU virt RAM starts at 0x40000000; 0x40080000
 * is the conventional load offset and leaves the low 512 KiB clear. `-kernel`
 * with an ELF honours these addresses and the entry point. */
ENTRY(_start)

SECTIONS
{
    . = 0x40080000;

    .text : {
        KEEP(*(.text.boot))     /* _start must be first — it is the entry */
        *(.text .text.*)
    }

    .rodata : ALIGN(8)  { *(.rodata .rodata.*) }
    .data   : ALIGN(8)  { *(.data .data.*) }

    /* The boot stack lives INSIDE .bss deliberately. Reserving it in the outer
     * scope let the 64 KiB land in file-backed space, so the ELF was 67 KiB for
     * ~1.5 KiB of code. .bss is NOBITS, so here it costs nothing on disk and
     * boot.S still zeroes it. SP must be 16-byte aligned on AArch64 (PCS), so
     * __stack_top is aligned explicitly rather than assumed. */
    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __stack_bottom = .;
        . += 0x10000;
        . = ALIGN(16);
        __stack_top = .;
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
