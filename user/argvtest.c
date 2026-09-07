/* user/argvtest.c — SYS_EXECVE argv/envp marshalling, the launching half (DDR-1032).
 *
 * Before this, sys_execve read `uargv`/`uenvp` and threw them away with a
 * (void) cast, and elf_build_image hardcoded argc=1 / argv[0]=path. So
 * execve(path, {"prog","--flag"}, ...) SUCCEEDED and the program saw no
 * arguments -- the same silent-discard shape DDR-877 called "worse than
 * incomplete" when it found mmap dropping fd and offset.
 *
 * This half only launches. The receiving half is user/argtest.asm, which is in
 * assembly because argc/argv/envp arrive ON THE STACK AT ENTRY and reading them
 * from C means trusting a prologue that force_align_arg_pointer has already
 * moved (DDR-823).
 *
 * The vectors below are chosen so ORDER and COUNT are both visible: three argv
 * entries with distinct values and one envp entry. A marshaller that delivered
 * the right strings in the wrong order, or dropped the last one, prints
 * differently from one that works -- "some strings arrived" is not the assertion.
 */

#define SYS_EXIT    4
#define SYS_WRITE   6
#define SYS_EXECVE 14

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    static const char a0[] = "argtest";
    static const char a1[] = "alpha";
    static const char a2[] = "beta";
    static const char e0[] = "PRADYOSVAR=set";
    /* The vectors are built ON THE STACK, not as statics. A `static const char
     * *argv[]` holds ADDRESSES, so the compiler puts it in writable data -- and
     * these probes link as a single R+X segment with no writable globals, which
     * ci-probe-rodata-check enforces (§NON-NEGOTIABLE 15). It caught exactly
     * that here. The strings themselves are const char[] and stay in .rodata. */
    const char *argv[4];
    const char *envp[2];
    argv[0] = a0; argv[1] = a1; argv[2] = a2; argv[3] = 0;
    envp[0] = e0; envp[1] = 0;

    long rc = nsi(SYS_EXECVE, (long)"/ARGTEST.ELF", (long)argv, (long)envp);

    /* execve does not return on success, so reaching here is the failure. */
    wr("ARGVTEST FAIL: execve returned\n");
    (void)rc;
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}
