/* user/domcaptest.c — DDR-1149: CAP_OCR / CAP_SCENE / CAP_NET_BROWSE at submission.
 *
 * Spawned FIVE times by the kernel under the `domcap` probe key, each with
 * argv[1] naming its role, and each is_agent (authority to submit at all):
 *
 *   GRANT   all three doors, all three capabilities   -> ok ok ok ok
 *   NODOOR  capabilities only (flags cleared)         -> -1 -1 -1 -1
 *   NOCAP   doors only (handles left CAP_NULL)        -> -1 -1 -1 -1
 *   DOORX   OCR door only, ALL three capabilities     -> ok -1 -1 ok
 *   CAPX    ALL three doors, OCR capability only      -> ok -1 -1 ok
 *
 * Fields: ocr/scene/browse submit PARSE_DOCUMENT / QUERY_SCENE / BROWSE_WEB via
 * NSI 31; child submits PARSE_DOCUMENT via NSI 92 (parent 0) so the second
 * submit path is covered. NODOOR and NOCAP each lack exactly ONE layer, so
 * neither check can be deleted with the gate green (DDR-1033's measured lesson);
 * DOORX and CAPX each narrow ONE layer per type: a single "OCR only" role
 * narrowing both was measured to let a wrong flag mapping pass (DDR-1149
 * sec.4.1), because the capability refusal masked it.
 *
 * Accepted actions are polled once so their queue slots are released. The
 * probe REPORTS, the gate JUDGES (DDR-1020): one line, printed unconditionally.
 *
 * argv arrives ON THE STACK at entry, so the entry point is a file-scope asm
 * trampoline (DDR-1032's reason for an assembly receiver): it passes the entry
 * RSP in RDI and realigns before calling C. Freestanding, no writable globals.
 */
#include "uline.h"

#define SYS_EXIT                 4
#define SYS_WRITE                6
#define SYS_SUBMIT_ACTION       31
#define SYS_POLL_RESULT         32
#define SYS_SUBMIT_CHILD_ACTION 92

/* Pinned by _Static_assert in kernel/aether/aether.h. */
#define ACTION_PARSE_DOCUMENT   16
#define ACTION_QUERY_SCENE      17
#define ACTION_BROWSE_WEB       18

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}
static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }

/* Accepted -> "ok" (and the entry is polled so its slot is freed); refused ->
 * the exact return value. */
static void field(uline *u, const char *name, long r) {
    ul_s(u, name);
    if (r >= 0) {
        ul_s(u, "ok");
        (void)nsi(SYS_POLL_RESULT, r, 0, 0);
    } else {
        ul_d(u, r);
    }
}

__asm__(".globl _start\n"
        "_start:\n"
        "  mov %rsp, %rdi\n"
        "  and $-16, %rsp\n"
        "  call domcap_main\n"
        "  ud2\n");

__attribute__((noreturn, force_align_arg_pointer, used))
void domcap_main(long *sp) {
    long argc = sp[0];
    const char *role = (argc >= 2) ? (const char *)sp[2] : "NOARGS";
    static const char pl[] = "/DOC.TXT";

    long o = nsi(SYS_SUBMIT_ACTION, ACTION_PARSE_DOCUMENT, (long)pl, slen(pl));
    long s = nsi(SYS_SUBMIT_ACTION, ACTION_QUERY_SCENE,    (long)pl, slen(pl));
    long b = nsi(SYS_SUBMIT_ACTION, ACTION_BROWSE_WEB,     (long)pl, slen(pl));
    long c = nsi4(SYS_SUBMIT_CHILD_ACTION, ACTION_PARSE_DOCUMENT,
                  (long)pl, slen(pl), 0);

    uline u; ul_init(&u);
    ul_s(&u, "PRADYOS_DOMCAP role="); ul_s(&u, role);
    field(&u, " ocr=", o);
    field(&u, " scene=", s);
    field(&u, " browse=", b);
    field(&u, " child=", c);
    ul_s(&u, "\n");
    const char *line = ul_end(&u);
    nsi(SYS_WRITE, 1, (long)line, slen(line));
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
