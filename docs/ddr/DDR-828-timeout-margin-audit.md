# DDR-828 — CI audit: `smoke-syscallfuzz` was a timeout, not a defect

**Status:** Implemented
**Date:** 2026-08-04
**Trigger:** a full audit of every red CI run in the previous 48 hours.

## §The audit

Eight runs on `dev/phase1` failed between 14:05 and 17:31 on 2026-08-03. Every
one was opened to its failing job, step and error string. They are **not** one
defect:

| gate | runs | verdict |
|---|---|---|
| `smoke-ed25519` | `c68dc24`, `5e551d8`, `df248af` ×3 | **EXPECTED.** DDR-826 — a writable global in an R+X-only probe. Deliberately left registered and red rather than hidden. Stopped failing at `ad8a3a4`, which is the fix. |
| **`smoke-syscallfuzz`** | **7 of the 8** | **THIS DDR.** A timeout, not a probe failure — see below. |
| `smoke-resched` | `ad8a3a4` only | single occurrence, OPEN-2 class. Triaged, not fixed. |
| `smoke-blkmq-trace` | `df248af` only | single occurrence, OPEN-2 class. Triaged, not fixed. |

The current tip `fd876cd` is **green twice** (`30878361148`, `30879247169`).

## §What `smoke-syscallfuzz` actually was

```
17:54:56  ##[group][14/18] make smoke-syscallfuzz
17:54:56  TIMEOUT_S=60 EXTRA_SENTINEL="$(printf 'PRADYOS_FUZZ_OK')"
17:55:56  [smoke] FAIL — required pattern 'PRADYOS_FUZZ_OK' not found.
```

Exactly 60 seconds. **The gate hit its window.** `PRADYOS_NET_FUZZ_OK` appears
in the same log, so the kernel was alive and progressing — nothing had crashed.

Measured locally: the same gate completes in **25 s**. That is a 2.4× margin on
this workstation and evidently not enough on a shared CI runner, especially as
the image has grown (every new probe ELF is `incbin`'d into it, and ed25519 +
aead added ~40 KB of probes plus their boot-time spawns).

**Matching by identity mattered here.** "Required pattern not found" is the same
string DDR-826 produced, and DDR-826 *was* a real defect. Reading the timestamps
rather than the message is what separated them: 60 s exactly is a window, not a
fault.

## §Fix — the class, per DDR-788's own precedent

DDR-788 already established the principle: **a gate with no
`FORBIDDEN_SENTINEL` is DDR-785 early-exit eligible, so a larger window is FREE
on success** — it stops the moment its sentinel appears. DDR-788 raised the
gates it covered to 120 s and recorded that it "retired the DDR-783
timeout-margin flake class".

**Seventeen gates were still at 60 s.** DDR-788 did not reach them, and the
flake class was therefore not retired — it was dormant until the image grew.

Eleven of those are early-exit eligible and are now **60 s → 120 s**:

```
smoke-fs           smoke-user         smoke-init        smoke-syscallfuzz
smoke-net-lo       smoke-net-fuzz     smoke-aether-queue smoke-gpu
smoke-apic         smoke-serialflood  smoke-aether-sec
```

**Five are deliberately left at 60 s** — `smoke-kill`, `smoke-fpu`,
`smoke-net-tcp-lo`, `smoke-fs-budget`, `smoke-nvme`. They declare a
`FORBIDDEN_SENTINEL`, so DDR-785 early exit does **not** apply and they burn
their whole window every run. Raising them would cost 5 × 60 s of real
wall-clock per CI run for gates that are not currently failing. If one of them
starts flaking, that is a different trade and should be made then, with the cost
stated.

`smoke-selftest` also stays at 60 s: it is host-only (a stub QEMU replaying
scripted logs), so its timing has nothing to do with image size.

**Cost on success: zero.** Early exit means a passing gate stops at its
sentinel regardless of the window. The only thing that changes is how long a
genuinely hung gate takes to give up.

## §What this does not fix

`smoke-resched` and `smoke-blkmq-trace` each failed **once**. One occurrence is
not a pattern, and both passed on every other run including the current tip.
They are triaged into the existing OPEN-2 intermittent bucket, **not** claimed
as fixed. If either recurs, DDR-824's context dump will now show what the probe
actually reported.

## §Rule earned

**A gate's timeout is a claim about how long the system takes, and it goes stale
as the system grows.** Seventeen gates carried a 60 s figure chosen when the
image was smaller. Nothing re-examined it, and the failure looked like a code
defect rather than a stale constant — twice, in the same session, with the same
error string.

When a gate fails on "pattern not found", **check the elapsed time against the
window before reading the code.**
