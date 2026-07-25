# DDR-777 — `-smp 4` three-way discriminator probe (timer vs scheduler vs guard)

**Status:** proposed (pre-code). Passive instrumentation only — no behaviour change.
**Master-doc reference:** `docs/AETHER_MASTER_FEATURES.md` **Section B, item 3**.
Parents: `DDR-775` (evidence + two corrections), `DDR-776` (blk watchdog).

## Where the investigation actually stands

Two of my earlier conclusions were wrong and are corrected here before any code:

1. **"It timed out at the full 180 s, therefore it hung" — WRONG.**
   `tools/qemu_runner/boot_test.sh` always runs QEMU for the whole `TIMEOUT_S`
   window and *then* greps the serial. `qemu-system-x86_64: terminating on signal
   15 … (timeout)` appears in **passing** runs too (e.g. a local `smoke-input`
   PASS shows exactly that line before `[input] PASS`). So the timeout line is
   **not** evidence of a hang. The only hard evidence is *"sentinel absent"*.
2. **"The watchdog's silence proves no blk request was stuck" — WRONG** (already
   retracted in DDR-775): the watchdog runs on the timer path, so its silence is
   also consistent with the watchdog never running.

## What is actually established

In run 30163444702 (`smoke-smpuser`), the serial contains `[smp] ap preempt OK`
and `[smp] resched OK` but **neither** `[smp] user on AP OK` **nor**
`[smp] user on AP FAIL`.

`smpuser_proof()` (`kernel/main.c:659`) is:

```c
if (!g_smp_have_aps) return;                    /* silent early return */
smp_resched_all();
uint64_t dl = g_ticks + 200;
while (!g_user_on_ap && g_ticks < dl) yield();
kputs(g_user_on_ap ? "…OK\r\n" : "…FAIL\r\n");
```

Every neighbouring proof (`smpsched_proof`, `smppreempt_proof`,
`smpresched_proof`) carries the **same** `!g_smp_have_aps` guard. Since two of
them printed `OK`, **`g_smp_have_aps` was 1**, so `smpuser_proof` did *not* take
the silent early return — it entered the loop and never reached its `kputs`.

## Three surviving explanations — and nothing yet distinguishes them

- **(A) Stalled timer.** `g_ticks` stops advancing, so `g_ticks < dl` never
  becomes false and the loop never exits. Would also explain the DDR-776 watchdog
  never firing (same timer path).
- **(B) Scheduler starvation.** `g_ticks` keeps advancing but `yield()` never
  returns for this thread — the BSP's proof thread is descheduled and never
  re-run, so the loop body never completes. The rest of the system stays alive
  (consistent with user-thread output appearing after this point).
- **(C) Guard/ordering effect** — a variant where the flag state differs from what
  the OK/FAIL split assumes.

Guessing between these is precisely the mistake already made twice (the DDR-771
timeout bump, then the virtio-blk theory). This slice **only** builds the
discriminator.

## Decision — a three-way discriminator, entirely passive

1. **Heartbeat from the timer path** (`kernel/idt.c`, beside the existing
   `net_poll_tick()` / `virtio_blk_watchdog()` throttles): every 500 ticks (~5 s)
   print `[hb] t=<g_ticks>`. Over a 180 s window that is ≤36 lines — negligible
   serial volume, and no gate asserts on `hb`/heartbeat (verified by grepping
   `Makefile`, `tools/`, `.github/`).
2. **Entry marker in `smpuser_proof()`**: immediately after the AP guard, print
   `[smp] user-on-AP probe t=<g_ticks>`, and include the tick in the existing
   OK/FAIL line.

### How the next failing run is read

| Observation | Conclusion |
|---|---|
| no `probe t=` line | the silent early return fired — **APs were not up** (C) |
| `probe t=` present, `[hb]` **stops** | **(A) timer stalled** — LAPIC/timer path under `-smp 4` is the root cause; B#3 moves there, and *every* `g_ticks`-bounded wait in the tree is exposed (systemic S2) |
| `probe t=` present, `[hb]` **continues**, no OK/FAIL | **(B) scheduler starvation** — the proof thread never resumes from `yield()`; B#3 is a runqueue/AP-claim bug |

That is a decisive experiment in one CI run, whichever way it falls.

## Blast radius / gates

`kernel/idt.c` (one throttled call) and `kernel/main.c` (two prints). No blocking
behaviour, no locks, no scheduler change. Existing SMP gates
(`smoke-smp`, `smoke-smpuser`, `smoke-smpsched`, `smoke-surfdestroy`) must stay
green, proving the probe is inert. Gate count stays **106**. `[hb]` is deliberately
**not** made a sentinel — it is evidence to read, not a condition to assert.

## Architecture prerequisite checklist

- NSI/syscalls, TCB fields, PMM/VMM, capabilities, AETHER queue/audit,
  filesystem/root-mount, network policy, compositor/UI: **none**.
- **Scheduler hooks: none** — the heartbeat is a print from the existing timer
  call site, adding no scheduler behaviour.
- New gate: none.
- **Security invariants:** **S2** — bounded: one throttled print per 500 ticks and
  two one-shot prints; no loop, no allocation. **S6** — read-only; takes no lock
  (a print from the ISR touches only `g_ticks` and the console, adding no lock
  ordering to the subsystem under investigation) and cannot fail an I/O or panic.
  S1/S3–S5/S7/S8 not engaged. W^X, NX and capability contracts untouched.

## Explicitly NOT in this slice

No behavioural fix. Not the virtio-blk Hazard 1/2 S2 fixes (real defects, but
landing them now would risk being mistaken for the cure). Not another timeout bump.
