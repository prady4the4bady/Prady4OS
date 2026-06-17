/* tests/toolchain/tc_main.c
 * Pulls together the C, NASM, and Rust units so ld.lld must resolve symbols
 * across all three toolchains. Never executed — linking is the test. */

int           tc_add(int a, int b);     /* from hello.c   */
int           tc_rust_value(void);      /* from libhello_rs.a (Rust no_std) */
unsigned long tc_asm_value(void);       /* from hello.asm */

void _start(void)
{
    volatile int sum = tc_add(1, 2) + tc_rust_value() + (int)tc_asm_value();
    (void)sum;
    for (;;) { }
}
