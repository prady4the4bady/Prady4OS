# DDR-759 — SMP block-read data-integrity audit (M1 3/3)

**Status:** proposed (pre-code)
**Layer:** driver self-test + gate. M1 kernel hardening 3/3 (SMP audit).

## Problem

The M1-audit finding (SESSION_HANDOFF) logged a rare intermittent `-smp 4`
early-boot failure where the whole FS self-test suite went red (`/HELLO.TXT not
found`, …). The signature — *wrong data* rather than a hang — points at the
multi-in-flight block completion path (`virtio_blk.c`, DDR-BLK-1 slots +
DDR-714C3 AP-routed completions) delivering a completion to the wrong request
slot under cross-CPU load, so a reader gets another request's bytes. Code review
of `submit`/`complete` found no obvious defect (both are lock-covered), and there
is **no repro** (0/3 local). The existing `blkmq_proof` self-test only checks read
*success* (`bd->read(...) == 0`), never *content* — so a mis-routed completion
that returns valid-but-wrong data passes it. That is the audit gap.

## Decision

Add `smp_blk_integrity()` — a boot self-test (kmain, next to `blkmq_proof`) that
verifies *data*, not just success, under concurrent multi-CPU block reads:

1. **Reference (single-threaded, before workers):** read sectors 0..3 of blk0
   once and record a 32-bit checksum per sector. Sectors 0..3 are stable
   read-only boot-image content (MBR + stage2 + kernel), so the reference is
   deterministic per boot. No writes anywhere (no FS/image corruption).
2. **Concurrent workers:** spawn 4 kernel threads (so, under `-smp 4`, they
   distribute across CPUs and keep the 8 request slots busy). Worker *i* reads
   sector `i & 3` 64 times and checksums each read; a checksum ≠ the reference
   means a mis-routed completion delivered the wrong slot's data → sets an error
   bit.
3. **Verdict:** wait for all 4 workers (bounded by `g_ticks`), then print
   `[smp] blk integrity OK` (all reads matched their sector's reference) or
   `[smp] blk integrity FAIL`.

If the intermittent race is real, this makes it **reproducible** (wrong data is
detected, not silently tolerated) — a repro to root-cause from. If it stays green
under sustained concurrent load, that is strong evidence the completion routing
returns correct data and the earlier failure was infra/timing, not corruption.
Either way the audit gains a deterministic data-integrity witness the block path
lacked.

## Gate — `smoke-blk-integrity` (new; 94 → 95)

`QEMU_SMP=4`, `EXTRA_SENTINEL='[smp] blk integrity OK'`, `FORBIDDEN='blk integrity
FAIL'` via `boot_test.sh`. Runs the workers on 4 CPUs so completions route to APs
(the DDR-714C3 path).

## Non-goals

- Not a write-path integrity test (raw sector writes would corrupt the boot
  image / mounted FS); read-path routing is the finding's surface.
- Does not *fix* a race — none is currently identified. This is the audit's
  detection instrument; a fix follows only if it produces a repro.
- No change to `virtio_blk.c` submit/complete (already DDR-BLK-1/714C3 hardened).
