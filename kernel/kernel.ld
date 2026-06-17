/* kernel/kernel.ld
 * Linker script for the NEXUS kernel (Phase 2a).
 *
 * The kernel is loaded by the bootloader as a FLAT binary at physical 0x10000
 * and run there directly (the low 1 GiB is identity-mapped). So we link at
 * 0x10000 with kernel_entry first. This low/flat base is provisional — see
 * docs/decisions/ADR-005. When the Phase 2b VMM lands the kernel will move to a
 * higher-half virtual base and be loaded as a proper ELF.
 */

ENTRY(kernel_entry)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text.boot)          /* kernel_entry must be the very first byte */
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }
    .bss    : { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr) }
}
