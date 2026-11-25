/* user/dynmain.c — DDR-1189 Amendment 5 (C1.6 P3b), arms D and E.
 *
 * A dynamically linked musl program: PT_INTERP /lib/ld-musl-x86_64.so.1 (the
 * kernel maps it, P3a), DT_NEEDED libtest.so and libc.so. Every symbol below
 * is bound by musl's ld.so, and printf runs from libc.so. Compiled -fPIC so
 * lib_var is read through the GOT, not a copy relocation. dyntest (the
 * driver) checks that it exits 0; the gate greps the two lines.
 * QEMU only, not verified on real hardware. */
#include <stdio.h>

extern int lib_var;
extern int lib_answer(void);
extern int (*lib_fp)(void);

int main(void) {
    printf("PRADYOS_DYN_D answer=%d libc=1\n", lib_answer());
    printf("PRADYOS_DYN_E var=%d fp=%d\n", lib_var, lib_fp());
    return 0;
}
