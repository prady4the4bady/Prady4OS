/* user/actionipctest.c — Section 3C ACTION_SEND_IPC, end to end (DDR-1084).
 *
 * DDR-1017 assessed this type as "not buildable at any ring": ipc_send/ipc_recv
 * were kernel-internal, there was no SYS_IPC_*, so an approved SEND_IPC had no
 * executor. DDR-1033 built the door -- and its own NOT CLAIMED said "the AETHER
 * action path does not yet CALL this", which is where the row then sat for ~51
 * DDRs. That is DDR-1084 sec.1's structural point, and it is the SECOND
 * consecutive instance (DDR-1034 -> RUN_EXPERIMENT, closed one commit ago by
 * DDR-1083): the DDR that retires a blocker is working on the subsystem, not on
 * the row, so nothing revisits the row.
 *
 * This file also makes aether.h's SEND_IPC pin OWED, correctly. That header
 * says 7 is deliberately unpinned because "nothing hand-copies 7, and a pin
 * whose probe does not exist would read as a claim that one does" -- true until
 * this commit (measured: the only two matches outside the header were COMMENTS).
 * DDR-1083 sec.2 found the RUN_EXPERIMENT pin beside it carrying a justification
 * that was FALSE; this is the same rule satisfied the right way round.
 *
 * WHAT THIS PROVES, AND WHAT IT DOES NOT. sys_ipc_send checks is_ipc and
 * ipc_send's cap_authorize and DOES NOT consult the action queue -- an agent
 * holding the door can call NSI 98 without submitting anything, before and
 * after. NO NEW ENFORCEMENT. What is added is propose -> arbitrate -> OBEY and
 * its audit record, the design DDR-1013 sec.2 states for every action type.
 *
 * THE OBVIOUS ARM IS VACUOUS AND THAT WAS MEASURED FIRST: smoke-sendipc already
 * requires ipctest.c's send to return 0, so "submit, then assert rc=0" passes on
 * a build with no submit at all. The message CONTENT does not rescue it either
 * -- this probe holds the words it sent, so printing them back proves nothing an
 * agent could not compute without sending (DDR-1066's M2 lesson).
 *
 * ARM B IS STRONGER THAN DDR-1083's AND THAT IS THE DESIGN. There, the decline
 * printed ran=0 -- a flag the probe reports ABOUT ITSELF, which that DDR's own
 * sec.9 recorded as the weaker half. Here the decline is CONFIRMED BY THE
 * KERNEL: having declined, the probe receives on the same slot and the kernel
 * returns -ETIMEDOUT because the endpoint is genuinely empty. -110 is a value
 * the probe cannot manufacture without the kernel having searched an empty
 * endpoint; a probe that wrongly sent leaves e->full=1 and the same receive
 * returns 0.
 *
 * COST, STATED NOT HIDDEN: ipc_recv blocks for DDR-961's 500-tick bound before
 * reporting -ETIMEDOUT. It spends no syscall budget (the process blocks in the
 * kernel and AETHER_RATE_MAX counts syscalls, not time), which matters because
 * this process holds is_agent.
 *
 * SLOTS 4 AND 5, MEASURED DISJOINT: ipctest.c:23 uses SLOT 2 (and 99 as its
 * out-of-range arm) and AGENT_ROSTER_N is 8. The two probes co-boot under one
 * key and must not share an endpoint, or arm B's emptiness claim would be about
 * ipctest's traffic rather than its own.
 *
 * Freestanding (no libc): raw syscalls, no writable globals (user.ld gives a
 * single R+X PT_LOAD, and ci-probe-rodata-check rejects any writable allocated
 * section -- the defect DDR-1032 hit with a static pointer array).
 */

#include "uline.h"          /* DDR-1056: one write(2) per measured line */

#define SYS_EXIT                 4
#define SYS_WRITE                6
#define SYS_SUBMIT_ACTION       31
#define SYS_POLL_RESULT         32
#define SYS_SUBMIT_CHILD_ACTION 92
#define SYS_IPC_SEND            98
#define SYS_IPC_RECV            99

/* Action wire format, pinned by _Static_assert in kernel/aether/aether.h. This
 * file is what makes the SEND_IPC pin owed (DDR-1084 sec.2). */
#define ACTION_DELETE_FILE   6
#define ACTION_SEND_IPC      7
#define AE_PENDING           1
#define AE_APPROVED          2

#define ETIMEDOUT          110      /* kernel/include/errno.h:33 */

#define SLOT_A               4      /* ipctest.c uses SLOT 2 -- disjoint */
#define SLOT_B               5
#define IPC_MSG_WORDS        4

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* Four arguments: SYS_SUBMIT_CHILD_ACTION takes the parent id in a4, which is
 * r10 in the SysV syscall convention (the form actiondagtest.c already uses). */
static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why, long v) {
    uline u; ul_init(&u);
    ul_s(&u, "ACTIONIPC FAIL: "); ul_s(&u, why);
    ul_s(&u, " rc=");             ul_d(&u, v);
    ul_s(&u, "\n");
    wr(ul_end(&u));
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* Poll at most twice around a ring-3 spin (0 syscalls). A PENDING action never
 * becomes anything else in a gate boot, so this must not loop: this process
 * holds is_agent, and an unbounded poll is killed with AGENT_RATE_LIMITED
 * before anything prints (the defect DDR-1016 sec.4 hit and measured). */
static long settle(long id) {
    long st = nsi(SYS_POLL_RESULT, id, 0, 0);
    if (st < 0) fail("poll1", st);
    if (st == AE_PENDING) {
        for (volatile long i = 0; i < 4000000L; i++) { }
        st = nsi(SYS_POLL_RESULT, id, 0, 0);
        if (st < 0) fail("poll2", st);
    }
    return st;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    unsigned long msg[IPC_MSG_WORDS];
    unsigned long back[IPC_MSG_WORDS];
    long id, st, sent, rc;

    /* ---- arm A: propose, be approved, and only then send ------------------ */
    msg[0] = 0xA71C0001UL; msg[1] = 0xA71C0002UL;
    msg[2] = 0xA71C0003UL; msg[3] = 0xA71C0004UL;

    /* The message IS the payload, so the queue entry and the audit record name
     * the bytes the agent then actually sends. */
    id = nsi(SYS_SUBMIT_ACTION, ACTION_SEND_IPC, (long)msg, (long)sizeof msg);
    if (id < 0) fail("submitA", id);
    st = settle(id);

    sent = 0; rc = 0;
    for (int i = 0; i < IPC_MSG_WORDS; i++) back[i] = 0;
    if (st == AE_APPROVED) {
        rc = nsi(SYS_IPC_SEND, SLOT_A, (long)msg, 0);
        sent = 1;
        if (rc == 0) {
            /* Read it back through the kernel: this is what separates "the send
             * returned 0" from "a message is actually in the endpoint". */
            long r = nsi(SYS_IPC_RECV, SLOT_A, (long)back, 0);
            if (r != 0) fail("recvA", r);
        }
    }
    { uline u; ul_init(&u);
      ul_s(&u, "PRADYOS_IPCACT_A st="); ul_d(&u, st);
      ul_s(&u, " sent=");               ul_d(&u, sent);
      ul_s(&u, " rc=");                 ul_d(&u, rc);
      ul_s(&u, " back=");               ul_x(&u, back[0]);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    /* ---- arm B: a verdict that is NOT approval, and obedience the KERNEL
     * confirms. The parent is force-pending, so DDR-839's DAG holds the child
     * PENDING; the probe declines, and the receive below then finds SLOT_B
     * empty and returns -ETIMEDOUT. A probe that sent anyway leaves e->full=1
     * and the same call returns 0. */
    long pid_parent = nsi(SYS_SUBMIT_ACTION, ACTION_DELETE_FILE,
                          (long)"/NOSUCH.TXT", slen("/NOSUCH.TXT"));
    if (pid_parent < 0) fail("submitP", pid_parent);
    long pst = nsi(SYS_POLL_RESULT, pid_parent, 0, 0);
    if (pst < 0) fail("pollP", pst);

    msg[0] = 0xB0B00005UL;              /* distinct from arm A's words */
    id = nsi4(SYS_SUBMIT_CHILD_ACTION, ACTION_SEND_IPC,
              (long)msg, (long)sizeof msg, pid_parent);
    if (id < 0) fail("submitB", id);
    st = settle(id);

    sent = 0;
    if (st == AE_APPROVED) {
        nsi(SYS_IPC_SEND, SLOT_B, (long)msg, 0);
        sent = 1;
    }
    /* Bounded by ipc_recv's own DDR-961 deadline; -ETIMEDOUT is the kernel
     * reporting an empty endpoint, which is the whole claim of this arm. */
    rc = nsi(SYS_IPC_RECV, SLOT_B, (long)back, 0);

    { uline u; ul_init(&u);
      ul_s(&u, "PRADYOS_IPCACT_B st="); ul_d(&u, st);
      ul_s(&u, " pst=");                ul_d(&u, pst);
      ul_s(&u, " sent=");               ul_d(&u, sent);
      ul_s(&u, " rc=");                 ul_d(&u, rc);
      ul_s(&u, "\n"); wr(ul_end(&u)); }

    wr("PRADYOS_IPCACT_OK\n");
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
