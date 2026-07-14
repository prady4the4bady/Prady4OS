# DDR-748 — `SYS_SYSINFO`: CPU + system introspection

**Status:** proposed (pre-code)
**Layer:** syscall + user probe. Read-only system introspection.

## Problem

Ring 3 has no way to learn what machine it is running on or the system's live
state: no CPU vendor/brand, no feature bits, no CPU count, uptime, or free
memory. Every OS exposes this (`uname`, `sysinfo(2)`, `/proc/cpuinfo`); PRADYOS
exposes none of it. The kernel already has every primitive — `cpu_cpuid`
(`cpu_mitigations.h`), `lapic_cpu_count`, `g_ticks` (100 Hz), `pmm_free_page_count`
— nothing wires them to a syscall.

## Decision

**`SYS_SYSINFO` (NSI 71)** — `(struct sysinfo *out) -> 0 | -EFAULT`. No
capability (read-only, non-sensitive, like `SYS_GETPID`). The handler fills a
pure-value struct and `copyout`s it:

```c
struct sysinfo {
    char     vendor[16];   /* CPUID leaf 0 (EBX,EDX,ECX) — 12 chars + NUL       */
    char     brand[64];    /* CPUID leaves 0x80000002..4 — 48 chars + NUL       */
    uint32_t cpu_count;    /* lapic_cpu_count()                                 */
    uint32_t feat_edx;     /* CPUID leaf 1 EDX (SSE2 bit 26, etc.)              */
    uint32_t feat_ecx;     /* CPUID leaf 1 ECX (SSE4.1 bit 19, AVX bit 28, ...) */
    uint32_t _pad;
    uint64_t uptime_ticks; /* g_ticks (100 Hz since boot)                       */
    uint64_t free_pages;   /* pmm_free_page_count() (4 KiB frames)              */
};
```

The brand string is populated only if CPUID leaf `0x80000000` reports
`>= 0x80000004` (QEMU does); otherwise `brand[0] = 0`. All CPUID reads use the
existing `cpu_cpuid`; the handler runs on the calling CPU (per-CPU CPUID is
identical here). Total RAM is out of scope (the PMM tracks only the live free
count); free-pages is the useful availability signal.

## Gate — `smoke-sysinfo` (new; 86 → 87)

A freestanding probe `user/sysinfotest.c` (musl-free, `user.ld`, default root)
calls `SYS_SYSINFO`, writes the `vendor`/`brand` strings to the console for
visibility, validates `cpu_count >= 1`, `vendor[0] != 0`, `brand[0] != 0`, and
`free_pages > 0`, and prints `PRADYOS_SYSINFO_OK` on success or `SYSINFO FAIL`
otherwise. Spawned like the other freestanding probes (`user_boot_from_sfs`).
`smoke-sysinfo` asserts `PRADYOS_SYSINFO_OK` (EXTRA_SENTINEL) with `SYSINFO FAIL`
forbidden — deterministic on QEMU (CPUID vendor/brand and the frame count are
stable per boot).

## Non-goals

- No total-RAM / used-RAM accounting (PMM tracks only free frames); no per-zone
  or slab stats.
- No load average, no per-CPU utilization (that is the DDR-735 agent-metrics
  lane's territory), no thermal/frequency.
- No PRISM `uname` builtin yet — the syscall + probe first; a shell surface can
  follow (keeps this slice off the shell per the fs/shell pivot).
