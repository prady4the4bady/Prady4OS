/* user/dynpbad.c — DDR-1189 Amendment 2 (C1.6 P2): two images smoke-dynlink
 * arm C must see refused. One source, built twice:
 *
 *   /DYNPINT.ELF  linked with a PT_INTERP (--dynamic-linker): P2 refuses an
 *                 ET_DYN with an interpreter (C4; mutant D3d skips that).
 *   /DYNPTXT.ELF  built with -DDYN_TEXTREL and linked -z notext: an aligned
 *                 8-byte word in .text holds this image's own address, so its
 *                 R_X86_64_RELATIVE lands in the read-execute segment. P2
 *                 refuses any relocation outside a writable PT_LOAD (C5;
 *                 mutant D3e skips that check).
 *
 * If either one is loaded and runs, it prints `PRADYOS_DYN_X <what> image ran`
 * (a forbidden sentinel) and exits 0. Raw NSI syscalls, no libc.
 * QEMU only, not verified on real hardware.
 */
#define SYS_EXIT   4
#define SYS_WRITE  6

#ifdef DYN_TEXTREL
#define DYN_WHAT "textrel"
#else
#define DYN_WHAT "interp"
#endif

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

__attribute__((noreturn, used)) void bad_main(void) {
    static const char msg[] = "PRADYOS_DYN_X " DYN_WHAT " image ran\n";
    nsi(SYS_WRITE, 1, (long)msg, sizeof msg - 1);
    for (;;)
        nsi(SYS_EXIT, 0, 0, 0);
}

/* Entry stub (DDR-823; ci-start-align-check). */
__asm__(".text\n"
        ".globl _start\n"
        ".hidden _start\n"
        ".type _start, @function\n"
        "_start:\n"
        "    and $-16, %rsp\n"
        "    call bad_main\n"
        "    ud2\n"
#ifdef DYN_TEXTREL
        /* The text relocation: never executed, 8-aligned so that only the
         * writable-segment check (not alignment) refuses it. */
        "    .balign 8\n"
        "dyn_textrel_word:\n"
        "    .quad bad_main\n"
#endif
        );
