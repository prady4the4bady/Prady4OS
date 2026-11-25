/* user/dyntrel.c — DDR-1189 Amendment 5 (C1.6 P3b), arm G.
 *
 * Needs libtrel.so, which carries DT_TEXTREL. ld.so must refuse to load it, so
 * this main never runs: if it does, it prints the forbidden sentinel
 * PRADYOS_DYN_X and exits 0 (dyntest wants 127). QEMU only, not verified on
 * real hardware. */
#include <stdio.h>

extern int trel_get(void);

int main(void) {
    printf("PRADYOS_DYN_X textrel library loaded trel=%d\n", trel_get());
    return 0;
}
