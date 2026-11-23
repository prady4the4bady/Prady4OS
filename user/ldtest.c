/* user/ldtest.c — DDR-1189 Amendment 3 (C1.6 P3a-3): a freestanding program
 * interpreter for smoke-dynlink arm D0.
 *
 * Built -fPIE and linked `ld -pie --no-dynamic-linker -z text`: an ET_DYN with
 * no PT_INTERP and NO dynamic relocations (every address it uses is computed
 * PC-relative), because the kernel relocates nothing when an interpreter is
 * present. Installed as /LIB/LDTEST.SO. A program linked with
 * --dynamic-linker=/LIB/LDTEST.SO (dynpie.c built -DDYN_D0, /DYND0.ELF) is
 * started here, not at its own entry. This probe:
 *
 *   1. reads the auxv the kernel built and checks AT_BASE is its own base
 *      (the run-time address of its ELF header, __ehdr_start);
 *   2. finds the program's bias from AT_PHDR and its PT_PHDR, walks the
 *      program's PT_DYNAMIC, and applies its R_X86_64_RELATIVE relocations
 *      (any other type stops it);
 *   3. prints   PRADYOS_DYN_D0 base_ok=B nrel=N
 *      and, when B is 1 and N > 0, jumps to AT_ENTRY with the entry stack and
 *      %rdi exactly as the kernel left them; otherwise it exits 3.
 *
 * It tests the kernel's half of PT_INTERP without musl. Raw NSI syscalls, no
 * libc. QEMU only, not verified on real hardware.
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
#define PT_PHDR    6

#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9

#define R_X86_64_RELATIVE 8

struct ld_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

struct ld_rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
};

extern const char __ehdr_start[] __attribute__((visibility("hidden")));

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

/* Signed decimal into out (at least 24 bytes), NUL-terminated. */
static void dec(char *out, long v) {
    char t[24];
    int n = 0, i = 0;
    unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    if (v < 0)
        out[i++] = '-';
    while (n)
        out[i++] = t[--n];
    out[i] = 0;
}

/* Apply the program's RELATIVE relocations. The count applied, or -1 for a
 * table this probe cannot read (no PT_PHDR, no PT_DYNAMIC, another type). */
static long relocate(uint64_t a_phdr, uint64_t a_phent, uint64_t a_phnum) {
    uint64_t bias = 0, dyn = 0;
    int have_bias = 0;
    const uint8_t *ph = (const uint8_t *)(uintptr_t)a_phdr;
    for (uint64_t i = 0; i < a_phnum; i++, ph += a_phent) {
        const struct ld_phdr *q = (const struct ld_phdr *)ph;
        if (q->p_type == PT_PHDR) {
            bias = a_phdr - q->p_vaddr;
            have_bias = 1;
        }
    }
    ph = (const uint8_t *)(uintptr_t)a_phdr;
    for (uint64_t i = 0; i < a_phnum; i++, ph += a_phent) {
        const struct ld_phdr *q = (const struct ld_phdr *)ph;
        if (q->p_type == PT_DYNAMIC)
            dyn = q->p_vaddr;
    }
    if (!have_bias || !dyn)
        return -1;
    uint64_t rela = 0, relasz = 0, relaent = sizeof(struct ld_rela);
    for (const uint64_t *d = (const uint64_t *)(uintptr_t)(bias + dyn); d[0] != DT_NULL; d += 2) {
        if (d[0] == DT_RELA)    rela = d[1];
        if (d[0] == DT_RELASZ)  relasz = d[1];
        if (d[0] == DT_RELAENT) relaent = d[1];
    }
    if (relaent != sizeof(struct ld_rela) || !rela)
        return -1;
    long n = 0;
    for (uint64_t o = 0; o + sizeof(struct ld_rela) <= relasz; o += sizeof(struct ld_rela)) {
        const struct ld_rela *r = (const struct ld_rela *)(uintptr_t)(bias + rela + o);
        if ((uint32_t)r->r_info != R_X86_64_RELATIVE)
            return -1;
        *(uint64_t *)(uintptr_t)(bias + r->r_offset) = bias + (uint64_t)r->r_addend;
        n++;
    }
    return n;
}

/* Returns the program's entry; the stub below jumps there. */
__attribute__((used)) uint64_t ld_main(const uint64_t *sp) {
    /* argc, argv[], NULL, envp[], NULL, then the auxv pairs */
    const uint64_t *p = sp + 1 + sp[0] + 1;
    while (*p)
        p++;
    p++;
    uint64_t a_phdr = 0, a_phent = 0, a_phnum = 0, a_base = 0, a_entry = 0;
    for (; p[0] != AT_NULL; p += 2) {
        if (p[0] == AT_PHDR)  a_phdr = p[1];
        if (p[0] == AT_PHENT) a_phent = p[1];
        if (p[0] == AT_PHNUM) a_phnum = p[1];
        if (p[0] == AT_BASE)  a_base = p[1];
        if (p[0] == AT_ENTRY) a_entry = p[1];
    }
    int base_ok = a_base != 0 && a_base == (uint64_t)(uintptr_t)__ehdr_start;
    long nrel = (a_phdr && a_phent && a_phnum) ? relocate(a_phdr, a_phent, a_phnum) : -1;
    char d[24];
    wr("PRADYOS_DYN_D0 base_ok=");
    wr(base_ok ? "1" : "0");
    wr(" nrel=");
    dec(d, nrel);
    wr(d);
    wr("\n");
    if (!base_ok || nrel <= 0 || !a_entry)
        for (;;)
            nsi(SYS_EXIT, 3, 0, 0);
    return a_entry;
}

/* The entry is an assembly stub (DDR-823; ci-start-align-check). It keeps the
 * entry %rsp (the program's argc/argv/envp/auxv) and %rdi (the capability the
 * kernel passes) in callee-saved registers across ld_main, then hands both
 * back unchanged and jumps to the program's entry, %rdx = 0 (no fini, SysV). */
__asm__(".text\n"
        ".globl _start\n"
        ".hidden _start\n"
        ".type _start, @function\n"
        "_start:\n"
        "    mov %rsp, %r12\n"
        "    mov %rdi, %r13\n"
        "    mov %rsp, %rdi\n"
        "    and $-16, %rsp\n"
        "    call ld_main\n"
        "    mov %r12, %rsp\n"
        "    mov %r13, %rdi\n"
        "    xor %edx, %edx\n"
        "    jmp *%rax\n");
