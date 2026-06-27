/* user/cmusl.c — PROC-D step 3: the first ring-3 C program on PRADYOS.
 *
 * Linked statically against the minimal musl subset (build/musl/lib/libc.a +
 * crt1.o) with user/user_c.ld. crt1.o -> __libc_start_main sets up the thread
 * pointer (via the overlay __set_thread_area -> SYS_SET_TLS) and stdio; printf
 * formats into the stdout buffer and flushes it to fd 1 with SYS_WRITEV, which
 * the kernel writes to the serial console. smoke-user greps PRADYOS_MUSL_OK. */
#include <stdio.h>

int main(void)
{
    printf("PRADYOS_MUSL_OK v1.2.5 2026\n");
    return 0;
}
