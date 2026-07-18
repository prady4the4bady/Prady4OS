/* kernel/kernel.ld
 * Higher-half NEXUS kernel (Phase 2b VMM, ADR-005 superseded by ADR-007).
 *
 * The kernel runs at the conventional higher-half virtual base
 * 0xFFFFFFFF80000000, loaded by the bootloader at physical 0x400000 (4 MiB,
 * DDR-733 — stage2 bounce-loads it there via unreal mode; the pre-733 home at
 * 0x10000 was bounded by the ~575 KiB top of conventional RAM). Sections are
 * linked at the high VMA while their LMA (used by `objcopy -O binary`) is the
 * physical load address — the bootloader maps VBASE -> 0x400000 (4 KiB pages,
 * a 2 MiB PT_HI span) and jumps high. A low identity map is kept so the
 * PMM/heap can still dereference physical frames directly.
 *
 * __bss_start/__bss_end bound the .bss the entry stub zeroes (it is NOLOAD, so
 * not present in the flat image).
 */

ENTRY(kernel_entry)

KERNEL_VBASE = 0xFFFFFFFF80000000;
KERNEL_LMA   = 0x400000;

SECTIONS
{
    . = KERNEL_VBASE;

    /* DDR-757 kernel-self W^X: section boundaries are page-aligned so each 4 KiB
     * PT_HI page carries exactly one policy (text RX / rodata R+NX / data RW+NX);
     * __text_end and __rodata_end are the policy boundaries vmm_protect_kernel
     * walks. */
    .text ALIGN(16) : AT(ADDR(.text) - KERNEL_VBASE + KERNEL_LMA) {
        *(.text.boot)              /* kernel_entry first */
        *(.text .text.*)
        . = ALIGN(4096);
        __text_end = .;
    }
    .rodata : AT(ADDR(.rodata) - KERNEL_VBASE + KERNEL_LMA) {
        *(.rodata .rodata.*)
        . = ALIGN(4096);
        __rodata_end = .;
    }
    .data : AT(ADDR(.data) - KERNEL_VBASE + KERNEL_LMA) {
        *(.data .data.*)
    }
    .bss ALIGN(64) (NOLOAD) : {        /* 64 = cache-line alignment for IPC rings */
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(64);
        __bss_end = .;
    }

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) *(.eh_frame_hdr) }
}
