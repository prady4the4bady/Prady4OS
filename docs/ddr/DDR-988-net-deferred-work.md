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

`g_net_lock` → `g_net_rxq_lock` → `g_heap_lock`. The RX ISR takes only
`g_net_rxq_lock`, and the rxq lock is held across a `memcpy` and two index
updates with no calls out. No cycle.

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
