/* user/libtest.c — DDR-1189 Amendment 5 (C1.6 P3b), arms D and E.
 *
 * Installed as /LIB/LIBTEST.SO (soname libtest.so). musl's ld.so finds it on its
 * default path (/lib) and binds the main probe /DYNMAIN.ELF to it:
 *   lib_answer  a function, called through the main program's PLT (arm D);
 *   lib_var     a global, read through the main program's GOT, which needs an
 *               R_X86_64_GLOB_DAT (arm E; mutant D5 skips that type);
 *   lib_fp      a pointer in this library's data to a preemptible function,
 *               which needs a symbolic R_X86_64_64 (arm E; mutant D5b).
 * QEMU only, not verified on real hardware. */
int lib_var = 7;

int lib_answer(void) {
    return 42;
}

int (*lib_fp)(void) = lib_answer;
