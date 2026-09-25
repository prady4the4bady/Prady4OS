# Changelog

All notable changes to PRADYOS. Format loosely follows Keep a Changelog;
decisions live in `docs/decisions/ADR-*.md` and `docs/ddr/DDR-*.md`.

## [v1.0.0] — UNRELEASED (release candidate; not tagged)

**Status: not tagged and not built as a release.** The operator gives the final
go/no-go for the ISO build and the `v1.0.0` tag. Everything below describes the
release candidate on `dev/phase1-seyp3n`. Decisions are cited by operator
comment (PR #17: 5822830053 and 5822896320, both OWNER-verified).

### Distribution restriction (operator, #49)

**This v1.0.0 build is NOT authorized for public distribution outside the
current development and test group until a real export-control legal review
has been done.** The image contains ChaCha20-Poly1305, X25519, HKDF, SHA-2,
SHA-3/SHAKE, Ed25519 and ML-DSA-44. No export classification has been made or
is implied. This does not restrict building or internal testing.

### Branding (operator, #14)

The logo and branding in this build are a **provisional placeholder**. They must
be replaced or licensed before any public marketing use.

### Support (operator, #68)

v1.0.0 is supported on a **best-effort basis**. No formal end-of-life date is
set.

### Integrity (operator, #63)

The ISO is published with a **SHA-256 checksum** (`build/pradyos.iso.sha256`).
**It is not signed.** Signing is formally deferred: a signature is only as good
as the custody of its key, and that is the open DDR-1059 question.

### What v1 is

- A from-scratch x86_64 kernel (NEXUS), bootable from one ISO by **BIOS or
  UEFI**. This is verified on every CI suite by `smoke-iso-x86`,
  `smoke-iso-userspace` and `smoke-uefi`.
- A **live** system. The root filesystem on the ISO is a RAM disk (DDR-972), so
  nothing persists across a reboot.
- The PRISM shell, a compositing desktop (up to four terminal windows), and the
  AETHER agent layer: Section 3C's eight action types, all gated.
- Networking over virtio-net or e1000e, with **DHCP** and **DNS**
  (DDR-1141). The DNS path goes through the same privacy-mode, CAP_NET,
  allowlist and audit checks as a socket connect. There is **no static
  fallback address**: without a lease, the interface says it is unconfigured.
- Post-quantum signature **primitives**: ML-DSA-44 keyGen, sign and verify,
  checked against NIST ACVP vectors, plus a tamper-evident SHA-256 audit chain.
  **This is not a post-quantum signed ledger** (DDR-1059), and **it is not a
  FIPS 140 validated module**: the vectors test algorithm conformance only.
- A **UEFI GOP framebuffer** (DDR-1142). On UEFI firmware the loader asks for
  the mode the firmware already set, and the desktop draws on it when there is
  no virtio-gpu. `smoke-gop` checks this against the **scanout**, not memory
  (a QMP screendump), for both the kernel's own pixels and the ring-3
  compositor's. **The proxy is OVMF on QEMU's std-vga.** No physical UEFI
  machine was tested. Limits: only the BGRX pixel format is accepted (others
  are refused, not converted), the firmware's resolution is used as-is, and a
  mode whose stride differs from its width is **not covered** by the gate,
  because the proxy's mode never has one.

### Known limitations: read before testing

**Open defects, named (operator, #1–#4):**
- **OPEN-1 route 1.** A CI-only hang in `smoke-surfdestroy`. It is not
  reproduced locally, there has been no occurrence since DDR-1009 §2, and no
  mechanism is named. Instruments are armed for its next occurrence
  (DDR-1124).
- **OPEN-12.** A ring-0 exception, seen once. A related defect was fixed
  (DDR-996), but the original's identity is unproven.
- **OPEN-13.** A kernel-heap double free, seen once. No mechanism is known. The
  next occurrence names both free sites (DDR-1024).
- **OPEN-2.** *"The dominant OPEN-2 mechanism (double dispatch, DDR-1139) is
  fixed and confirmed against a pre-registered criterion. Two rarer panic
  signatures seen before the fix have not recurred and are unattributed."*
  **OPEN-2 is not claimed closed as a whole.**

**Hardware and platform:**
- **Tested only under QEMU.** One owner-run VirtualBox boot is on record
  (DDR-906). No physical machine has been tested.
- **Disable Secure Boot.** The UEFI loader is unsigned.
- **No KPTI, no retpoline and no RSB refill.** On Intel CPUs without
  `RDCL_NO` (in practice pre-2018 Intel), Meltdown is **not mitigated**. Every
  boot log prints `[cpu] exposure: … meltdown=<yes|no> mds=<yes|no> kpti=0`,
  so the exposure is visible. **Deferred past v1 by operator decision**
  (PR #17 comment 5827611413, accepting DDR-1140 §2's recommendation). KPTI
  first needs IST and per-CPU entry stacks this kernel does not have, it would
  add two full TLB flushes to every syscall and interrupt, and its protection
  cannot be demonstrated under QEMU TCG.
- **No USB.** Input is PS/2, virtio-input and COM1. **US QWERTY only.**
- **No Wi-Fi, no Bluetooth, no audio, no IPv6, no TLS.**
- **Display:** virtio-gpu, or the UEFI GOP framebuffer (see above). **A BIOS
  boot without virtio-gpu has no desktop**: there is no VBE path, so it runs
  the serial shell only.
- **Entropy fails closed.** On hardware with neither RDSEED nor virtio-rng,
  the crypto consumers (vault, ACC) refuse to start. This is deliberate
  (DDR-816).

**Scope (operator decisions):**
- **Live boot only.** There is no installer.
- **Single-user.** There are no accounts and no login.
- **No telemetry** and no crash reporting. Nothing is sent anywhere.
- **Privacy mode stops caller-directed egress** (sockets and DNS). It does not
  stop DHCP lease renewal, because dropping the lease would take the interface
  down.
- **Deferred past v1:**
  - the signed-ledger key custody (DDR-1059);
  - the CAP_OCR, CAP_SCENE and CAP_NET_BROWSE agent capabilities;
  - compiler hardening (a stack protector and CFI), to a post-tag DDR;
  - KPTI, retpoline and RSB refill (DDR-1140 §2), to a post-tag series;
  - B#14/B#15 and Group G, pending respecification;
  - the Group F domain agents.
- **SFS snapshots** exist on disk but are reachable only from a kernel
  self-test. There is no user tooling for them.

### Legal

- `LICENSE` is proprietary; all rights are reserved.
- `EULA.txt`, `PRIVACY.txt` and `THIRD_PARTY_NOTICES.txt` ship on the ISO. The
  notices are lwIP's BSD-3-Clause and musl's MIT texts.
- The EULA and privacy policy are **drafts** and have not been reviewed by
  counsel.

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
