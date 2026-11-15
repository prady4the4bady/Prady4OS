/* kernel/syscall/sys_proc.c — sys_lseek / sys_getcwd (Phase 5b slice 5, ADR-022).
 *
 * lseek repositions a VFS fd; getcwd returns the process cwd, which is "/" until
 * a real working-directory / mount-point namespace exists (deferred, see
 * docs/build_status.md). sys_getpid is unchanged in syscall.c (SYS_GETPID).
 */
#include "sys_proc.h"
#include "syscall.h"
#include "sched.h"
#include "fd.h"
#include "vfs.h"
#include "uaccess.h"
#include "errno.h"
#include "vmm.h"               /* VMM_USER_MIN/MAX for fs_base validation */
#include "cpu_mitigations.h"   /* cpu_wrmsr + MSR_IA32_FS_BASE (PROC-D)   */
#include "rtc.h"               /* rtc_now for SYS_CLOCK (DDR-709)          */
#include "metric_page.h"   /* DDR-812: shutdown commit */
#include "acpi.h"              /* acpi_poweroff / acpi_power_available (DDR-746) */
#include "lapic.h"             /* lapic_cpu_count (DDR-748)                        */
#include "irq.h"               /* g_ticks (DDR-748)                               */
#include "pmm.h"               /* pmm_free_page_count (DDR-748)                    */
#include "string.h"            /* memset for SYS_SYSINFO (cpu_cpuid via cpu_mitigations.h above) */
#include "vdso_page.h"         /* ktime_mono_ns / ktime_real_ns (DDR-1188 K7) */
#include "signal.h"            /* signal_interruptible (nanosleep EINTR)       */
#include "spinlock.h"

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static long sys_lseek(long fd, long offset, long whence, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a4;
    struct fd_entry *e = fd_get(current_thread, (int)fd);
    if (!e)
        return -EBADF;
    if (e->kind != FD_VFS || !e->file)
        return -ESPIPE;                       /* console / non-seekable */

    int64_t base;
    switch (whence) {
        case SEEK_SET: base = 0;                          break;
        case SEEK_CUR: base = (int64_t)e->off;            break;
        case SEEK_END: base = (int64_t)e->file->size;     break;
        default:       return -EINVAL;
    }
    int64_t pos = base + (int64_t)offset;
    if (pos < 0)
        return -EINVAL;
    e->off = (uint64_t)pos;
    return (long)pos;
}

static long sys_getcwd(long ubuf, long size, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    static const char cwd[] = "/";
    size_t need = sizeof cwd;                  /* 2: '/' + NUL */
    if (size < 0 || (uint64_t)size < need)
        return -ERANGE;
    if (copyout((void __user *)(uintptr_t)ubuf, cwd, need) < 0)
        return -EFAULT;
    return (long)need;                         /* Linux getcwd returns length incl. NUL */
}

/* sys_set_tls (PROC-D, ADR-023) — set the calling thread's pointer (FS base).
 * The analog of Linux arch_prctl(ARCH_SET_FS): record it in the TCB (authority
 * for switch-in restore) and program IA32_FS_BASE for the current run. The base
 * must be 0 or a user-range address — ring 3 may never aim %fs at kernel space. */
static long sys_set_tls(long fs_base, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    uint64_t fb = (uint64_t)fs_base;
    if (fb != 0 && (fb < VMM_USER_MIN || fb >= VMM_USER_MAX))
        return -EINVAL;
    current_thread->fs_base = fb;
    cpu_wrmsr(MSR_IA32_FS_BASE, fb);
    return 0;
}

/* SYS_CLOCK (DDR-709): seconds since midnight from the RTC, for the time-of-day
 * ambiance selection (the vDSO clock is monotonic-since-boot, not wall-clock). */
static long sys_clock(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a1; (void)a2; (void)a3; (void)a4;
    struct rtc_time t;
    rtc_now(&t);
    return (long)((uint32_t)t.hour * 3600u + (uint32_t)t.minute * 60u + t.second);
}

/* SYS_GETPROCS (DDR-743): ring-3 process listing for `ps`. Snapshots the
 * index-th thread in the scheduler ring via sched_snapshot (which walks the ring
 * under IRQ-off; sys_proc.c must not touch the ring itself), then copies the
 * pure-value struct out. Returns 1 (filled), 0 (index past the last thread), or
 * -errno. Best-effort: a create/exit between successive indices only adds/drops
 * a row, which `ps` tolerates. */
static long sys_getprocs(long index, long uout, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a3; (void)a4;
    if (index < 0)
        return -EINVAL;
    struct procinfo pi;
    if (!sched_snapshot((int)index, &pi))
        return 0;                                 /* end of the ring */
    if (copyout((void __user *)(uintptr_t)uout, &pi, sizeof pi) < 0)
        return -EFAULT;
    return 1;
}

/* SYS_POWEROFF (DDR-746): ACPI S5 soft-off. CAP_SOVEREIGN-gated (mirrors
 * SYS_SET_MODE) — a non-sovereign caller gets -EPERM and survives, no
 * self-escalation (ADR-026 §D6). On authority it does not return; if the S5
 * path was never resolved it reports -ENODEV instead of hanging. */
static long sys_poweroff(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a1; (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign)
        return -EPERM;
    if (!acpi_power_available())
        return -ENODEV;
    /* DDR-812: clean-shutdown commit, before the machine goes away. */
    metric_lockbox_commit_shutdown();
    acpi_poweroff();                              /* no return */
}

/* SYS_REBOOT (DDR-747): ACPI/PC reset. CAP_SOVEREIGN-gated like SYS_POWEROFF;
 * the 0xCF9/8042 fallbacks always reset on a PC, so no -ENODEV. No return. */
static long sys_reboot(long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a1; (void)a2; (void)a3; (void)a4;
    if (!current_thread->is_sovereign)
        return -EPERM;
    acpi_reboot();                                /* no return */
}

/* SYS_SYSINFO (DDR-748): read-only CPU + system introspection. No capability —
 * the fields are non-sensitive (CPU identity, counts, uptime, free frames). */
struct sysinfo {
    char     vendor[16];
    char     brand[64];
    uint32_t cpu_count;
    uint32_t feat_edx;
    uint32_t feat_ecx;
    uint32_t _pad;
    uint64_t uptime_ticks;
    uint64_t free_pages;
};

static long sys_sysinfo(long uout, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    struct sysinfo si;
    memset(&si, 0, sizeof si);

    uint32_t a, b, c, d;
    /* CPUID leaf 0: vendor string in EBX,EDX,ECX (12 bytes). */
    cpu_cpuid(0, 0, &a, &b, &c, &d);
    memcpy(si.vendor + 0, &b, 4);
    memcpy(si.vendor + 4, &d, 4);
    memcpy(si.vendor + 8, &c, 4);
    si.vendor[12] = 0;

    /* CPUID leaf 1: feature flags. */
    cpu_cpuid(1, 0, &a, &b, &c, &d);
    si.feat_ecx = c;
    si.feat_edx = d;

    /* CPUID leaves 0x80000002..4: processor brand string (48 bytes), if present. */
    cpu_cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a >= 0x80000004u) {
        for (uint32_t leaf = 0; leaf < 3; leaf++) {
            cpu_cpuid(0x80000002u + leaf, 0, &a, &b, &c, &d);
            memcpy(si.brand + leaf * 16 + 0,  &a, 4);
            memcpy(si.brand + leaf * 16 + 4,  &b, 4);
            memcpy(si.brand + leaf * 16 + 8,  &c, 4);
            memcpy(si.brand + leaf * 16 + 12, &d, 4);
        }
        si.brand[48] = 0;
    }

    si.cpu_count    = lapic_cpu_count();
    if (si.cpu_count == 0) si.cpu_count = 1;    /* single-CPU fallback (no MADT) */
    si.uptime_ticks = g_ticks;
    si.free_pages   = pmm_free_page_count();

    if (copyout((void __user *)(uintptr_t)uout, &si, sizeof si) < 0)
        return -EFAULT;
    return 0;
}

/* SYS_TIME (DDR-749): ring-3 wall-clock date/time. No capability (non-sensitive,
 * like SYS_CLOCK). Copies the broken-down RTC reading out verbatim. */
static long sys_time(long uout, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    struct rtc_time t;
    rtc_now(&t);
    if (copyout((void __user *)(uintptr_t)uout, &t, sizeof t) < 0)
        return -EFAULT;
    return 0;
}

/* SYS_MEMINFO (DDR-752): physical-frame accounting. No capability. used is
 * derived (total - free), never stored, so it can't drift. */
struct meminfo {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint32_t page_size;
    uint32_t _pad;
};

static long sys_meminfo(long uout, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    struct meminfo mi;
    mi.total_pages = pmm_total_page_count();
    mi.free_pages  = pmm_free_page_count();
    mi.used_pages  = (mi.total_pages >= mi.free_pages)
                   ? (mi.total_pages - mi.free_pages) : 0;
    mi.page_size   = 4096;
    mi._pad        = 0;
    if (copyout((void __user *)(uintptr_t)uout, &mi, sizeof mi) < 0)
        return -EFAULT;
    return 0;
}

/* SYS_SETNAME (DDR-756): rename the CALLING thread (self only — no authority to
 * touch another). Copies up to 15 chars into the tcb's own name buffer and
 * repoints t->name at it; ps (SYS_GETPROCS reads t->name) reflects it at once. */
static long sys_setname(long uname, long a2, long a3, long a4, long a5, long a6) {
    (void)a5; (void)a6;
    (void)a2; (void)a3; (void)a4;
    struct tcb *t = current_thread;
    char nm[16];
    if (copyinstr(nm, (const void __user *)(uintptr_t)uname, sizeof nm, 0) < 0)
        return -EFAULT;
    int i = 0;
    for (; i < 15 && nm[i]; i++)
        t->name_buf[i] = nm[i];
    t->name_buf[i] = 0;
    t->name = t->name_buf;
    return 0;
}

/* ---- DDR-1188 P1 (C1.5): the time ABI --------------------------------- */

struct k_timespec { int64_t tv_sec; int64_t tv_nsec; };
#define NS_PER_SEC   1000000000ull
#define NS_PER_TICK  10000000ull       /* PIT at 100 Hz */
#define CLK_REALTIME  0
#define CLK_MONOTONIC 1

static long sys_clock_gettime(long clk, long uts, long a3, long a4, long a5, long a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    uint64_t ns;
    if (clk == CLK_MONOTONIC)
        ns = ktime_mono_ns();
    else if (clk == CLK_REALTIME)
        ns = ktime_real_ns();
    else
        return -EINVAL;
    struct k_timespec ts = { (int64_t)(ns / NS_PER_SEC), (int64_t)(ns % NS_PER_SEC) };
    if (copyout((void __user *)(uintptr_t)uts, &ts, sizeof ts) < 0)
        return -EFAULT;
    return 0;
}

/* Nobody wakes a sleeper early: the wait is a timed block on a word no one
 * sets. Static storage (DDR-1200 Amendment 2 point 4: the tick sweep may take
 * a timed wait's lock at any time). */
static spinlock_t g_nsleep_lk = SPINLOCK_INIT;

/* Sleeps until CLOCK_MONOTONIC reaches start + req, never less: each block is
 * rem / 10 ms + 1 ticks, and the loop re-reads the clock until the deadline
 * has passed. A signal ends the sleep with -EINTR and the remaining time in
 * *urem (DDR-1188 A2-1: the signal wakes the block). */
static long sys_nanosleep(long ureq, long urem, long a3, long a4, long a5, long a6) {
    (void)a3; (void)a4; (void)a5; (void)a6;
    struct k_timespec req;
    if (copyin(&req, (const void __user *)(uintptr_t)ureq, sizeof req) < 0)
        return -EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= (int64_t)NS_PER_SEC)
        return -EINVAL;
    if ((uint64_t)req.tv_sec > (UINT64_MAX / NS_PER_SEC) / 2)
        return -EINVAL;                 /* a deadline that would overflow */
    uint64_t want = (uint64_t)req.tv_sec * NS_PER_SEC + (uint64_t)req.tv_nsec;
    uint64_t deadline = ktime_mono_ns() + want;
    struct tcb *t = current_thread;
    /* DDR-1188 A2-1: sig_raise wakes a blocked thread through futex_kick,
     * which takes t->fx_lock and unblocks t. Registering this sleep's lock
     * there makes a signal end the block. The signal check and the block
     * both run under the lock, so a signal raised after the check waits for
     * the wait to be published and then wakes it: none is lost. */
    long ret;
    uint64_t rem = 0;
    uint64_t fl = spin_lock_irqsave(&g_nsleep_lk);
    __atomic_store_n(&t->fx_lock, &g_nsleep_lk, __ATOMIC_SEQ_CST);
    for (;;) {
        uint64_t now = ktime_mono_ns();
        if (now >= deadline) { ret = 0; break; }        /* done, as on Linux */
        rem = deadline - now;
        if (signal_interruptible(t) || signal_kill_pending(t)) { ret = -EINTR; break; }
        uint64_t ticks = rem / NS_PER_TICK;     /* MUTANT C2 */
        if (!ticks) { ret = 0; break; }
        volatile int done = 0;
        sched_block_timeout(&g_nsleep_lk, &done, ticks);
    }
    __atomic_store_n(&t->fx_lock, (spinlock_t *)0, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&g_nsleep_lk, fl);
    if (ret == -EINTR && urem) {
        struct k_timespec r = { (int64_t)(rem / NS_PER_SEC), (int64_t)(rem % NS_PER_SEC) };
        if (copyout((void __user *)(uintptr_t)urem, &r, sizeof r) < 0)
            return -EFAULT;
    }
    return ret;
}

void sys_proc_register(void) {
    syscall_register(SYS_LSEEK,   sys_lseek);
    syscall_register(SYS_GETCWD,  sys_getcwd);
    syscall_register(SYS_SET_TLS, sys_set_tls);
    syscall_register(SYS_CLOCK,   sys_clock);     /* DDR-709 */
    syscall_register(SYS_GETPROCS, sys_getprocs); /* DDR-743 */
    syscall_register(SYS_POWEROFF, sys_poweroff); /* DDR-746 */
    syscall_register(SYS_REBOOT,   sys_reboot);   /* DDR-747 */
    syscall_register(SYS_SYSINFO,  sys_sysinfo);  /* DDR-748 */
    syscall_register(SYS_TIME,     sys_time);     /* DDR-749 */
    syscall_register(SYS_MEMINFO,  sys_meminfo);  /* DDR-752 */
    syscall_register(SYS_SETNAME,  sys_setname);  /* DDR-756 */
    syscall_register(SYS_CLOCK_GETTIME, sys_clock_gettime); /* DDR-1188 A1-4 */
    syscall_register(SYS_NANOSLEEP,     sys_nanosleep);     /* DDR-1188 A1-4 */
}
