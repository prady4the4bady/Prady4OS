/* tests/toolchain/hello.c
 * Freestanding C unit — proves clang can compile bare-metal x86_64 objects. */

int tc_add(int a, int b)
{
    return a + b;
}
