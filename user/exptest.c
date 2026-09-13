/* user/exptest.c — the bounded experiment executor, both sides (DDR-1034).
 *
 * THE KERNEL SPAWNS THIS TWICE, once with the door granted and once without.
 * That is not redundancy: `is_exec` is a PER-PROCESS flag, so one process
 * cannot exercise both the allowed and the refused path. The DENY process is
 * spawned WITH the CAP_EXEC capability and then has is_exec cleared -- it holds
 * the capability and lacks only the door -- so a mutant that deletes the
 * is_exec check cannot be saved by cap_authorize. DDR-1033's arm B was passing
 * for exactly that wrong reason and only a mutant found it.
 *
 * The probe REPORTS and the gate JUDGES. Every arm prints its value before any
 * exit, because a fail() before a print silently removes an arm -- the dead-arm
 * class, seven instances deep in this project.
 */

#include "uline.h"          /* DDR-1056: one write per measured line */

#define SYS_EXIT            4
#define SYS_WRITE           6
#define SYS_RUN_EXPERIMENT 100
#define SYS_EXP_RESULT     101

/* Opcodes, hand-copied from kernel/aether/experiment.h. */
#define OP_HALT  0
#define OP_PUSH  1
#define OP_ADD   2
#define OP_SUB   3
#define OP_MUL   4
#define OP_DUP   5
#define OP_DROP  6
#define OP_SWAP  7
#define OP_JNZ   8

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

static void wrdec(long v) {
    char b[24];
    int i = 0, neg = 0;
    unsigned long u;
    if (v < 0) { neg = 1; u = (unsigned long)(-v); } else { u = (unsigned long)v; }
    if (!u) b[i++] = '0';
    while (u) { b[i++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg) b[i++] = '-';
    while (i) { char c = b[--i]; nsi(SYS_WRITE, 1, (long)&c, 1); }
}

/* Emit `PUSH imm` at p; returns the new offset. The immediate is 8 raw
 * little-endian bytes, so this is written out rather than memcpy'd -- freestanding
 * probes have no libc. */
static unsigned emit_push(unsigned char *c, unsigned p, long v) {
    unsigned long u = (unsigned long)v;
    c[p++] = OP_PUSH;
    for (int i = 0; i < 8; i++) { c[p++] = (unsigned char)(u & 0xFF); u >>= 8; }
    return p;
}

/* The result record, hand-copied from kernel/aether/experiment.h. Packed and
 * field-for-field, because the kernel copies it out verbatim. */
struct exp_result {
    unsigned long long seq;
    unsigned int  pid;
    int           status;
    long long     value;
    unsigned int  steps;
    unsigned int  code_len;
} __attribute__((packed));

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    /* Built on the STACK, never as a static: user.ld gives these probes a
     * single R+X segment, and ci-probe-rodata-check rejects any writable
     * allocated section (the defect DDR-1032 hit with a static pointer array). */
    unsigned char code[64];
    long long v = 0;
    unsigned p;

    /* ---- ARM A / ARM B: 6 * 7, reported by value ------------------------
     * rc=0 v=42 in the granted process; rc=-EPERM in the un-granted one, which
     * holds CAP_EXEC and lacks only is_exec. */
    p = 0;
    p = emit_push(code, p, 6);
    p = emit_push(code, p, 7);
    code[p++] = OP_MUL;
    code[p++] = OP_HALT;

    long rc = nsi(SYS_RUN_EXPERIMENT, (long)code, (long)p, (long)&v);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_EXP_CALC rc=");    /* DDR-1056 */
      ul_d(&u, rc); ul_s(&u, " v="); ul_d(&u, (long)v);
      ul_s(&u, "\n"); wr(ul_end(&u)); }
    if (rc != 0) {
        /* The deny process. Print the gate line and stop: every arm below needs
         * the door, so continuing would report refusals as if they were bounds
         * checks. */
        { uline u; ul_init(&u); ul_s(&u, "PRADYOS_EXP_GATE rc="); /* DDR-1056 */
          ul_d(&u, rc); ul_s(&u, "\n"); wr(ul_end(&u)); }
        nsi(SYS_EXIT, 0, 0, 0);
        for (;;) { }
    }
    wr("PRADYOS_EXP_GATE rc=0\n");

    /* ---- ARM C: the step cap is REACHABLE and enforced -------------------
     * A JNZ that always branches back to its own condition. Without EXP_JNZ in
     * the opcode set this arm could not exist and EXP_MAX_STEPS would be a
     * bound whose only reachable value is the passing one. Expect -ELOOP (40). */
    p = 0;
    p = emit_push(code, p, 1);       /* offset 0: push a non-zero condition   */
    code[p++] = OP_JNZ;              /* offset 9                              */
    code[p++] = (unsigned char)(-11);  /* -> pc 11 + (-11) = 0          */
    code[p++] = OP_HALT;
    rc = nsi(SYS_RUN_EXPERIMENT, (long)code, (long)p, (long)&v);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_EXP_LOOP rc=");    /* DDR-1056 */
      ul_d(&u, rc); ul_s(&u, "\n"); wr(ul_end(&u)); }

    /* ---- ARM D: operand-stack overflow is refused ------------------------
     * One PUSH then DUP past EXP_STACK_N (32). Expect -EOVERFLOW (75). */
    p = 0;
    p = emit_push(code, p, 5);
    for (int i = 0; i < 40; i++)
        code[p++] = OP_DUP;
    code[p++] = OP_HALT;
    rc = nsi(SYS_RUN_EXPERIMENT, (long)code, (long)p, (long)&v);
    { uline u; ul_init(&u); ul_s(&u, "PRADYOS_EXP_OVF rc=");     /* DDR-1056 */
      ul_d(&u, rc); ul_s(&u, "\n"); wr(ul_end(&u)); }

    /* ---- ARM E: the store recorded what the KERNEL computed ---------------
     * Slot 0 is arm A's run: status 0, value 42, and 4 retired instructions
     * (PUSH, PUSH, MUL, HALT). The agent never wrote any of those numbers. */
    struct exp_result r;
    r.seq = 0; r.pid = 0; r.status = 0; r.value = 0; r.steps = 0; r.code_len = 0;
    rc = nsi(SYS_EXP_RESULT, 0, (long)&r, 0);
    wr("PRADYOS_EXP_REC rc="); wrdec(rc);
    wr(" st="); wrdec((long)r.status);
    wr(" v=");  wrdec((long)r.value);
    wr(" steps="); wrdec((long)r.steps);
    wr("\n");

    wr("PRADYOS_EXP_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
