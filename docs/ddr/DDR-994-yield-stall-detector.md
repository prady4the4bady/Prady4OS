# DDR-994 — A detector for the wait that never ends (OPEN-1 route 1)

**Status:** IMPLEMENTED and mutation-checked. Gate green. See §8.
**Relates:** OPEN-1 (route 1), DDR-981 (the AP freeze), DDR-990 §12 (the three
signatures), DDR-955 (`sched_block_timeout`), DDR-968/986 (instrument-only DDRs).
**Gate:** `smoke-yieldstall` (new) + `[yieldstall]` in `GLOBAL_FORBIDDEN`.

---

## 1. Why the existing instruments cannot see this

DDR-990 §12 established that OPEN-1 is **at least three signatures**, not one:

| route | artefact | status |
|---|---|---|
| 1 | a **hang** in `sys_read`/`vfs_read`, **no panic at all** | **open — nothing detects it** |
| 2 | ring-0 `#PF`, 1/20 local | open (DDR-985) |
| 3 | ring-0 `#GP`, `RDI=0xDDDD…` in `tcp_new_port` | closed by DDR-987/990 |

The hammer closed route 3, which was never OPEN-1's own artefact. Route 1 is the
one the CI captures actually show, and **no panic-based detector can address it,
because a hang prints nothing.** Every instrument this repo has is keyed to
something being printed or something faulting:

- `GLOBAL_FORBIDDEN` matches text a failing probe emits. A hung probe emits none.
- The panic path (DDR-970/979) requires a fault. There is no fault.
- `[apfreeze]` (DDR-981) triggers on **"this cpu stopped taking interrupts"**.
  In route 1 the cpu is *fine* — it takes timer interrupts, `g_ticks` advances,
  other threads run. One thread waits forever. `ap_freeze_probe` is structurally
  blind to it, and reusing it here would be colour-matching two different
  failures because both are "something stopped".

What IS reusable from DDR-981 is the discipline, not the trigger: bounded shots,
print only on the failure path, name the sentinel so a recurrence cannot hide in
a flake.

## 2. The mechanism, named

`yield()` appears at 26 sites. Five are reachable from ring 3; one of those,
`sys_yield` (`syscall.c:155`), is a bare call that returns immediately and is not
a wait at all. The other **four are spin-waits, and all four are unbounded**:

| site | waits for | bounded? |
|---|---|---|
| `vfs.c:27` `mnt_lock` | `m->busy` — another **thread** holding the mount mutex | no |
| `sys_io.c:57` pipe write | ring full while a reader exists | no |
| `sys_io.c:268` pipe read | ring empty while a writer exists | no |
| `sys_io.c:293` console read | a **keystroke** | no — **and correctly so** |

```c
static void mnt_lock(struct vfs_mount *m) {
    while (__atomic_exchange_n(&m->busy, 1, __ATOMIC_ACQUIRE))
        yield();
}
```

DDR-981 fixed the **interrupt masking inside `yield()`** — which is why the cpu
no longer freezes, and why `[apfreeze]` stopped firing — but it never bounded the
spin itself. A holder that never releases (or that is itself stuck behind
something else) leaves the waiter spinning forever: **cpu busy, thread never
progresses, nothing printed, no panic.** That is route 1's signature exactly, and
`mnt_lock` sits directly on the `vfs_read` path where the captures hang.

**This is a hypothesis with a named mechanism, not a measurement.** It is not
claimed that `mnt_lock` *is* OPEN-1. It is claimed that route 1 is a wait that
never ends, that four such waits exist, that three of them are on the hang's own
call path, and that none of them can currently say so.

## 3. The one that must NOT be flagged

`sys_io.c:293` is a console read blocking for a keystroke. **That wait is
legitimately unbounded** — PRISM sits in it for the whole of every boot. A
blanket "waited too long" watchdog would fire in all 152 gates on the first run
and be switched off within the day, which is how a detector becomes noise and
then becomes deleted.

The discriminator is not duration. It is **what is being waited on**:

- sites 1, 2, 4 wait on state owned by **another thread in this system**. If that
  thread is not making progress, nobody will ever release it. A long wait here is
  a liveness bug.
- site 3 waits on **the outside world**. A long wait is Tuesday.

So the instrument is applied to the three intra-system waits and deliberately not
to the console read. That asymmetry is the design, and it is why this cannot be a
generic hook inside `yield()` itself — `yield()` does not know who called it.

## 4. The instrument

```c
/* Reports ONCE per wait, then keeps spinning. */
void yield_stall_note(const char *site, uint32_t spins, uint64_t ticks, int *done);
```

Each of the three sites keeps stack-local counters (no writable globals per
DDR-826) and calls the helper across the threshold:

```c
uint32_t n = 0; uint64_t t0 = g_ticks; int noted = 0;
while (<condition>) {
    yield();
    if (++n >= YIELD_STALL_SPINS && (g_ticks - t0) >= YIELD_STALL_TICKS)
        yield_stall_note("mnt_lock", n, g_ticks - t0, &noted);
}
```

**Both a spin count and a tick span are required**, and neither alone is enough:
a loaded cpu can legitimately spin a great many times inside one tick, and a
low-spin wait spanning seconds means the thread is barely being scheduled — a
different defect (DDR-989's territory). Reporting both gives the denominator
§NON-NEGOTIABLE 17 requires and tells the two apart on sight.

Sentinel:

```
[yieldstall] site=mnt_lock spins=<n> ticks=<d> pid=<p> cpu=<c>
```

Added to `GLOBAL_FORBIDDEN`, so a recurrence in **any** gate reddens that gate
and names itself — the DDR-981 lesson, where `smoke-smp` and `smoke-rqstress`
both measured 20/20 while the defect was live and the only evidence sat in a
serial log nobody asserted on.

## 5. It REPORTS. It does not repair.

§NON-NEGOTIABLE 3: no fix without a named mechanism from a real failing artefact.
There is a named mechanism here but **no captured artefact of it firing**, so
this DDR is not permitted to change locking semantics — no timeout that bails
with `-EIO`, no deadline that breaks the wait, no lock-ordering change. The spin
continues exactly as it does today; the only difference is that it says so.

That restraint is the point. Bailing out of `mnt_lock` on a deadline would
convert a hang into a silent `-EIO` on a live mount and would look like a fix
while destroying the evidence. Get the artefact first. The fix is a later DDR,
written against a real capture.

## 6. Gate, and how it avoids being vacuous

`smoke-yieldstall`, two arms:

- **A — the detector fires.** A gated kernel self-test drives `yield_stall_note`
  past its threshold synthetically and asserts the sentinel's exact shape,
  including that it prints **once** and not once per spin. Deterministic; no
  timing dependence.
- **B — the detector is WIRED.** Arm A passes even if no real call site ever
  calls the helper — that is DDR-988 §9's vacuous gate, and DDR-993 §5 is the
  freshest reminder that a mutation check only tests what the harness can
  express. Arm B therefore takes `mnt_lock` on a scratch mount and holds it past
  the threshold from a second thread, asserting `site=mnt_lock` specifically.

**Mutation check (required, both directions):** remove the `yield_stall_note`
call from `mnt_lock` — arm B must fail while arm A still passes. Raise
`YIELD_STALL_TICKS` above the arm's hold — arm B must fail. A detector that
survives either mutation is decoration.

> **CORRECTION, made after running them.** This paragraph originally predicted
> that raising the threshold would fail **both** arms. That was wrong about my
> own design, and the measurement said so: M2 fails arm B and **arm A still
> passes**. It cannot do otherwise — arm A calls `yield_stall_note` *directly*
> with literal values, so it never reaches a threshold test; the thresholds live
> at the three call sites, not in the reporter. Predicting a mutant's outcome is
> not the same as knowing it, which is the entire reason §6 requires running
> them. The corrected expectation is above, and §8 records what was measured.
>
> The result is better than the prediction: neither mutant kills arm A, and that
> asymmetry is the design working. Arm A is a unit test of the reporter, arm B
> is a wiring test of the call site. Two properties, two arms, and the mutants
> separate them cleanly.

## 7. What this does NOT claim

- **Not a fix for OPEN-1**, and not a claim that `mnt_lock` is OPEN-1. It makes
  route 1 *legible*. If the next occurrence prints no `[yieldstall]` line, the
  hypothesis is refuted and that is a real result — the same shape as DDR-985
  refuting its own Claim A.
- **Not complete coverage of unbounded waits.** Three of four ring-3 sites are
  instrumented by design (§3), and the 22 kernel-thread `yield()` sites in
  `main.c` are not touched at all — most are boot-time handshakes that cannot
  outlive their gate.
- **Not a substitute for `sched_block_timeout`.** That already exists (DDR-955,
  `sched.c:1434`, four callers) for waits that *should* have deadlines. These
  three spins are a different shape: they hold no lock and wait on a condition,
  and converting them to blocking waits is a real design change — a later DDR,
  not this one.

---

## 8. Results

Kernel hashes per R1. Four distinct hashes, so no run measured a stale binary.

| kernel | hash | arm A | arm B | gate |
|---|---|---|---|---|
| instrumented | `037ad1d6a8b046b1` | fires once | `site=mnt_lock` | **PASS** |
| M1 — call removed from `mnt_lock` | `652c09d4d7236655` | **passes** | absent | FAIL |
| M2 — `YIELD_STALL_TICKS` 500 -> 5000 | `7d37400fc8679ad0` | **passes** | absent | FAIL |

Both mutants leave arm A green and kill arm B, which is exactly the vacuity
§6 was written against: **an instrument that is never called still passes arm
A.** Had this gate shipped with arm A alone it would have been green on a
detector wired to nothing.

The live capture:

```
[yieldstall] site=selftest spins=12345 ticks=678 pid=0 cpu=0
[yieldstall] site=mnt_lock spins=127344 ticks=500 pid=0 cpu=0
PRADYOS_YIELDSTALL_RELEASED
PRADYOS_YIELDSTALL_WAITER_DONE
```

**The denominator (§NON-NEGOTIABLE 17): `mnt_lock` spins ≈ 127,344 / 500 ticks
≈ 255 spins per tick.** That is the first measurement this repo has of how fast
a `yield()` spin actually turns over, and it calibrates `YIELD_STALL_SPINS`:
20,000 spins is ~78 ticks of spinning, comfortably below the 500-tick arm, so
the tick threshold is the binding one at these sites. Both are kept — a site
that spins far slower would be caught by ticks alone, and one that spins far
faster inside a single tick must not trip on spins alone.

Restoring the fix reproduced `037ad1d6a8b046b1` byte-for-byte, so the revert is
verified rather than assumed (§NON-NEGOTIABLE 16).

`WAITER_DONE` is asserted for a reason: a detector that wedged the boot it is
diagnosing would be worse than none, so the arm proves the waiter is *released*
and exits.

## 9. Gate registration and one exemption

`smoke-yieldstall`, shard 9, `strict`, 120 s. Gate count 152 -> **153**.

`[yieldstall]` is in `GLOBAL_FORBIDDEN`, so a stall in **any** other gate reddens
it and names itself — the DDR-981 lesson, where the defect was live while
`smoke-smp` and `smoke-rqstress` both measured 20/20 and the only evidence sat
in a serial log nobody asserted on.

That forces one exemption: this gate emits the sentinel deliberately, twice, so
it would fail itself. It runs with `SKIP_GLOBAL_FORBIDDEN=1`. **Stated cost:**
that one 7-second boot loses global-list coverage. What remains is not nothing —
`boot_test.sh` still requires `NEXUS KERNEL OK` and both `EXTRA_SENTINEL`s, so a
panic or a wedge in this gate still fails it by absence. A narrower exemption
would need a per-gate allow-list that `boot_test.sh` does not have, which is not
worth inventing for one gate days from a deadline.

---

## 8. FIRST CAPTURE — the detector fired, and it corrects this DDR

CI run 32702146725, shard 9, `smoke-vault`, commit `70081d8`:

```
[sfs] umount ctx=0x07C2A000 ... live=0
[sfs] mount  ctx=0x07C2A000 ... live=1      (x many — the SFS self-tests)
[sfs] persistent root provisioned
[hb] t=3000 ... rqdepth=8 curpid=81
[yieldstall] site=mnt_lock spins=68981 ticks=500 pid=45 cpu=0
```

**§6's precommitment was: "if the next occurrence prints no `[yieldstall]` line
the hypothesis is refuted."** It printed, and it named `mnt_lock` — the site this
DDR singled out as sitting directly on the `vfs_read` path where OPEN-1's CI
captures hang. A thread waited more than five seconds on a mount mutex while the
SFS self-test cycled that same context.

### 8.1 What this does NOT establish — and the one-shot report is why

The opening line **cannot distinguish a deadlock from heavy legitimate
contention**, and that is exactly the question:

- `mnt_lock` is held across real block I/O.
- The self-test cycles mount/umount dozens of times in a row.
- CI runs under TCG, where each of those is slow.
- 68,981 spins over 500 ticks is ~138/tick against §6's measured turnover of
  ~255/tick — the waiter is being scheduled and spinning normally. That fits a
  deadlock and a slow-but-finite wait *equally well*.

A long wait that ENDS is not OPEN-1. Silence after the report could mean either
"still stuck" or "got in one tick later and said nothing", and the instrument as
shipped could not tell them apart — a gap in the design, not in the capture.

**Fix: the waiter now reports when it gets in.** `yield_stall_done()` emits
`[yieldstall] RESOLVED site=... spins=... ticks=...`, and `g_yieldstall_open`
counts reports with no matching resolution. An opened stall with **no RESOLVED
partner** is a hang; one with a partner was merely slow. Wired at all three
instrumented sites.

### 8.2 `[yieldstall]` REMOVED from GLOBAL_FORBIDDEN

It was added there when the detector shipped, on the assumption that any 5 s
yield-spin is pathological. This capture shows that assumption is not
established, and while it stands, the sentinel reddens gates on a signal not
shown to be fatal — inventing failures rather than detecting them. That is a
regression introduced by DDR-994 itself, and `smoke-vault` is the gate it broke.

Re-add it only once a capture shows an OPENED stall with no RESOLVED partner.
The rationale is recorded in `boot_test.sh` beside the `[apfreeze]` note so the
next session does not "restore" it as an oversight.

### 8.3 A fifth unbounded spin, not in §6's count

`switch_wait_offcpu` (`sched.c:479`) is a fifth unbounded yield-free spin and is
not one of the four this DDR instrumented. It is not ring-3 reachable, so it is
outside OPEN-1 route 1's scope — but §6's inventory said "four", and four is
wrong. Found by hanging a boot on it (DDR-996 §8.3).

### 8.4 A SECOND mnt_lock capture, and a THIRD unrelated signature

**`mnt_lock`, capture 2** — run 32705586225, shard 2, `smoke-acc`, `b5ad0e2`:
`site=mnt_lock spins=52491 ticks=500 pid=43 cpu=0`, immediately after
`[sfs] freelist persist OK`. Same shape as capture 1 (shard 9, `smoke-vault`,
pid 45, 68,981 spins): both land exactly on the 500-tick threshold, both follow
heavy SFS activity, both ~105-138 spins/tick. Two gates, two pids, one site —
this is reproducible, not a one-off, which is why §8.1's RESOLVED line matters:
the next CI run answers "slow or stuck" for both.

**A third, DIFFERENT signature — recorded, NOT diagnosed.** Two gates failed with
no `[yieldstall]` line at all, so `mnt_lock` is not involved:

| gate | shard | `preempt` | note |
|---|---|---|---|
| `smoke-invariants` | 8 | 1212, frozen t=7000..11500 | `rqdepth=14`, INIT/PRISM alternating |
| `smoke-poweroff` | 5 | 1509, frozen t=10500..11500 | `rqdepth=5`, `curpid` varies (18/41/40) |

The shared feature is a **frozen `preempt` counter while `curpid` keeps
changing** — threads are still switching voluntarily, but timer-driven
preemption has stopped. That is suggestive and it is not a diagnosis: two
heartbeat samples cannot distinguish "preemption stopped" from "nothing needed
preempting". Both gates pass locally on the tip.

No named mechanism, therefore no fix — §NON-NEGOTIABLE 3. Written down so the
next occurrence is recognised as the third recurrence rather than investigated
from scratch, and so it is not mistaken for the `mnt_lock` stall above (which it
demonstrably is not — different gates, no sentinel, different counters).
