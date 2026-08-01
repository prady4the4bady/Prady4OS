# DDR-816 — a kernel entropy source, or crypto refuses to start

**Status:** §Design — code follows DDR-812.
**Date:** 2026-08-01
**Prerequisite for:** DDR-813 (ACC), DDR-814 (AGS), DDR-815 (update
propagation). None of them can be implemented before this; DDR-813 §Blocker 1
found it.
**Numbering note:** the session brief pencilled DDR-816 for boot acceleration.
That is now **DDR-817**, because this is on the critical path for §S1–§S4 and
boot acceleration is not.

## §Problem

`grep -il "rdrand|rdseed|random|entropy|csprng|getrandom"` over `kernel/` matches
one file, and the match is the word "random" **inside a comment**. There is no
entropy in this kernel.

Every remaining §S feature is cryptographic, and each needs randomness in a place
where its absence is fatal rather than degrading:

* **X25519 private keys.** Predictable key ⇒ every "encrypted" channel is
  readable by anyone running the same kernel image. The system presents as
  encrypted and provides nothing.
* **ChaCha20-Poly1305 nonces.** Reuse under one key breaks **confidentiality and
  authenticity together** in this construction — not a weakening, a break.

## §Design — three sources, strict preference, and no silent fallback

### 1. virtio-rng (primary)

The virtio transport is already generic and complete —
`virtio_pci_attach` / `negotiate` / `setup_queue` / `driver_ok` / `notify`
(`virtio_pci.h`) serve blk, net, gpu and input today. A virtio-rng driver is a
new consumer of existing, gate-proven infrastructure rather than new transport
code, which is the cheapest correct option available and gives **real host
entropy** under QEMU and under any hypervisor this will run on.

`kernel/drivers/rng/virtio_rng.c`, dispatched from the same PCI scan in
`main.c:1945-1951` that already switches on `vendor_id == 0x1AF4`.

**Not asserted:** virtio-rng is virtio device type 4, but this DDR does **not**
state its PCI device/class ID from memory. The value is read from the virtio
specification at implementation time and confirmed empirically — if it is wrong
the device is simply never found, `rng_source()` reports `NONE`, and the gate
fails loudly. Same discipline as DDR-804's fw_cfg layout: the gate verifies the
constant rather than the document asserting it.

### 2. RDSEED / RDRAND (x86_64 only, secondary)

Gated on a CPUID feature check, never assumed. **Unavailable on riscv64 and
aarch64** (ADR-034 targets), so it can never be the only source — which is
exactly why it is second rather than first.

### 3. No third source — **fail closed**

If neither is present, `rng_bytes()` returns failure and **every crypto feature
refuses to initialise**. No jitter entropy, no timing fallback, no seeding from
`g_ticks`.

This is the central decision of this DDR, and it is deliberately the
conservative one. A jitter-entropy fallback is attractive because it always
"works" — and that is the problem: under TCG emulation, timing variance is
largely an artefact of the host scheduler and is neither measurable nor
trustworthy from inside the guest. An entropy source that silently degrades to
predictable output is **strictly worse than none**, because the absence is
detectable and the degradation is not. This is the same argument DDR-811 made
about a hand-rolled hash, and it applies with more force here: a wrong hash
fails a vector immediately, whereas a weak key produces ciphertext that looks
perfect and protects nothing.

A jitter source may be added later, behind its own DDR, with a health test that
must **fail loudly** rather than degrade.

## §API

```c
int      rng_init(void);                        /* probes sources; 0 if any found */
int      rng_bytes(void *out, uint32_t len);    /* 0 on success, <0 if no source  */
unsigned rng_source(void);                      /* RNG_NONE | RNG_VIRTIO | RNG_CPU */
```

`rng_source()` exists so the failure is **visible**: the boot log states which
source is in use, and the lockbox (DDR-812) records it. A system running without
entropy must say so on every boot, not discover it when a key is needed.

## §Invariants

1. `rng_bytes()` never returns success without having filled `out` from a real
   source. There is no path that returns pseudo-random or constant bytes.
2. No crypto feature initialises when `rng_source() == RNG_NONE`.
3. The active source is recorded in the boot log and in the metric lockbox.

## §Blast radius

* new `kernel/drivers/rng/virtio_rng.c` + `kernel/crypto/rng.h`
* `kernel/main.c` — one more `vendor_id == 0x1AF4` case, plus `rng_init()`
* `tools/qemu_runner/boot_test.sh` — the gate attaches `-device virtio-rng-pci`
* DDR-812's record gains the source field

## §Gate — `smoke-rng`

`FORBIDDEN_SENTINEL: PRADYOS_RNG_STUB`. Opt-in via `QEMU_PROBES=rng`.

Testing a random source is genuinely hard, and the honest position is that a gate
**cannot** prove randomness. What it can prove, and what these arms assert:

* **A** — no virtio-rng device attached and no CPU support → `rng_source()`
  reports `NONE`, `rng_bytes()` **fails**, and crypto init refuses. The failure
  is loud. This arm is the point of the fail-closed design.
* **B** — device present but the driver returns a fixed buffer (stubbed) → two
  successive 32-byte draws are **identical** → gate FAILS on
  `PRADYOS_RNG_STUB`. This is the arm that catches the most likely
  implementation error, which is a driver that appears to work and returns the
  same page every time.
* **C** — device present and working → two draws differ, `rng_source()` reports
  `VIRTIO`, and the boot log names it.

**Mechanism metric:** the probe draws twice and compares byte-for-byte, and
reports the source. Asserting "the call returned 0" would pass against a stub
that memsets zero — which is precisely the failure this exists to reject.

Distinct kernel SHAs per arm, printed — DDR-811's arm A passed with an identical
SHA and proved nothing until that was checked.
