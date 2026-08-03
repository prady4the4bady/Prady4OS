# DDR-826 — a writable global in a probe is a runtime #PF, not a link error

**Status:** Implemented
**Date:** 2026-08-03
**Found by:** `smoke-ed25519` failing with no output at all.

## §Problem

`user/user.ld` links each freestanding probe as a **single R+X `PT_LOAD`**. That
is deliberate — W^X by construction (ADR-021). There is no writable segment.

So a `static fe C_D;` in `kernel/crypto/ed25519.c` has nowhere legal to live. It
lands in `.lbss` (large-code-model BSS, 264 bytes, flags `WA`). **lld does not
error.** It places the orphan section inside the read-only segment, the link
succeeds, the ELF looks fine, and the first store faults at runtime:

```
[trap] user #PF pid=29 rip=0x80000035D4 cr2=0x8000004F80 err=0x7
```

`err = 0x7` decodes as present | **write** | user — a user-mode write to a
present read-only page. `cr2` sits 0xB0 past the end of `.lrodata`, exactly
where the orphan was placed.

## §Why the symptom was so misleading

The probe **spawned** (`Ed25519 vector probe spawned` is in the serial log) and
then died on `curve_init()`'s first store, **before printing a single byte**.

So the gate reported:

```
[smoke] FAIL — required pattern 'PRADYOS_ED25519_VECTORS_OK' not found
```

which reads as *"the Ed25519 implementation is wrong"*. It was not. Every RFC
8032 vector passed on the host both before and after the fix. The failure was a
link-script violation in code that had nothing to do with the arithmetic.

Two false hypotheses were entertained and killed by measurement rather than
argument:

- **"too slow under TCG."** Killed by arithmetic: the probe costs ~3 ms on the
  host at `-O2`; even at `-O0` and a 100× TCG penalty that is ~6 s against a
  150 s window — a 25× margin. Plausible-sounding, and wrong.
- **"the harness swallowed the verdict."** Killed by re-running with
  `SERIAL_LOG` on a durable path: the verdict was real, and the serial log
  showed the probe spawning and producing nothing.

The decisive evidence was one line of the serial log plus `err=0x7`. It was only
available because `/tmp` had wiped the first log and the re-run used a path
under `build/`.

## §Fix, in two parts

**1. The bug.** `ed25519.c` now derives its curve constants into a caller-
provided `ed_ctx` on the **stack** — no file-scope mutable state at all:

```c
typedef struct { fe d, sqrtm1; ge B; } ed_ctx;
static void curve_init(ed_ctx *cx);
```

Cost: three `fe_invert` calls per public API call, against the 5+ scalar
multiplications each of those calls already performs. Negligible, and it buys
the removal of an entire failure class rather than a workaround for it.

Verified: `.data` and `.bss` in `ed25519_user.o` are now **size 0**, and every
RFC 8032 vector still passes on the host after the refactor.

**2. The class.** `make ci-probe-rodata-check` fails the build if any ELF has a
writable allocated section **and no writable `PT_LOAD` to hold it**.

The predicate is the important part. The first version simply flagged writable
sections, and immediately failed on `init`, `prism`, `compositor`,
`aether_daemon` and `cmusl` — which are **correct**: they link with
`$(USER_C_LD)`, which does provide a writable segment. Rather than maintain a
list of which program uses which linker script — that list would be the seventh
hand-maintained thing in this repo to drift silently — **the check asks the
binary**: does this ELF have somewhere legal to put writable data? If yes, skip.
If no, a writable section is a latent runtime fault.

Verified: 43 ELFs, none flagged.

## §Seventh instance

| # | Where | Silent failure | Fixed by |
|---|---|---|---|
| 1 | `ci.yml` gate list | 8 gates never ran | DDR-817 |
| 2 | Makefile `user/` sources | probes tested stale | DDR-822 |
| 3 | `user/` `_start` attribute | a new probe #GPs | DDR-823 |
| 4 | `syscall_register()` | NSI ≥ 80 vanished | DDR-823 |
| 5 | `check_global_forbidden()` | the `op=` line discarded | DDR-824 |
| 6 | crypto sources + Makefile | a build that never ran | DDR-825 |
| 7 | **writable global in a probe** | **link succeeds, first store faults** | **DDR-826** |

Six of the seven are *build-time silence*: the toolchain had the information and
did not surface it. The lesson generalises past "keep lists in sync" to
**anywhere a tool accepts something it cannot honour**. lld could see that a
writable section had no writable segment; it placed it anyway.

## §Operational note

`/tmp` was wiped mid-session and took the first gate log with it — the WSL
flakiness `boot_test.sh` already documents in its `SERIAL_LOG` comment. Gate
logs worth reading should be written under `build/gatelogs/`, not `/tmp`.
