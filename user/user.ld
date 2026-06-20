/* user/user.ld — link script for static ring-3 binaries (Phase 5a, ADR-021).
 *
 * One read+execute PT_LOAD segment based at 0x8000000000 (PML4 slot 1, the
 * per-process user range). Code and read-only data share the segment; there is
 * no writable segment, so the image is W^X by construction (no RWX page). */
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);          /* PF_R | PF_X */
}

SECTIONS {
    . = 0x8000000000;
    .text : { *(.text*) *(.rodata*) } :text
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.eh_frame_hdr) }
}
