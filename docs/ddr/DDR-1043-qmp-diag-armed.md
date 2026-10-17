# DDR-1043 — the silent-panic instrument was never armed, and would have been corrupted if it were

**Status:** FIXED (two defects) + measured both directions
**Trigger:** CI run 33627355396, shard 7, tip `c656037`, `smoke-smp`.
**Bears on:** OPEN-1, OPEN-12, DDR-1009 §2, DDR-1019.

---

## §1 — The artefact that cannot be diagnosed

```
SYSGETPID OK
SYSGETCWD OK
SYSLSEEK OK

*** NEXUS KERNEL PANIC ***
qemu-system-x86_64: terminating on signal 15 (timeout)
```

Banner, then **not one further byte** for the rest of the window. `boot_test.sh`
does not kill on a forbidden pattern mid-run — `early_exit_eligible` is 0 for any
gate declaring `FORBIDDEN_SENTINEL`, and the global list is checked after QEMU
exits — so nothing truncated this. The guest genuinely stopped printing.

**This exact signature is already on record.** DDR-1009 §2 captured it on
`81274f4`, shard 6, `smoke-msixap`: banner, then silence for ~100 s to the
timeout kill. That commit changed **Markdown only** — verified there by
`git diff --name-only` and a bit-for-bit rebuild — so the signature predates any
code change under discussion here, including this session's. DDR-1019 explains
the silence: the one-winner panic latch is claimed *before* the dump, so a winner
that cannot print silences every later panic and leaves only frozen CPUs.

Three local `smoke-smp` runs on the identical kernel (`5970a8506c66c115`) are
clean, which bounds nothing useful and is recorded as such.

**No fix to the panic path is attempted** — §NON-NEGOTIABLE 3 forbids one, and
there is no named mechanism because there is no register state. That absence is
what this DDR is about.

---

## §2 — DEFECT 1: the instrument exists and nothing ever armed it

`boot_test.sh` carries a DDR-887 watcher that, 5 s before the hard timeout and
while QEMU is still alive, dumps every vCPU's `info cpus` + `info registers -a`
through QMP. It is gated on `QEMU_QMP_DIAG`.

```
$ grep -rn 'QEMU_QMP_DIAG' Makefile tools/ .github/ | grep -v boot_test.sh
(nothing)
```

**Nothing in the repository has ever set it.** The one instrument that can answer
"the kernel printed a banner and then stopped — what was the CPU doing?" has been
switched off in CI, which is the *only* place that failure has ever been seen.

This is the DDR-986 / DDR-1024 shape (a diagnostic designed and then not reached)
and DDR-1010's rule stated plainly: an opt-in instrument is guaranteed OFF where
it matters.

**Armed** in `.github/workflows/ci.yml` on the shard step.

### §2.1 — Narrowed, so arming it is free

Firing on every full-window gate would append a register dump to ~39 healthy
gates' logs per shard run. The watcher now fires only when
`all_required_present` is **false** — a run 5 s from the kill that has not
satisfied its own sentinels is one that is already going to fail. Healthy runs
emit nothing.

Predicate reuse is deliberate: `all_required_present()` is the same function the
early-exit loop consults, so the two cannot drift about what "done" means.

---

## §3 — DEFECT 2: the dump was written into a file QEMU overwrites

The watcher appended to `$SERIAL_LOG`. **QEMU holds that same file open through
`-serial file:` and writes at its own tracked offset, without `O_APPEND`.** So
the guest's next serial output overwrites whatever the dump appended.

Measured, on the first run that ever armed it — the dump was there, and it was
wrecked:

```
[hb] t=3500 ... curpid=43
00000000246                      <- register text resuming MID-LINE
R12=0000008fffffffc8 R13=...
RIP=ffffffff80015076 RFL=00000006 ...
```

The header and the entire `--- info cpus ---` section are **gone**;
`grep -c 'QMP' <log>` returns **0** on a log that visibly contains register text.
Only the tail survived, because QEMU had stopped writing by then.

So the instrument would have produced a **corrupted artefact on the first failure
it was ever armed for** — and a partial register block with no header is exactly
the kind of thing §INV.23 warns about reading positionally.

**Fixed:** the dump goes to a sidecar, `${SERIAL_LOG}.qmpdump`, which no other
process touches.

### §3.1 — And it is printed, not merely written

A sidecar nobody prints is a sidecar nobody reads: every CI artefact that has
actually driven a diagnosis in this tree (DDR-1009 §2, DDR-1019) was read out of
a **job log**. `report_qmpdump` is called from all four failure paths — missing
kernel sentinel, missing required pattern, forbidden pattern, and the DDR-791
foreign-probe failure.

### §3.2 — And it is cleared with the capture it belongs to

`boot_test.sh` truncates `$SERIAL_LOG` at start; it did not truncate the sidecar.
A run reusing a `SERIAL_LOG` path would have printed the *previous* run's
registers as its own — a stale artefact presented as current, which is the same
class of error the whole instrument exists to remove. The sidecar is now removed
beside the truncation.

---

## §4 — Measured, both directions

| | run | dump written | dump printed |
|---|---|---|---|
| **positive** | `EXTRA_SENTINEL` that never appears, 40 s window | **yes**, intact: header + `info cpus` + `info registers -a` | **yes** |
| **negative** | `smoke-blk-timeout` (declares `FORBIDDEN_SENTINEL`, so full window), healthy | **no** | **no** (0 occurrences) |

The negative arm is the one that makes arming this globally defensible: a healthy
full-window gate produces nothing at all.

`make smoke-selftest` rc=0 — the boot-harness meta-test, which is what would
catch a change to `boot_test.sh` breaking early exit or the global-forbidden
scan.

---

## §5 — What this does and does not claim

- It **does not fix** the panic, or explain the silence. DDR-1019 named the
  mechanism for silence-after-banner and deferred the latch watchdog; that is
  unchanged.
- It **does not attribute** the shard-7 failure. The signature predates this
  session (DDR-1009 §2, on a Markdown-only commit), and 3/3 local runs on the
  identical kernel are clean, which is far too little to bound anything. Whether
  DDR-1041 aggravates it is not established and is not claimed either way.
- What it **does** is make the next occurrence carry register state instead of
  nothing. That is the difference between "OPEN-1 recurred" and a diagnosis.

## §6 — Residual

- The dump is taken 5 s before the kill, which for a panic that happened at
  t≈60 s in a 340 s window is ~280 s late. The CPUs are wherever they ended up,
  not where they faulted. Better than nothing — a halted CPU's `RIP` still names
  its halt site, which is precisely how DDR-1019 resolved the shard-9 `[apfreeze]`
  — but it is not a fault-time snapshot, and nothing here provides one.
- `QEMU_QMP_DIAG` is armed for the shard job only. The AETHER-gate job and the
  multi-arch boot job do not set it.
