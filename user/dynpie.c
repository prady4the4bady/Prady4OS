/* user/dynpie.c — DDR-1189 Amendment 2 (C1.6 P2): static-PIE probe for
 * smoke-dynlink arm C (C1, C2, C3).
 *
 * Built -fPIE and linked `ld -pie --no-dynamic-linker -z text`: an ET_DYN with
 * no PT_INTERP, whose only dynamic relocations are R_X86_64_RELATIVE. The
 * loader picks its base and applies them. dyntest's boot instance execve()s it
 * four times; each run prints one line and exits 0:
 *
 *   PRADYOS_DYN_P base=0x... rel=R aux=X
 *
 *   base  the run-time address of its own ELF header (__ehdr_start; the first
 *         PT_LOAD maps file offset 0 at link address 0, so this is the base).
 *   rel   1 when a data pointer and a function pointer, each needing a
 *         RELATIVE relocation, equal the addresses the code computes
 *         PC-relative (C1; mutant D3 leaves them unrelocated).
 *   aux   1 when AT_ENTRY is _start, AT_PHNUM and AT_PHENT match the ELF
 *         header, AT_PHDR is base + e_phoff, and musl's rule (the address of
 *         _DYNAMIC minus PT_DYNAMIC's p_vaddr, read through AT_PHDR) gives the
 *         same base (C3; mutant D3c leaves AT_PHDR and AT_ENTRY unbiased).
 *         AT_PHDR is dereferenced only after it has matched.
 *
 * DDR-1189 A3 P3a-3: built -DDYN_D0 and linked with
 * --dynamic-linker=/LIB/LDTEST.SO, it is arm D0's main program (/DYND0.ELF).
 * The kernel then relocates nothing, so rel=1 means the interpreter did; the
 * line is PRADYOS_DYN_D0M and adds ibase, the AT_BASE it was given.
 *
 * Raw NSI syscalls, no libc. No arrays are zero-filled, so no memset is
 * emitted. QEMU only, not verified on real hardware.
 */
#include <stdint.h>

#define SYS_EXIT   4
#define SYS_WRITE  6

#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_BASE    7
#define AT_ENTRY   9
#define PT_DYNAMIC 2

struct pie_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct pie_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

extern const struct pie_ehdr __ehdr_start __attribute__((visibility("hidden")));
extern char _DYNAMIC[] __attribute__((visibility("hidden")));
extern char _start[] __attribute__((visibility("hidden")));

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

/* "0x" and 16 hex digits into out[0..17], NUL at out[18]. */
static void hex64(char *out, uint64_t v) {
    static const char dig[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 16; i++)
        out[2 + i] = dig[(v >> (60 - 4 * i)) & 0xfu];
    out[18] = 0;
}

/* The two relocated pointers (C1). volatile: each comparison loads the word
 * the loader wrote, so the compiler cannot fold it. */
static int pie_target = 42;
static void pie_marker(void) {}
static int *volatile rel_ptr = &pie_target;
static void (*volatile rel_fn)(void) = &pie_marker;

__attribute__((noreturn, used)) void pie_main(const uint64_t *sp) {
    uint64_t base = (uint64_t)(uintptr_t)&__ehdr_start;
    int rel = (rel_ptr == &pie_target) && (rel_fn == &pie_marker);

    /* argc, argv[], NULL, envp[], NULL, then the auxv pairs */
    const uint64_t *p = sp + 1 + sp[0] + 1;
    while (*p)
        p++;
    p++;
    uint64_t a_phdr = 0, a_phent = 0, a_phnum = 0, a_entry = 0, a_base = 0;
    for (; p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_BASE)  a_base = p[1];
        if (p[0] == AT_PHDR)  a_phdr = p[1];
        if (p[0] == AT_PHENT) a_phent = p[1];
        if (p[0] == AT_PHNUM) a_phnum = p[1];
        if (p[0] == AT_ENTRY) a_entry = p[1];
    }
    const struct pie_ehdr *eh = &__ehdr_start;
    int aux = a_entry == (uint64_t)(uintptr_t)_start &&
              a_phnum == eh->e_phnum && a_phent == eh->e_phentsize &&
              a_phdr == base + eh->e_phoff;
    if (aux) {                         /* musl's rule (ldso/dlstart.c) */
        uint64_t mb = 0;
        const uint8_t *ph = (const uint8_t *)(uintptr_t)a_phdr;
        for (uint64_t i = 0; i < a_phnum; i++, ph += a_phent) {
            const struct pie_phdr *q = (const struct pie_phdr *)ph;
            if (q->p_type == PT_DYNAMIC) {
                mb = (uint64_t)(uintptr_t)_DYNAMIC - q->p_vaddr;
                break;
            }
        }
        aux = (mb == base);
    }

    char hb[19];
    hex64(hb, base);
#ifdef DYN_D0
    wr("PRADYOS_DYN_D0M base=");
#else
    (void)a_base;
    wr("PRADYOS_DYN_P base=");
#endif
    wr(hb);
    wr(rel ? " rel=1" : " rel=0");
#ifdef DYN_D0
    wr(aux ? " aux=1" : " aux=0");
    hex64(hb, a_base);
    wr(" ibase=");
    wr(hb);
    wr("\n");
#else
    wr(aux ? " aux=1\n" : " aux=0\n");
#endif
    for (;;)
        nsi(SYS_EXIT, 0, 0, 0);
}

/* The entry is an assembly stub (DDR-823; ci-start-align-check), as in
 * dyntest.c: it hands the entry %rsp to pie_main and aligns the stack. */
__asm__(".text\n"
        ".globl _start\n"
        ".hidden _start\n"
        ".type _start, @function\n"
        "_start:\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call pie_main\n"
        "    ud2\n");
