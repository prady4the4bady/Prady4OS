/* user/libtrel.c — DDR-1189 Amendment 5 (C1.6 P3b), arm G.
 *
 * Installed as /LIB/LIBTREL.SO and linked -z notext: an aligned 8-byte word in
 * .text holds trel_get's address, so the library carries a relocation into its
 * read-execute segment and DT_TEXTREL. musl's ld.so then asks for
 * mprotect(PROT_READ|PROT_WRITE|PROT_EXEC) over the mapping and fails the load
 * on any error but ENOSYS (ldso/dynlink.c). The kernel answers -EACCES (W^X,
 * DDR-1189 L1), so the load fails and /DYNTREL.ELF exits 127 (mutants D8, D8b).
 * QEMU only, not verified on real hardware. */
int trel_get(void) {
    return 5;
}

__asm__(
    "    .text\n"
    "    .balign 8\n"
    "trel_word:\n"
    "    .quad trel_get\n");
