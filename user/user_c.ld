/* user/user_c.ld — link script for static ring-3 *C* binaries (PROC-D, ADR-023 §D7).
 *
 * Unlike user/user.ld (one R+X segment, used by the hand-written asm programs
 * that have no mutable globals), a C program linked against musl has writable
 * global state — the stdout FILE, the libc struct, malloc state, the static
 * builtin_tls block. So this script emits THREE W^X-clean PT_LOAD segments,
 * each page-aligned so the ELF loader maps every page exactly once with the
 * permissions it derives from p_flags (kernel/exec/elf.c):
 *
 *   text   PF_R|PF_X  -> RX     (.text)            — execute, read, no write, no NX
 *   rodata PF_R       -> R + NX (.rodata, relro)   — read-only, never executable
 *   data   PF_R|PF_W  -> RW + NX (.data, .bss)     — read-write, never executable
 *
 * No segment is ever both W and X, so ADR-021 W^X holds by construction. Base
 * 0x8000000000 (PML4 slot 1, the per-process user range), matching user.ld.
 * The image is ET_EXEC (link with -static / -no-pie) so the loader accepts it. */
ENTRY(_start)

PHDRS {
    text   PT_LOAD FLAGS(5);          /* PF_R | PF_X */
    rodata PT_LOAD FLAGS(4);          /* PF_R        */
    data   PT_LOAD FLAGS(6);          /* PF_R | PF_W */
}

SECTIONS {
    . = 0x8000000000;

    /* -mcmodel=large (required: the user base 0x8000000000 exceeds the 32-bit
     * relocation range, so C code uses 64-bit absolute refs) emits the large
     * section variants .ltext/.lrodata/.ldata/.lbss — match both spellings. */
    .text : { *(.text*) *(.ltext*) } :text

    . = ALIGN(0x1000);                /* own page(s): R+NX, never shares with RX/RW */
    .rodata : {
        *(.rodata*) *(.lrodata*)
        *(.data.rel.ro*)             /* constant-after-link in a static ET_EXEC */
    } :rodata

    . = ALIGN(0x1000);                /* own page(s): RW+NX */
    .data : {
        *(.data*) *(.ldata*)
        *(.got) *(.got.plt)
    } :data
    .bss : {
        *(.bss*) *(.lbss*)
        *(COMMON)
    } :data

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.eh_frame_hdr) }
}
