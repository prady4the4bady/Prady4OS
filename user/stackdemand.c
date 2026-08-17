/* user/stackdemand.c — ring-3 demand-paged-stack probe (ADR-038 Option 3).
 *
 * ADR-038 replaced the eager 8 MiB stack map (2048 frames per process) with an
 * eagerly-mapped top window of USER_STACK_EAGER_PAGES plus #PF-driven growth
 * below it. Three things can go wrong, and this probe exercises all three in
 * one boot. Arms run in this order because arm 3 is fatal by design.
 *
 * ARM 1 — GROWTH. Walk the stack down past the eager window, touching one byte
 *   per page, well beyond it. Each touch below the window must take a growth
 *   fault and succeed. Prints PRADYOS_STACK_GROW_OK.
 *
 * ARM 2 — SYSCALL WITH A DEEP STACK POINTER. This is the arm that exists
 *   because of a real regression: vmm_user_range_ok (ADR-022) validates syscall
 *   pointers WITHOUT faulting, so a buffer on an untouched stack page is
 *   rejected with -EFAULT. Option 1 of ADR-038 failed 0/30 on exactly this.
 *   The buffer here sits deliberately BELOW the eager window, is written (so
 *   the page is present) and then passed through write(2). A gate that only
 *   tested arm 1 would not have caught the regression that motivated it.
 *   Prints PRADYOS_STACK_SYSCALL_OK.
 *
 * ARM 3 — GUARD PAGE. Recurse without bound until the stack runs past
 *   USER_STACK_BOT into the guard page. The guard is below the growth range, so
 *   the #PF handler must NOT satisfy it — the process must be killed and the
 *   KERNEL must survive. This arm never returns, so it is last and prints its
 *   intent first (PRADYOS_STACK_GUARD_ARM) so the gate can tell "reached the
 *   overflow arm" from "died earlier".
 *
 * The gate also asserts the kernel is still alive AFTER the kill, and that
 * pmmfree recovers — a partially-grown stack must not leak frames on teardown.
 */
#include <stdio.h>
#include <stdint.h>

/* Raw SYS_WRITE rather than libc write(): the static musl link used for probes
 * does not export write(), and more importantly a raw syscall passes THIS
 * function's stack buffer straight to the kernel. libc's write() would copy
 * through stdio's own buffer first, so the kernel would validate stdio's
 * pointer, not a deep stack pointer — the arm would test nothing. */
#define SYS_WRITE 6
static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* Deeper than USER_STACK_EAGER_PAGES (8) so the growth path is genuinely
 * exercised; 64 pages = 256 KiB, far past the window and far short of 8 MiB. */
#define GROW_PAGES 64
#define PAGE       4096

/* volatile + a returned checksum: without them the compiler is entitled to
 * delete the whole traversal, and the arm would pass by not existing. */
static volatile uint64_t g_sink;

static uint64_t touch_down(int pages) {
    volatile char frame[PAGE];
    frame[0] = (char)pages;
    frame[PAGE - 1] = (char)(pages ^ 0x5a);
    g_sink += (uint64_t)frame[0] + (uint64_t)frame[PAGE - 1];
    if (pages <= 0)
        return g_sink;
    return touch_down(pages - 1);
}

/* Arm 3: unbounded recursion. Each frame touches its own page so the stack
 * actually advances rather than being optimised into a loop. */
/* The recursion is guarded by a volatile so it is not UNCONDITIONAL self-call:
 * clang rejects that outright under -Werror (-Winfinite-recursion), and the
 * project forbids suppression flags (R2). g_sink is volatile, so the compiler
 * cannot prove the branch is always taken, while at runtime it always is —
 * which is exactly what arm 3 needs. */
static uint64_t overflow_forever(uint64_t depth) {
    volatile char frame[PAGE];
    frame[0] = (char)depth;
    g_sink += (uint64_t)frame[0];
    if (g_sink == (uint64_t)-1)
        return depth;                 /* unreachable in practice; defeats the warning */
    return overflow_forever(depth + 1) + 1;
}

int main(void) {
    /* ---- ARM 1: growth below the eager window ---------------------------- */
    uint64_t sum = touch_down(GROW_PAGES);
    if (sum == 0) {
        printf("STACKDEMAND FAIL — growth traversal produced no writes\n");
        fflush(stdout);
        return 1;
    }
    printf("PRADYOS_STACK_GROW_OK pages=%d sum=%llu\n",
           GROW_PAGES, (unsigned long long)sum);
    fflush(stdout);

    /* ---- ARM 2: syscall with a buffer below the eager window -------------- */
    /* 32 KiB of locals puts this buffer ~8 pages down at minimum, past the
     * 8-page eager window. It is written before the syscall, so the pages are
     * present — the point is that they were DEMAND-mapped, not eagerly mapped,
     * and vmm_user_range_ok must still accept them. */
    {
        volatile char pad[24 * PAGE];
        char buf[64];
        pad[0] = 1;
        pad[sizeof pad - 1] = 2;
        g_sink += (uint64_t)pad[0] + (uint64_t)pad[sizeof pad - 1];
        for (int i = 0; i < 63; i++)
            buf[i] = 'A' + (i % 26);
        buf[63] = '\n';
        /* write(2) copies FROM this stack buffer: the kernel validates the
         * range with vmm_user_range_ok, which never faults. If the pages were
         * not present this returns -EFAULT and the arm fails. */
        long n = nsi(SYS_WRITE, 1, (long)buf, (long)sizeof buf);
        if (n != (long)sizeof buf) {
            printf("STACKDEMAND FAIL — deep-stack write returned %ld, want %d\n",
                   n, (int)sizeof buf);
            fflush(stdout);
            return 1;
        }
        printf("PRADYOS_STACK_SYSCALL_OK bytes=%ld\n", n);
        fflush(stdout);
    }

    /* ---- ARM 3: guard page — fatal by design, so it goes last ------------- */
    printf("PRADYOS_STACK_GUARD_ARM\n");
    fflush(stdout);
    g_sink += overflow_forever(0);

    /* Unreachable: the guard page must kill this process. Reaching here means
     * the guard was fillable, which is the defect arm B of the ADR describes. */
    printf("STACKDEMAND FAIL — overflow returned; guard page was fillable\n");
    fflush(stdout);
    return 1;
}
