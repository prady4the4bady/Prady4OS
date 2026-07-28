# Changelog

All notable changes to PRADYOS. Format loosely follows Keep a Changelog;
decisions live in `docs/decisions/ADR-*.md` and `docs/ddr/DDR-*.md`.

## [v0.1.0-aether] — 2026-07-29

First tagged release. The NEXUS kernel (x86_64) plus the complete AETHER
host-side agent layer.

### The headline: BUG-1

BUG-1 — intermittent `-smp 4` gate failures, open since DDR-775 — is closed. It
was **two independent defects producing one symptom**, and it was never the
scheduler defect it appeared to be.

**What it looked like:** `AGENT_METRICS FAIL: agent never observed as scheduled`,
on a different gate every run, never reproducible locally (0/4 attempts).

**What it actually was:**

1. **A CMOS/RTC SMP race (DDR-796).** `cmos_read()` is a two-port sequence —
   `outb(0x70, reg)` selects a register, `inb(0x71)` reads it — over state owned
   by the chipset, not the CPU, with no lock. Under `-smp 4` two CPUs interleave
   and each reads the other's register, so `SYS_CLOCK` can run backwards. The
   metrics probe treats any decrease as a midnight wrap (`+86400`), so its
   120-second window collapsed to zero and it reported a scheduling failure that
   had not happened. Fixed with one IRQ-saving spinlock across the whole of
   `rtc_now()`.

2. **A serial flood (DDR-797).** `syscallfuzz`'s `WILD[]` listed `0x8000000000`
   as an "unmapped user" address; `user/user.ld:13` bases the user image at
   exactly that address. Passing it as every argument made `SYS_WRITE` become
   `write(fd=0, buf=<image base>, count=~512 GB)`, and the kernel correctly wrote
   what was mapped — dumping the probe's own image to the console ~20× per boot,
   **83% of all serial traffic**. That delayed boot enough for the metrics probe
   to lose its race even after fix (1). Fixed by using an address that is
   genuinely unmapped. Serial output: **97,564 bytes → 5,901**.

**The diagnostic value, which outlasts the fix:** BUG-1 converted from
*unreproducible-intermittent* to *reliably reproducible* once two ingredients
were combined — `-smp 4` **and** a full-length window (a gate declaring
`FORBIDDEN_SENTINEL`, so DDR-785 early exit does not cut the boot short). All
four earlier local attempts had one ingredient, never both.

**What made that possible was the harness fix**, not more kernel reading. Adding
`GLOBAL_FORBIDDEN` to `boot_test.sh` — every pattern any gate forbids is
forbidden in *all* gates, since every boot runs every probe — turned a failure
that had been silently tolerated into one that failed 4/4 runs. The bug had been
frequent all along; nothing was checking for it.

Two wrong conclusions are recorded rather than quietly dropped: it is not
scheduler starvation (DDR-791 finding 2), and DDR-791's A/B that "exonerated" the
fuzz probe was **invalid** — its three arms reported byte-identical counts
because the ELF is embedded by `incbin` and editing the `.c` rebuilt nothing.
An A/B whose arms produce byte-identical output has not proven equivalence; it
has failed to rebuild.

### Added — kernel (x86_64)

- Sealed objective-function region (F#68 / DDR-795): one frame, kernel-writable,
  mapped read-only + NX into every user address space. A ring-3 store faults at
  `METRIC_USER_VA+0x40` and is killed cleanly; the gate asserts the fault
  address, not merely the absence of a success message.
- `GLOBAL_FORBIDDEN` harness check (DDR-791) + BSP-liveness instrument (DDR-777).
- Gates: `smoke-metric`, `smoke-rtc-smp`, `smoke-serialflood`,
  `smoke-rqstress-liveness`.

### Added — kernel (new architectures)

- **aarch64** and **riscv64** bootstraps (ADR-034): reach C, bring up PL011 /
  NS16550A consoles, print `NEXUS KERNEL OK`. CI-green on QEMU `virt`.
  **Boot-only** — see Caveats.

### Added — AETHER Python layer

Sections B (B-01…B-17), C (C-01…C-10), D (D-01…D-15) and I (I-01…I-10) complete,
plus:

- `ollama_bridge` (DDR-792) — retry transport never semantics; a read timeout
  *after* first byte is not retried; the 30 s deadline covers all attempts.
- `cloud_bridge` (DDR-793) — built once all four CONFIRM-1 gates were met.
- F#68 metric lockbox — objective function immutable below `CAP_SOVEREIGN`, with
  a hash chain verified at load so a direct edit of the store is caught.
- `capability/` (I-02) fail-closed principal enforcement, `daemon/` (I-10) as the
  only spawn path, privacy netfilter, shared egress rate limiter.

### Caveats — read before relying on this tag

- **`cloud_bridge` is built but NOT enabled.** DDR-794's R1 (a sovereign thread
  bypasses both the `CAP_NET` check and the egress allowlist) and R3 (no
  per-destination egress audit) are kernel-side and open.
- **aarch64/riscv64 are boot-only.** The ~107 x86_64 smoke gates are not ported;
  those architectures have no PMM, VMM, scheduler, VFS or syscall surface yet.
- **One skipped test**: `test_quarantine.py:69` (B-05 symlink), platform-limited.
- The privacy netfilter covers the *Python transport boundary*, not ring-3
  sockets; a kernel lwIP hook is future work.
