# DDR-994 — A detector for the wait that never ends (OPEN-1 route 1)

**Status:** DESIGN. Not implemented.
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

`yield()` appears at 26 sites. Four are reachable from ring 3, and **all four are
unbounded**:

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
`YIELD_STALL_TICKS` above the arm's hold — both must fail. A detector that
survives either mutation is decoration.

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
