# DDR-1014 — `sched_unblock` consumed its one kick on a CPU that cannot be kicked

**Status:** TWO FIXES. One is a real scheduler defect found by reading; the other
is the false CI failure that led me to it, and it corrects DDR-1004 §6.1, which
predicted a residual and named the wrong one.

---

## 1. The artefact

CI run on `72a474a`, shard 5, **`smoke-percpu-sched`**, failed at gate 3 of 14:

```
[smoke]   [smp] resched FAIL ipis=0 ran=1 idle=1
```

`72a474a` is **one Markdown file** over `483e853` (`git diff --name-only`), so it
is the identical kernel — this is not the diff.

Note which gate went red. `smoke-percpu-sched` does not own that assertion; its
own `FORBIDDEN_SENTINEL` is `current FAIL` / `gs FAIL` / `percpu FAIL`. It failed
because **`resched FAIL` is a `GLOBAL_FORBIDDEN` entry**, i.e. DDR-791's "forbidden
in every gate, not only the one that owns it" doing its job.

## 2. The kernel defect: a kick spent on a CPU that never receives one

`smp_resched_one` (`smp.c:161`) declines for the BSP, and said nothing about it:

```c
void smp_resched_one(uint32_t cpu_idx) {
    struct percpu *pc = percpu_get(cpu_idx);
    if (pc && pc->present && !pc->is_bsp) {        /* <-- BSP: silently skipped */
        __atomic_add_fetch(&g_resched_ipis, 1, __ATOMIC_RELAXED);
        lapic_send_ipi(pc->apic_id, LAPIC_WAKE_VECTOR);
    }
}
```

`sched_unblock` (`sched.c:1825`) broke out of its search **on the call**, not on a
delivery:

```c
for (int c = 0; c < PERCPU_MAX; c++) {
    struct percpu *o = percpu_get((uint32_t)c);
    if (c != self && o && o->present && o->idle) {
        smp_resched_one((uint32_t)c);
        break;                                     /* <-- stops looking */
    }
}
```

**So an unblock running on an AP that finds the BSP idle first calls
`smp_resched_one(0)`, sends nothing, and stops.** Any idle AP later in the list is
never kicked, and the freshly-READY thread waits a full timer tick — precisely
the latency rq-3 exists to remove.

**This is reachable in ordinary operation, not just in the boot proof.** There are
62 `sched_unblock` call sites outside `sched.c`, and `virtio_blk.c:102/198` are on
the completion path, which runs in **MSI-X interrupt context on whichever CPU the
vector is routed to** — routinely an AP.

It is a *latency* defect, not a correctness one: the comment three lines above
says "the timer remains the backstop", and it does. Nothing hangs. The
optimisation simply does nothing, silently, in a common case.

**Fix:** `smp_resched_one` returns 1 on a delivered IPI and 0 otherwise, and the
loop breaks only on 1. The BSP rule stays in one place — the caller does not
learn it, it just stops treating a call as a delivery.

## 3. The probe defect: a predicate that paraphrased the kernel instead of matching it

DDR-1004's proof decides an IPI is owed by scanning for an idle non-self CPU:

```c
if (c != self_idx && o && o->present && o->idle) { idle_seen = 1; break; }
```

The kernel's own predicate has a fourth clause the proof did not: **the BSP is
never kicked.** So when the BSP is the idle CPU, the proof expects an IPI that
the kernel will never send, and prints `ipis=0 ran=1 idle=1` on a correct system.

That is a **deterministic** false failure, needing no timing at all. Fixed by
adding `&& !o->is_bsp`, so the two loops ask the same question.

### 3.1 This corrects DDR-1004 §6.1

DDR-1004 §6.1 said:

> "`idle_seen` is sampled just before `sched_unblock`, and a CPU can leave idle in
> between. The window is far narrower than the old unconditional assertion but is
> not zero, so **a FAIL with `idle=1` is strong evidence, not proof.**"

The reservation was right and the mechanism was wrong. That timing window is real
but narrow; **this** one is a predicate mismatch that fires whenever the BSP
happens to be the idle CPU. DDR-1004 added the `idle=` field precisely so a future
FAIL could be read — it worked, and what it read out was not what §6.1 expected.

A proof whose predicate paraphrases the kernel's is not testing the kernel; it is
testing the paraphrase. That is the general lesson, and it is the same shape as
DDR-1012's first gate (comparing two gradient rows rather than one pixel before
and after) and DDR-1013's probe constant (a wire format hand-copied and drifted).
Three instances in one session of *a test that describes the system in its own
words instead of asking the system*.

## 4. What is NOT claimed

- **No artefact of §2 firing.** The consumed kick is found by reading. Its symptom
  is a 10 ms latency, and no gate measures wake latency, so it would not have
  announced itself. The CI failure in §1 is the *probe* defect (§3), which on the
  boot path runs on the BSP where §2 cannot trigger.
  §NON-NEGOTIABLE 3 asks for a named mechanism from a real failing artefact: §3
  has both. §2 has a named mechanism and a code path, and is fixed on that basis
  because it is a two-line change that makes the caller stop asserting something
  the callee never promised — not because a capture demanded it. Recorded plainly
  rather than dressed up as artefact-driven.
- **Not a cause of OPEN-2.** Different subsystem, different symptom; a missed
  latency optimisation does not freeze a CPU with `if=0`.
- **No claim that this ends `resched FAIL` in CI.** The narrow timing window
  DDR-1004 §6.1 named is still there and is still not proof-grade.


---

## 5. MEASURED

Kernel **`c9740c9a61332f37`**, `-Werror` clean, 1,102,218 B.

### 5.1 The proof still asserts — three `OK`, not `SKIP`

This is the load-bearing check, and it is the one DDR-1004 §6 established: adding
`!o->is_bsp` narrows `idle_seen`, and a narrowing that made it *always* false
would turn every run into `SKIP` and silently retire the assertion.

```
run 1: [smp] resched OK
run 2: [smp] resched OK
run 3: [smp] resched OK
```

Three consecutive `OK` on `smoke-rqstress` with the capture pinned. So an idle
non-BSP CPU is still normally visible, the IPI is still required, and it is still
observed. The predicate was aligned with the kernel's, not switched off.

### 5.2 Gates

| gate | result |
|---|---|
| `smoke-percpu-sched` (the one that went red in CI) | PASS |
| `smoke-rqstress` | PASS |
| `smoke-resched` | PASS |
| `smoke-smppreempt` | PASS |
| `smoke-crosswake` | PASS |
| `smoke-smpsched` | PASS |
| `smoke-blk-integrity` | PASS |
| `smoke-blkmq` | PASS |
| `smoke-shell` | PASS |

`ci-shard-check` OK (158 / 10 / 7); `ci-probe-rodata-check` OK (61 ELFs).

The SMP and block gates are listed because §2 changes `sched_unblock`, a
scheduler hot path reached from `virtio_blk`'s completion handler in interrupt
context — the gates most likely to notice a mistake there.

### 5.3 Still not measured

**The latency §2 recovers.** No gate times a wake, so the fix cannot be shown to
improve anything; it can only be shown not to break anything, which is what §5.2
does. A gate that measured enqueue-to-run latency across an AP unblock would make
§2 demonstrable rather than merely correct-by-reading, and it does not exist.


---

## 6. A SECOND artefact, on the pre-fix kernel — and a possible consolidation

Forty minutes after §1's capture, the identical line appeared again:

```
[smoke]   [smp] resched FAIL ipis=0 ran=1 idle=1
shard 5: FAILED at smoke-smpuser after 1 of 14 gates
```

CI on **`6a6c07e`** — docs-only, so kernel `ba6ac01fe015b2a4`, i.e. **before** the
fix in §2/§3 (which is `c9740c9a61332f37`).

Two things worth keeping.

**It reddened a different gate.** §1's occurrence took `smoke-percpu-sched` at
gate 3 of 14; this one took `smoke-smpuser` at gate 1 of 14. Neither owns the
assertion — both were caught by `resched FAIL` being a `GLOBAL_FORBIDDEN` entry,
so the failure surfaces in whichever gate on that shard happens to run first.
**The gate name in a `resched FAIL` report carries no information about the
defect**, which is worth knowing before anyone tries to correlate one.

### 6.1 This may fold one of DDR-1009's four signatures into this defect

DDR-1009 §1 recorded four distinct CI signatures, the first being *"shard 5
`smoke-smpuser` (timeout, gate 1/14)"* on `81274f4`. **That is the same shard, the
same gate, and the same position as the occurrence above.**

If it was also a `resched FAIL`, then DDR-1009's four signatures are three, and
one of them is now fixed.

**This is not claimed.** The `81274f4` job log was truncated before the guest
lines and only its tail survives, so the actual sentinel is unknown; and a
`GLOBAL_FORBIDDEN` hit and a genuine timeout look identical at the tail (both end
in `terminating on signal 15 … (timeout)`, because `boot_test.sh` kills QEMU on a
forbidden hit too). Matching on shard-plus-gate-plus-position alone is exactly the
colour-matching DDR-975 §7 and DDR-1010 §2 each had to retract.

**What would settle it:** `resched FAIL` on the fixed kernel should stop. If
shard 5 goes quiet across the next several suite-runs on `c9740c9a61332f37` or
later, the consolidation is likely; if `smoke-smpuser` keeps failing there, it was
a different thing and DDR-1009's signature #1 stands on its own.
### 6.2 MEASURED — five suite-runs on the fixed kernel, shard 5 quiet

Checked 2026-08-30 ~17:30 UTC, both events on every SHA at or after the fix:

| tip | event | jobs | result |
|---|---|---|---|
| `792f162` | push | 15/15 | green |
| `792f162` | pull_request | 15/15 | green |
| `438afdb` | push | 15/15 | green |
| `438afdb` | pull_request | 15/15 | green |
| `6e5427a` | push | 15/15 | green |

**These five pool onto ONE kernel binary.** `git diff --name-only` shows
`438afdb` and `6e5427a` change **only** `docs/ddr/*.md` relative to `792f162`, so
the DDR-1009 §8.3 pooling discipline permits it — this is the same argument that
turned three SHAs into twelve suite-runs there, applied honestly in the other
direction. Kernel `c9740c9a61332f37`.

**Zero `resched FAIL`, and shard 5 green in all five.** On the pre-fix kernel
`ba6ac01fe015b2a4` it fired twice within 40 minutes.

**What this does and does not establish.** It answers the question §6.1 posed —
shard 5 went quiet — and it is the outcome the consolidation hypothesis
predicts. It does **not** establish a rate. Against DDR-1009's measured 25%
per-suite failure on the *previous* binary, five clean runs would occur by luck
with probability `0.75^5 = 0.24`; and that 25% pooled four different signatures,
only one of which is this defect, so it is not even the right prior. **The
consolidation of DDR-1009 signature #1 into this defect remains LIKELY, not
shown** — settling it needs the `81274f4` sentinel, which is lost, or a much
longer quiet run. Do not upgrade the wording without one.

**What would reopen it:** a single `[smp] resched FAIL` line on
`c9740c9a61332f37` or later. DDR-1004 §6.1's narrow timing window is then the
remaining candidate, and it is **not** proof-grade — instrument before changing
anything.
