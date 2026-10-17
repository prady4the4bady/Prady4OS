/* user/dnstest.c -- DDR-1141 sec.4: SYS_DNS_RESOLVE (NSI 103) probe.
 *
 * Built THREE times from this one source (Makefile, -DDNS_PHASE=n), run in
 * SERIES by main.c, which waits for each pid to vanish before the next:
 *
 *   phase 1  dnstest1.elf  CAP_NET, not sovereign. Arm L + its audit half.
 *                          10.77.0.2:53 is NOT yet on the allowlist.
 *   -------  main.c now seeds netallow_add(10.77.0.2, 53).
 *   phase 2  dnstest2.elf  CAP_NET, not sovereign. Arm A + its audit half.
 *   phase 3  dnstest3.elf  sovereign + CAP_NET. Arm P (privacy on, then off).
 *
 * WHY THREE, and why arm L names the SAME resolver arm A later uses: the refused
 * query must be one that WOULD reach the host responder if it were sent. A
 * refused query to a resolver nothing answers (the first design used 10.77.0.5)
 * is invisible at the far end whether or not the kernel sends it, so arm H could
 * not have seen a kernel with no allowlist check at all. Measured on that first
 * design before any mutant was run; recorded in DDR-1141 sec.6.
 *
 * Three phases because the two identities cannot share a process (a process
 * cannot change its own kernel-set flags: the allowlist is meaningless for a
 * sovereign caller, and only a sovereign may throw privacy mode), and because
 * phase 3's privacy mode must never overlap phase 2's allowlisted query.
 *
 * The resolver is named EXPLICITLY (10.77.0.2, slirp's host alias, which
 * reaches the host responder tools/ci/dns_responder.py on 127.0.0.1:53). The
 * DHCP-learned resolver is slirp's own proxy, which forwards to the host's REAL
 * resolvers; a strict gate cannot depend on the internet (DDR-1141 sec.3).
 *
 * THE OBVIOUS ARMS ARE VACUOUS, which is why the gate reads the HOST side too:
 * "the refused call returned -EPERM" passes on a kernel that audits a refusal
 * and sends the query anyway. The responder logs every name it receives, and
 * the gate requires `denied` and `private` to be ABSENT from that log (arm H).
 * And "the allowed call returned 0" passes on a kernel that fabricates an
 * answer: 10.77.0.99 exists only in the responder's reply, so it is the one
 * value this probe cannot manufacture.
 *
 * The probe REPORTS and the gate JUDGES (DDR-1020): every value is printed,
 * nothing fail()s before a print, so no arm can be silently removed. Each line
 * is ONE write(2) (DDR-1056). Freestanding, no writable globals (user.ld).
 */

#define SYS_GETPID         2
#define SYS_WRITE          6
#define SYS_EXIT           4
#define SYS_SET_MODE      30
#define SYS_READ_AUDIT    37
#define SYS_DNS_RESOLVE  103

#ifndef DNS_PHASE
#error "build with -DDNS_PHASE=1|2|3"
#endif

/* Must match kernel/aether/aether.h. */
#define ACTION_NET_DNS       14
#define AR_CAP_DENIED         7
#define AR_NET_CONNECT       10
#define MODE_PRIVACY_ON       2
#define MODE_PRIVACY_OFF      3

#define RESOLVER      0x0A4D0002u   /* 10.77.0.2 -- listed by main.c AFTER phase 1 */

struct audit_entry {
    unsigned long timestamp;
    unsigned pid, action_type;
    unsigned long action_id;
    unsigned result, _pad;
};

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* A line builder: append into a stack buffer, then ONE write(2). */
struct line { char b[160]; long n; };
static void ls(struct line *l, const char *s) {
    while (*s && l->n < (long)sizeof l->b - 1) l->b[l->n++] = *s++;
}
static void ld(struct line *l, long v) {
    char t[24]; int k = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    if (v < 0) ls(l, "-");
    do { t[k++] = (char)('0' + (u % 10)); u /= 10; } while (u && k < 23);
    while (k > 0 && l->n < (long)sizeof l->b - 1) l->b[l->n++] = t[--k];
}
static void lx(struct line *l, unsigned v) {
    const char *hx = "0123456789ABCDEF";
    ls(l, "0x");
    for (int i = 28; i >= 0; i -= 4)
        if (l->n < (long)sizeof l->b - 1) l->b[l->n++] = hx[(v >> i) & 0xF];
}
static void lflush(struct line *l) {
    ls(l, "\n");
    nsi(SYS_WRITE, 1, (long)l->b, l->n);
    l->n = 0;
}

static long resolve(const char *name, unsigned resolver, unsigned *ip) {
    *ip = 0;
    return nsi(SYS_DNS_RESOLVE, (long)name, (long)ip, (long)resolver);
}

#if DNS_PHASE != 3
static int seen(const struct audit_entry *e, long n, unsigned pid,
                unsigned long id, unsigned result) {
    for (long i = 0; i < n; i++)
        if (e[i].pid == pid && e[i].action_type == ACTION_NET_DNS &&
            e[i].action_id == id && e[i].result == result)
            return 1;
    return 0;
}
#endif

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    struct line l; l.n = 0;
    unsigned ip;
    long rc;

    const unsigned long id = ((unsigned long)RESOLVER << 16) | 53ul;
#if DNS_PHASE == 1
    /* Arm L -- resolver NOT on the allowlist yet: refused, and NOT sent. */
    rc = resolve("denied.pradyos.test", RESOLVER, &ip);
    ls(&l, "PRADYOS_DNS_L rc="); ld(&l, rc); ls(&l, " ip="); lx(&l, ip); lflush(&l);
#elif DNS_PHASE == 2
    /* Arm A -- now listed: a real query, and a real answer. */
    rc = resolve("allowed.pradyos.test", RESOLVER, &ip);
    ls(&l, "PRADYOS_DNS_A rc="); ld(&l, rc); ls(&l, " ip="); lx(&l, ip); lflush(&l);
#endif
#if DNS_PHASE != 3
    /* Arm U -- the decision is in the audit ring, attributed to THIS pid. */
    unsigned pid = (unsigned)nsi(SYS_GETPID, 0, 0, 0);
    struct audit_entry buf[64];
    long n = nsi(SYS_READ_AUDIT, (long)buf, 64, 0);
    int hit = (n > 0) && seen(buf, n, pid, id,
                              DNS_PHASE == 1 ? AR_CAP_DENIED : AR_NET_CONNECT);
    ls(&l, DNS_PHASE == 1 ? "PRADYOS_DNS_U1 denied=" : "PRADYOS_DNS_U2 allowed=");
    ld(&l, hit); lflush(&l);
#else
    (void)id;
    /* Arm P -- privacy ON refuses even a sovereign, allowlisted query ... */
    long mon = nsi(SYS_SET_MODE, MODE_PRIVACY_ON, 0, 0);
    long on = resolve("private.pradyos.test", RESOLVER, &ip);
    unsigned ip_on = ip;
    /* ... and privacy OFF lets the same path through, so the refusal above was
     * privacy and not a broken resolver path. */
    long moff = nsi(SYS_SET_MODE, MODE_PRIVACY_OFF, 0, 0);
    rc = resolve("released.pradyos.test", RESOLVER, &ip);
    ls(&l, "PRADYOS_DNS_P mode="); ld(&l, mon); ld(&l, moff);
    ls(&l, " on="); ld(&l, on); ls(&l, " onip="); lx(&l, ip_on);
    ls(&l, " off="); ld(&l, rc); ls(&l, " ip="); lx(&l, ip); lflush(&l);
#endif
    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
