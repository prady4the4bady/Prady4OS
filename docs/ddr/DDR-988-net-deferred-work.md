# DDR-988 — lwIP deferred work: drain on release, never block an ISR

**Status:** implemented
**Supersedes:** DDR-987 §11 (partially — §11's trylock is kept, its liveness
argument and its cadence figure are both replaced here)
**Relates:** DDR-987 (the lwIP core lock), OPEN-1, OPEN-12

---

## 1. Why this exists

DDR-987 §11 stopped `net_poll_tick()` blocking on `g_net_lock` inside the timer
ISR, which was the direct cause of the frozen-tick regression merged in PR #12.
An adversarial review of PR #13 — which I requested, naming timer starvation as
the risk I most wanted attacked — falsified the argument §11 rested on. Three of
its four findings are correct and are fixed here. The fourth is a factual error
in my own documentation, corrected in §6.

I record this as a falsification rather than a refinement because §11 shipped a
claim I had not measured, and the review measured it.

## 2. What §11 got wrong

§11 justified dropping a contended tick with:

> "If another cpu is already inside lwIP, the timer work it would do is being
> done; skipping this tick loses nothing."

**This is false.** Holding `g_net_lock` does not imply running timers. Of the
lock's holders, exactly two call `sys_check_timeouts()` (`net_pump_locked`,
`net_timeouts_locked`). The rest do not:

| Holder | Runs timers? |
|---|---|
| `psock_read` (drains up to 2048 B) | no |
| `psock_write` | no |
| `psock_close` / `psock_connect` / `psock_state` | no |
| `pradyos_netif_rx_isr` | no |
| `net_loopback_test` setup/teardown | no |

So a tick skipped because `psock_read` held the lock loses that period's timer
work outright. With one trylock attempt per timer event and no retry, a
correlated sequence of holders starves TCP retransmit, delayed-ACK and
time-wait indefinitely. Nothing in §11 bounded that.

## 3. The second hole: RX preserved the freeze path

§11 removed blocking from the timer ISR and explicitly kept it in RX:

> "RX keeps the blocking acquire — dropping a received frame is a real loss."

That reasoning weighed the wrong cost. `pradyos_netif_rx_isr` is called from
`net_complete()` (`virtio_net.c:67-82`) — from an interrupt handler, **up to 64
times in a single IRQ**, and its critical section is not a bounded `pbuf_alloc`
as §11 asserted: it is `netif.input` → `ethernet_input` → `ip_input` →
`tcp_input`, the whole lwIP receive path, once per frame. A CPU that blocks
there spins with interrupts off and cannot take its own next timer interrupt.
That is precisely the mechanism §11 set out to remove, left intact on the
higher-frequency path.

## 4. Design — deferred work, drained by the releaser

The fix is to make **release** the service point rather than a future acquire.

Two pieces of deferred state:

- `g_net_timer_pending` — a coalesced flag. `net_poll_tick` sets it when it
  cannot get the lock. Consumed with `__atomic_exchange_n(..., 0, ACQ_REL)` so
  a set racing with a drain is never lost.
- `g_net_rxq[16]` — a bounded ring of raw Ethernet frames (`len` + ≤1514 B),
  guarded by its own `g_net_rxq_lock`. The RX ISR **never touches
  `g_net_lock`**; it copies the frame into the ring and returns.

Every release of `g_net_lock` goes through:

```c
static void net_unlock(uint64_t fl) {   /* drain, THEN release */
    net_drain_locked();
    spin_unlock_irqrestore(&g_net_lock, fl);
}
```

`net_drain_locked()` injects every queued frame via `pradyos_netif_rx()` and, if
the timer flag was set, runs `sys_check_timeouts()` + `netif_poll_all()` — all
while still holding the lock, so it is ordinary locked lwIP work.

### 4.1 Liveness argument (the thing §11 lacked)

Every acquire of `g_net_lock` is followed by a release in bounded time: no
holder yields, sleeps or blocks, and §8 already removed the two multi-hundred-
round loops that used to hold it. Therefore for deferred work pending at time T:

- **Lock contended at T** — the holder releases at T+ε and drains at that
  release. Serviced in ε.
- **Lock free at T** — `net_poll_tick` acquires within one poll period and
  drains directly.

Either way the work is serviced in bounded time, and the bound does not depend
on a future trylock happening to win. That is the guarantee §11 did not have.

**CORRECTION (§12.1): the clause "no holder yields, sleeps or blocks" above is
FALSE** — holders block on `g_console_lock` and `g_heap_lock`. The conclusion
survives but only conditionally; read §12.2 before relying on this paragraph.

**The residual window, stated honestly:** the RX ISR enqueues without
`g_net_lock`, so a frame landing between a holder's drain and its unlock is not
seen by that drain. It waits for the next acquire — at most one poll period
(§6). This is bounded and it is not a starvation: it cannot repeat indefinitely,
because the next acquire drains unconditionally.

### 4.2 Bounds

- Drain is capped at `NET_DRAIN_ROUNDS = 2`. RX frames originate from the NIC,
  not from lwIP, so a drain cannot generate its own work; the cap is belt-and-
  braces against an unforeseen feedback path, not load-bearing.
- Ring full, or `g_net_rxq_lock` contended (two RX ISRs on two CPUs at once):
  the frame is **dropped and counted**, never waited on. A drop under double
  contention on a bounded `memcpy` is rare and TCP retransmits; a blocked ISR
  freezes a CPU. That is the trade, made explicitly this time.

### 4.3 Lock order

**INCOMPLETE AS FIRST WRITTEN — see §12.1 for the corrected order.** This said
`g_net_lock → g_net_rxq_lock → g_heap_lock`, omitting that a holder also reaches
`g_console_lock` (via `kputs` in the lwIP echo callbacks) and `g_pmm_lock` (via
`kmalloc`). The RX ISR part is accurate: it takes only `g_net_rxq_lock`, held
across a `memcpy` and two index updates with no calls out. There is no cycle,
but the order to reason from is §12.1's, not this one.

## 5. Counters (R17 — a denominator, and a reader)

`g_net_tick_skipped` was incremented with a plain `++` from ISRs on multiple
CPUs — `cli` serializes the local CPU only, so increments could be lost — and
**nothing read it**, which makes it not a measurement. All three counters are
now `__atomic_add_fetch(..., __ATOMIC_RELAXED)` and are printed by the existing
`[hb]` heartbeat (`idt.c`, every ~5 s, evidence-only, no gate asserts on it)
alongside `thre_drops` / `rx_drops`:

```text
[hb] t=<n> ... net_skip=<n> net_defer=<n> net_rxdrop=<n>
```

- `net_skip` — timer events that deferred instead of polling inline.
- `net_defer` — RX frames queued instead of injected inline.
- `net_rxdrop` — RX frames lost (ring full or rxq lock contended). **This is the
  one that must stay at or near 0**; a rising `net_rxdrop` means the ring is
  undersized for the offered load.

`net_skip` and `net_defer` are expected to be non-zero and healthy: they are the
cost side of never blocking an ISR, not faults.

## 6. Correction: the poll cadence is ~10 Hz, not 100 Hz

DDR-987 §11 and its code comment both state that `net_poll_tick()` runs "at
100 Hz" and that a skipped tick costs "at most 10 ms of timer latency".

**Both are wrong.** `kernel/idt.c:266` calls it only when `(now % 10u) == 0`,
and the PIT is 100 Hz — so lwIP polling is **~10 Hz, one opportunity per
~100 ms**. A single lost opportunity therefore costs **≥100 ms**, an order of
magnitude more than claimed. The kernel-side comment at `idt.c:266` was correct
all along ("~every 100 ms"); the error is mine, in the port and in the DDR.

This matters beyond bookkeeping: it makes each skipped opportunity ten times as
expensive, which is what turns §2's starvation from a latency nit into a real
defect. Corrected in the code comment and here.

## 7. Closes DDR-987 §6

§6 left the `ps_recv` / `ps_err` / `ps_connected` callbacks unaudited for
re-entrancy. Audited: none of the three touches `g_net_lock`. They are invoked
by lwIP core from inside the locked region and are safe there. §6 is closed —
and the audit was a precondition for this DDR, since `net_drain_locked()` calls
`pradyos_netif_rx()` (and thus those callbacks) with the lock held.

## 8. What this does NOT establish

- It does not prove the DDR-987 use-after-free is gone. Per DDR-987 §5 the gate
  suite cannot: the defect's base rate is ~1/20 in one gate. The two-CPU
  `connect`/`close` hammer probe remains the only positive proof and is still
  unwritten.
- It does not measure `net_rxdrop` under real load. CI offers almost no network
  traffic, so a green run is weak evidence about ring sizing. The counter exists
  so that a future load test has a denominator to read.

## 9. The counter immediately caught a vacuous gate

`net_rxdrop` earned its place on the first boot it was readable. The `[hb]`
heartbeat printed:

```text
[hb] t=3500 ... net_skip=0 net_defer=137 net_rxdrop=613 ...
```

613 dropped frames is not the "~0" §5 says healthy, and the value was frozen
from early boot onward — a burst, not a trickle. The producer was not the NIC.
It was `net_fuzz_test()`, a **synchronous kernel self-test**, which injected its
512 malformed frames and 256 SYN frames through `pradyos_netif_rx_isr()`.

Once §3 turned that wrapper into a 16-slot enqueue, 613 of the 768 frames
(750 after the `len < 14` filter) were dropped before reaching lwIP. The
malformed-frame and SYN-flood hardening the gate exists to exercise was
therefore *not being exercised* — and `smoke-net-fuzz` **still passed**, because
its pass criterion is survival, and dropping a frame is a very reliable way to
survive it.

This is the DDR-973 §6 failure mode again: a gate that is green because it
stopped testing anything. It is worth naming how close it came to shipping —
the gate was green, the build was warning-clean, and nothing but a counter with
a reader would have shown it. §5's insistence on a reader was not bookkeeping.

**Fix:** a self-test is not an interrupt. `net_inject_locked()` takes
`g_net_lock` and calls the raw injector synchronously, per frame (never held
across the loop, per §8). The ISR wrapper is now reached only from
`virtio_net_set_rx()`, which is its only legitimate caller.

**Expected after the fix:** `net_rxdrop=0` and `net_defer=0` on a CI boot, since
CI offers no real inbound traffic and nothing else uses the ISR path. Any
non-zero value there is then a genuine signal rather than self-inflicted noise —
which is what makes the counter useful for the load testing §8 says is still
owed.

### 9.1 Measured after the fix

Same gate, post-fix kernel `4ef7bd008c4c969d`, `KEEP_SERIAL=1`:

```text
net_skip=0 net_defer=2 net_rxdrop=0
```

**`net_rxdrop`: 613 -> 0.** All 750 fuzz frames now reach lwIP, so
`smoke-net-fuzz` tests what it claims to again.

One correction to §9's prediction, which said to expect `net_defer=0`: it is
**2**. Those are genuine inbound frames arriving through the virtio-net IRQ —
the deferred path doing exactly its job — not self-inflicted noise. The
prediction was slightly wrong and the observed value is the correct one; what
matters is that `net_rxdrop` is 0, and it is.

`net_skip=0` says the timer never once found `g_net_lock` contended in this
boot. That is consistent with the fix but is **not** evidence for it: with no
network load there is little to contend over. Per §8 this remains unmeasured
under load.

## 10. Socket-handle defects found in the same review

The PR #13 review also raised four findings against the DDR-987 §10 handle work.
Three were real defects I introduced; one was a documentation contradiction.

### 10.1 A stale write handle returned `-EIO` instead of `-EBADF`

`psock_write()` ended `return (e == ERR_OK) ? len : -1;` — and `-1` **is**
`PSOCK_STALE`. So a genuine `tcp_write()` failure and a dead handle were the
same value at the source, and `sys_sock_write` then flattened everything
negative to `-EIO`, discarding the `-EBADF` that `psock_read` and `psock_close`
both document for a stale handle. A ring-3 caller could not distinguish "your
socket is gone, reopen it" from "the send failed, retry it".

Three distinct failures now carry three distinct codes:

| code | meaning | errno |
|---|---|---|
| `PSOCK_DENIED` (-2) | not this caller's slot | `-EPERM` |
| `PSOCK_EIO` (-3) | the operation itself failed | `-EIO` |
| `PSOCK_STALE` (-1) | stale/closed handle | `-EBADF` |

`sock_err()` maps all three, and `sys_sock_write` routes through it.

### 10.2 The previous owner got `-EPERM` for its own stale handle

`psock_close()` cleared `s->owner = 0`, leaving the slot owned by nobody. A
later call by the process that had *just closed the handle* then failed the
owner check in `psock_resolve()` **before** reaching the generation check, so it
got `PSOCK_DENIED` (`-EPERM`) where the header promises `PSOCK_STALE`
(`-EBADF`). Use-after-close of one's own fd is an ordinary application bug and
must report as a bad handle, not as a permission violation — the latter sends a
programmer hunting a capability problem that does not exist.

Fix: **stop clearing `s->owner` on close.** Retaining the retired owner until
the slot is reallocated produces exactly the documented split at no extra state
— the previous owner matches on owner, then trips `!s->used` and gets STALE;
an unrelated caller still mismatches and gets DENIED. Safe because
`psock_reap_owner()` gates on `used` (a retired slot is never re-closed) and
`psock_connect()` overwrites `owner` when it reallocates.

The two `psock_connect` rollback sites still clear `owner`, and should: there
the caller never received a handle at all, so the slot is genuinely unclaimed.

### 10.3 The header contradicted itself about what ring 3 holds

`pradyos_net.h` opened with "ring 3 holds the returned **slot index**" and then,
four lines later, "the returned value is an opaque HANDLE `((gen << 3) | slot)`,
**not a bare slot index**." A caller who believed the first half and used the
value as an array index would address the wrong slot — handle 9 is slot 1, not
slot 9. The stale half is removed.

### 10.4 OPEN-1 was recorded as root-caused when it is not

`docs/PRADYOS_MASTER_PLAN.md` and `CLAUDE.md` both carried OPEN-1 as
"**ROOT-CAUSED — DDR-987**". That overstates what is established, and it
contradicts this repo's own records: DDR-985 refuted its own Claim A, and
DDR-987 §5 says the gate suite cannot prove the fix at a ~1/20 base rate.

The precise position, now recorded in both files:

- **Established:** a real cross-CPU lwIP use-after-free exists and is fixed. Its
  artefact is a `#GP` with `RAX=0xDDDDDDDDDDDDDDDD` (kheap `POISON_FREE`) in
  `tcp_output`, reached from `sys_sock_connect`.
- **Not established:** that OPEN-1's `#PF` is that same defect.

OPEN-1 stays **OPEN**, and a green CI suite is not evidence for closing it —
only a 20× `smoke-surfdestroy` campaign on the merged tip would be.

This is the same discipline DDR-985 applied to itself when its own hypothesis
failed, and it should not have lapsed two documents later.

## 11. A failed gate now keeps its serial capture

DDR-987 moved `SERIAL_LOG`'s default out of `/tmp` (§NON-NEGOTIABLE 7) so a
failing gate's capture would survive. That fix was **half of one**, and the
review caught the missing half: every failure path in `boot_test.sh` still
called `serial_rm`, which deletes the capture whenever `KEEP_SERIAL` is unset.

Relocating a file and then deleting it anyway fixes nothing. Worse, the deletion
is worst exactly where it hurts most:

- A **failed** gate is precisely the run whose capture is the only evidence.
- **CI never sets `KEEP_SERIAL`**, so in CI the artefact was destroyed 100% of
  the time — every CI failure this project has tried to diagnose was diagnosed
  without the capture that existed and was thrown away.

This is the other half of why the DDR-985 run-16 panic reached
`exception: #PF page fault` and no further: the `vector=`, `RIP=` and `CR2=`
lines were written to a file that the failure path then removed. I recorded the
`/tmp` default as *the* cause at the time. It was one of two, and the second one
is the one that matters in CI.

**Change:** the nine failure sites call `serial_keep_fail()`, which keeps the
capture unconditionally and prints its path so the evidence is *named in the
gate output* rather than left to be guessed at. Only the single PASS site still
deletes. `KEEP_SERIAL=1` still forces retention on a passing run, so no existing
diagnostic workflow changes.

The asymmetry is deliberate: a passing gate's capture is worth nothing and
accumulating one per gate per run would fill the disk, while a failing gate's
capture is sometimes the only thing standing between a defect and another round
of guessing.

### 11.1 A methodology error in the campaign that measured this

The `smoke-evresize` 20× campaign run alongside this work is **not** a clean 20×
on one binary, and it should not be quoted as one. `smoke-evresize` depends on
`$(IMG)`, so when I edited `lwip_port.c` / `sys_socket.c` mid-campaign, run 4
rebuilt the kernel. The honest split:

| runs | kernel | result |
|---|---|---|
| 1–3 | `4ef7bd008c4c969d` (pre-§10) | 3/3 pass |
| 4–20 | `e3919140872fd2ea` (post-§10) | 17/17 pass |

I had explicitly guarded against editing `boot_test.sh` mid-run, having broken a
gate that way earlier the same session — and then edited C sources the gate's
own prerequisites rebuild, which is the hazard §INV.10 exists to name. Guarding
one instance of a class and missing another is how §11 of DDR-987 went wrong
too.

Two things follow, and only two:

1. **17/17 on `e3919140872fd2ea` is a real result** for the code being pushed,
   and it incidentally proves that code compiles `-Werror`-clean and boots.
2. It does **not** show DDR-988 fixed the CI `smoke-evresize` failure. That
   failure has never reproduced locally — 20 boots here, 0 failures, versus 1 in
   3 CI runs on `3f6dbff`. Consistent with the OPEN-12 family of CI-only
   intermittents, and **not attributed to this work**. The `[hb]` heartbeat now
   carries `net_skip=`, so a recurrence can implicate or exonerate lwIP directly
   instead of leaving it open.

## 11.2 The keep-on-fail change broke run isolation — caught by CI

§11 reddened **eight of ten shards** on `2d3e6f0`, all deterministically at
`smoke-selftest`:

```text
FAIL: clean boot is unaffected by the global list — exit 1, expected 0
[smoke] FAIL — a probe reported 'AGENT_METRICS FAIL' during this gate's boot.
```

That is `boot_test.sh`'s own DDR-785 self-test, and the diagnosis is that the
failing case matched a forbidden pattern belonging to a **different, earlier
case**.

**Mechanism.** `selftest.sh` reuses one path — `SERIAL_LOG="$WORK/serial.log"` —
for all seven cases. Until §11, every exit path deleted that file, so each case
started from nothing. **Isolation was a side effect of the cleanup.** §11 kept
failed captures and thereby removed the side effect, leaving a window between
`boot_test.sh` beginning its scan and the stub qemu's `: > "$log"` truncation.
In that window the previous case's content is live and matchable.

This is worth stating plainly: the deletion had two jobs, and only one of them
was documented. I removed it for the documented reason without asking what else
it was holding up.

**Fix — both halves, because either alone is wrong:**

1. **Truncate `SERIAL_LOG` at startup.** A gate may never inherit an earlier
   run's serial content, whatever the path and whoever wrote it. Isolation is
   now explicit rather than a by-product of cleanup.
2. **Keep the failed capture under a unique name** (`<path>.fail-<pid>`).
   Without this, (1) would have the next gate truncate away the very evidence
   §11 exists to preserve — trading one silent data loss for another.

**Mutation-checked in both directions**, since a fix that satisfies the gate
without preserving evidence would be worse than no fix:

- `smoke-selftest`: 7/7 pass — isolation restored.
- A deliberately failed gate (bogus sentinel) prints
  `[boot_test] FAIL — capture kept: …/serial-18001.log.fail-18001` and leaves a
  **7,681-byte** capture on disk — evidence still preserved.

### 11.3 `QEMU_ERR` was in `/tmp` all along

Verifying 11.2 surfaced a second one: `QEMU_ERR="$(mktemp)"` — the same
§NON-NEGOTIABLE 7 violation DDR-987 fixed for `SERIAL_LOG` and did not notice
one line further down. It was harmless while the file was always deleted; it is
not harmless now that §11 preserves it on failure, because keeping evidence in
the directory the rules call unreliable is not keeping it. `QEMU_ERR` now lives
under `build/gatelogs/` with a `mktemp` fallback if that is unwritable.

### 11.4 What this says about the local gate set

The regression was deterministic — eight of ten shards, first run — and my local
run missed it because `smoke-selftest` was not in the twelve gates I chose. I
picked gates by *what the C changes touched* (sockets, network, shell, block)
and `boot_test.sh` is touched by **every** gate, including the one whose only
job is to test `boot_test.sh` itself. A harness change needs the harness's own
gate, and that is not something to rediscover next time: **any change under
`tools/qemu_runner/` must run `smoke-selftest` before push.**

### 11.5 SKIP is not FAIL — and an empty capture preserves nothing

The 11.2 fix was verified by its own gate and still shipped two defects, both
found by checking a detail the gates do not assert on: three zero-byte
`qemuerr-*.log` files left behind by seven **passing** gates.

**Cause 1 — I converted the call sites by pattern, not by meaning.** §11 replaced
every `serial_rm "$SERIAL_LOG" "$QEMU_ERR"` except the last with
`serial_keep_fail`, on the assumption that "not the final PASS site" implies
"failure site". It does not. Classifying the nine by the exit code that follows:

| exit | sites | failure? |
|---|---|---|
| `exit 0` | 1 (the `SKIP (nothing to boot)` path) | **no** |
| `exit 1` / `exit 3` | 8 | yes |

The SKIP path is a clean no-op — no image to boot yet, "expected during Phase 0"
— and §11 made it announce `FAIL — capture kept` and leak an empty file. Exactly
one of the nine was misclassified, which is what makes a mechanical
find-and-replace across exit paths a bad way to change exit-path semantics.

**Cause 2 — `serial_keep_fail` skipped empty files instead of removing them.**
It tested `[ -s "$__f" ] || continue`, so a zero-byte capture was neither kept
nor deleted. Every host-env failure (QEMU refused to start, OVMF missing) and
every SKIP left a 0-byte file in `build/gatelogs/`. An empty capture preserves
nothing by definition; keeping it is not caution, it is litter, and litter in
the evidence directory makes real captures harder to find.

**Fixes:** the SKIP path returns to `serial_rm`; `serial_keep_fail` removes
empty files, and on a successful copy removes the original (the evidence lives
in the `.fail-<pid>` copy, which the next run's truncate cannot reach).

**Verified both directions again:**

- `smoke-selftest` 7/7; `smoke-net`, `smoke-capnet`, `smoke-shell` green, with
  **0** leftover files.
- A deliberately failed gate keeps **both** captures under unique names:
  `serial-28255.log.fail-28255` (7,879 B) and `qemuerr-28255.log.fail-28255`
  (70 B) — the latter being exactly the file §11.3 moved out of `/tmp`, now
  actually surviving a failure.

**The pattern across 11.2 and 11.5 is worth naming.** Both were caused by
changing a cleanup path without enumerating what that path was doing: 11.2 lost
run isolation, 11.5 lost the SKIP/FAIL distinction. A deletion in a harness is
rarely only a deletion, and "the gate still passes" did not catch either one —
11.2 needed CI and 11.5 needed looking at the directory afterwards.

## 12. The §4.1 liveness argument was overstated — corrected

I asked for §4.1 to be attacked rather than accepted. It did not survive intact.

### 12.1 The documented lock order is incomplete, and the "no holder blocks" claim is false

§4.1 asserts:

> Every acquire of `g_net_lock` is followed by a release in bounded time: no
> holder yields, sleeps or blocks.

**The last clause is false.** Holders block on other spinlocks, on two paths I
did not enumerate:

1. **console.** `net_drain_locked()` calls `pradyos_netif_rx()` *while holding
   `g_net_lock`* (that is the whole design). That enters lwIP's receive path,
   which reaches `tcp_echo_recv()` and `tcp_echo_accept()` — and both call
   `kputs()`, which takes `g_console_lock` with a **blocking**
   `spin_lock_irqsave` (`console.c:56`). So a `g_net_lock` holder can spin,
   interrupts disabled, waiting on another CPU's console output.
2. **heap → pmm.** lwIP allocation under the lock (`pbuf_alloc`, `tcp_seg`)
   routes through `kmalloc`/`kfree`, which take `g_heap_lock`, which can reach
   the PMM lock.

The real order is therefore not `net → rxq → heap` but:

```text
g_net_lock → g_console_lock
g_net_lock → g_rxq_lock
g_net_lock → g_heap_lock → g_pmm_lock
```

§4.3 is corrected to this. Recording an incomplete lock order is worse than
recording none: the next person reasoning about a deadlock will trust it.

### 12.2 What survives, stated precisely

The conclusion is not overturned, but its *justification* is now conditional
rather than absolute:

- **Claimed before:** holds are bounded because holders never block.
- **True:** holds are bounded **provided every nested lock's holder is itself
  bounded.** Console, heap and PMM holders do not yield and do not call back
  into lwIP, so the composite is still bounded — but that is a claim about three
  other subsystems, not a property of this design, and it must be re-checked
  whenever any of them changes.

**No deadlock cycle exists.** Nothing takes `g_console_lock` and then enters
lwIP. The nearest candidate is `idt.c`, which calls `net_poll_tick()` at
`ticks % 10` and the `[hb]` heartbeat at `ticks % 500`; on the tick where both
fire they run **sequentially, not nested**, and `net_poll_tick` has released
before the heartbeat takes the console lock.

**The real residual cost** is latency, not liveness: because the drain runs
under the lock, a network RX drain can now wait on serial output, which under
TCG is slow. §11's `net_defer` counter is the denominator if this ever needs
measuring.

### 12.3 What I am NOT doing here, and why

The suggested deeper fix — forbid console output from callbacks reached under
`g_net_lock`, and make the deferred-work executor independent of any holder that
can block on heap or PMM — is correct in principle and **out of scope for this
PR**:

- `PRADYOS_NET_TCP_OK` / `PRADYOS_NET_TCP_READY` are **gate sentinels**.
  Removing those `kputs` calls silently guts `smoke-net`, which is exactly the
  vacuous-gate failure mode §9 caught the hard way.
- Deferring them (flag under the lock, print outside) is a real change to gate
  timing, days from a release, in a PR whose job is to *recover* a red branch.
- An independent executor means a kernel worker thread — a larger change than
  the defect it addresses, and unmeasured.

Recorded as owed work rather than done badly. The same reasoning as DDR-989 §6:
a recovery PR is the wrong vehicle for a subsystem change, and PR #12 is what
happens when that line is crossed.

### 12.4 Also fixed: the INT/TERM trap discarded the capture

`boot_test.sh` installed `trap 'kill "$qemu_pid"; exit 130' INT TERM`, which
bypassed `serial_keep_fail` entirely — a cancelled run threw away its serial log
at exactly the moment that log is most likely to hold the only useful diagnosis.

And cancellation is **routine here, by that trap's own comment**: the workflow
concurrency group cancels the older run whenever two dispatches land on one ref,
which is precisely what happens while chasing the 3-green rule. "The original
file is left in place" is not a defence either — §11.2 now truncates
`SERIAL_LOG` at the start of the next run, and a cancelled CI workspace is
discarded without an artifact upload.

`on_interrupt()` now reaps QEMU, stops the QMP watcher if one is running,
preserves both captures via `serial_keep_fail`, prints their paths, and exits
130. Verified by sending SIGTERM to a live gate: `INTERRUPTED` printed, both
captures kept (7,879 B serial + 70 B qemuerr), `rc=130`.

### 12.5 Confirmed by review: retired-owner PID reuse is safe

The §10.2 change I was least sure of was independently checked, and the check
found the mechanism I had not: `sched.c:1514-1519` calls `socket_reap_pid()`
**before** the thread becomes a zombie, so a PID cannot be recycled while it
still owns a live proxy socket. The residual effect is exactly what §10.2
claimed and no more — a recycled PID can distinguish its predecessor's retired
slot from another PID's slot, an information disclosure about retired-slot state
and not an authorization bypass, since `!used` still blocks every operation.
