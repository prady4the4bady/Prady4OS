/* kernel/kernel.ld
 * Higher-half NEXUS kernel (Phase 2b VMM, ADR-005 superseded by ADR-007).
 *
 * The kernel runs at the conventional higher-half virtual base
 * 0xFFFFFFFF80000000, but is still loaded by the bootloader at physical
 * 0x10000. So sections are linked at the high VMA while their LMA (used by
 * `objcopy -O binary`) is the physical load address — the bootloader maps
 * VBASE -> 0x10000 (4 KiB pages) and jumps high. A low identity map is kept so
 * the PMM/heap can still dereference physical frames directly.
 *
 * __bss_start/__bss_end bound the .bss the entry stub zeroes (it is NOLOAD, so
 * not present in the flat image).
 */

ENTRY(kernel_entry)

KERNEL_VBASE = 0xFFFFFFFF80000000;
KERNEL_LMA   = 0x10000;

SECTIONS
{
    . = KERNEL_VBASE;

    .text ALIGN(16) : AT(ADDR(.text) - KERNEL_VBASE + KERNEL_LMA) {
        *(.text.boot)              /* kernel_entry first */
        *(.text .text.*)
    }
    .rodata ALIGN(16) : AT(ADDR(.rodata) - KERNEL_VBASE + KERNEL_LMA) {
        *(.rodata .rodata.*)
    }
    .data ALIGN(16) : AT(ADDR(.data) - KERNEL_VBASE + KERNEL_LMA) {
        *(.data .data.*)
    }
    .bss ALIGN(16) (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr) }
}
