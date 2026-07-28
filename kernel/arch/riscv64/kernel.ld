/* riscv64 link script (ADR-034). OpenSBI occupies 0x80000000 and jumps to the
 * payload at 0x80200000 in S-mode. */
ENTRY(_start)

SECTIONS
{
    . = 0x80200000;

    .text : {
        KEEP(*(.text.boot))     /* _start first — it is the entry */
        *(.text .text.*)
    }

    .rodata : ALIGN(8)  { *(.rodata .rodata.*) }
    .data   : ALIGN(8)  { *(.data .data.*) }
    .sdata  : ALIGN(8)  { *(.sdata .sdata.*) }

    /* Boot stack inside .bss (NOBITS) so it costs no file space; boot.S zeroes
     * the whole region including it. */
    .bss : ALIGN(16) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __stack_bottom = .;
        . += 0x10000;           /* 64 KiB boot stack */
        . = ALIGN(16);
        __stack_top = .;
        __bss_end = .;
    }

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
