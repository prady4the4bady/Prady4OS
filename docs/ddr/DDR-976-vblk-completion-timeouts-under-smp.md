# DDR-976 — `[vblk] compl wait timeout` fires 301 times in 20 `-smp 4` boots

Status: ACCEPTED (measurement + named mechanism). **No fix in this DDR.**
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

**Reopens:** the virtio-blk half of B#3, which DDR-974 §5 had just declared
unseen. **Does not disturb:** DDR-878's ruling on the `slot_waiter` defect, which
this measurement independently re-confirms.

---

## 1. The measurement

Campaign: `smoke-rqstress` (`QEMU_SMP=4`), 20 runs, canonical kernel
`sha256 ab00c00c05fb6fb5e369c0841960f8ef6aa4b16054af3e55527824169f004ea9`
(1,061,246 B). Serial kept per run (`build/gatelogs/rq_serial<N>.log`).
Stray-QEMU pre-flight per §INV.3 before the campaign: no match.

**The gate passed 20/20.** The logs say something the exit codes do not:

| string | runs affected | total occurrences |
|---|---|---|
| `[vblk] compl wait timeout` | **17 / 20** | **301** |
| `[vblk] slot wait timeout` | 0 / 20 | 0 |
| `[vblk] slot wait list depth>=2` | 0 / 20 | 0 |
| `churn FAIL` / `rqstress FAIL` / `[BUG]` | 0 / 20 | 0 |

Per-run counts: 24, 23, 0, 23, 18, 20, 9, 24, 27, 28, 6, 18, 16, 5, 5, 28, 0,
14, 13, 0. Highly variable, three clean runs, up to 28 in one boot.

## 2. Why this is not cosmetic

`submit()` (`kernel/drivers/blk/virtio_blk.c:269-279`) does **not** retry:

```c
while (!v->req[s].done) {
    if (sched_block_timeout(&v->compl_lock, &v->req[s].done, 500) == -ETIMEDOUT) {
        kputs("[vblk] compl wait timeout\r\n");
        v->req[s].used = 0;
        v->req[s].waiter = 0;
        slot_wake_one(v);
        spin_unlock_irqrestore(&v->compl_lock, fl);
        return -EIO;                       /* <-- the I/O FAILS */
    }
}
```

Every one of those 301 lines is a **block read or write that returned `-EIO`**
to its caller. The gates stay green because the callers on those paths either
tolerate the error or do not assert on it. 500 ticks at 100 Hz is a **5-second**
stall per occurrence, which also explains why `-smp 4` boots run long.

## 3. Why nobody has been seeing it

Only two gates list the string as a `FORBIDDEN_SENTINEL`:

- `smoke-blk-timeout` (Makefile:788) — built by **DDR-955 specifically to prove
  "its deadline does NOT fire on healthy I/O"**. It sets **no `QEMU_SMP`**, so it
  runs **uniprocessor**.
- `smoke-rename-sfs` (Makefile:1159) — also no `QEMU_SMP`.

Every gate that runs at `QEMU_SMP=4` either does not watch for the string or does
not fail on it. The one gate whose stated purpose is to catch this deadline
firing has never been run in the configuration where it fires.

## 4. The mechanism — SMP-specific by construction

`virtio_blk.c:342-344`, choosing the MSI-X destination for each block device:

```c
unsigned ncpu = lapic_cpu_count();
uint32_t dest = (ncpu > 1) ? lapic_apic_id_at(1 + (unit % (ncpu - 1)))
                           : lapic_id();
```

The index starts at **1**, so with `ncpu > 1` the destination is **always an AP,
never the BSP**. With the four block devices this image creates and `ncpu = 4`:

| unit | vector | `1 + (unit % 3)` | destination |
|---|---|---|---|
| 0 | 56 | 1 | CPU 1 (AP) |
| 1 | 57 | 2 | CPU 2 (AP) |
| 2 | 58 | 3 | CPU 3 (AP) |
| 3 | 59 | 1 | CPU 1 (AP) |

(Confirmed in the boot log: `blk0 … msix vec=56` through `blk3 … msix vec=59`,
with `[smp] cpus online=4/4`.)

At `-smp 1` the same line takes the `lapic_id()` branch and every completion is
delivered to the BSP — the CPU that is also running the waiter. **The routing
difference between the passing and failing configurations is structural, not
incidental**, and it lines up exactly with the measured rates.

## 5. The `-smp 1` control

Same workload, same kernel, same sentinels, varying only `QEMU_SMP`:

| `-smp` | timeouts | serial lines |
|---|---|---|
| 1 | 0 | 427 |
| 4 | 1 | 453 |

One run each is weak on its own; §5.1 gives the uniprocessor side a real
denominator against the 17/20 already measured at `-smp 4`.

**A methodology note, because the first attempt at this control was vacuous.**
Run without a `FORBIDDEN_SENTINEL`, `boot_test.sh` is eligible for the DDR-785
early exit and terminates at `NEXUS KERNEL OK` — 33 lines, long before any block
I/O. Both arms reported 0 timeouts and the comparison meant nothing. Setting a
`FORBIDDEN_SENTINEL` makes each arm burn its full window (427/453 lines, matching
the campaign's 434-477) and is what makes the two arms comparable at all.

### 5.1 Uniprocessor arm result — 0/10, zero occurrences

Ten runs, `QEMU_SMP=1`, everything else identical to the §1 campaign:

```text
up run  1..10 : timeouts=0   lines=416..427   rqstressOK=1
SMP=1: runs_with_timeout=0/10   total_timeouts=0
```

Every run booted to completion (416-427 serial lines, against the `-smp 4`
campaign's 434-477) and printed `[smp] rqstress OK`. So the arms are comparable:
both reached the same point in the same workload, and one of them times out.

**The discriminator, side by side:**

| configuration | runs | runs with ≥1 timeout | total timeouts |
|---|---|---|---|
| `QEMU_SMP=1` | 10 | **0** | **0** |
| `QEMU_SMP=4` | 20 | **17** | **301** |

Under any reasonable reading this is not a sampling artefact: if the underlying
per-boot probability were the same in both arms, observing 17/20 in one and 0/10
in the other is vanishingly unlikely. The completion timeout is **specific to the
multiprocessor configuration**, which is what §4's routing difference predicts.

## 6. What is established, and what is not

**Established.** The timeout fires at a high, variable rate under `-smp 4` on the
shipped kernel; it returns `-EIO` rather than retrying; the completion vector is
routed to an AP whenever `ncpu > 1`; and the uniprocessor configuration, where it
is routed to the BSP, does not show it.

**Not established.** *Why* the AP does not service the completion inside 5 s. At
least three candidates remain open, and they need different fixes:

1. the AP is halted and the MSI-X is not waking it promptly;
2. the AP is busy or briefly interrupt-masked in a long region;
3. the programmed destination APIC ID is occasionally wrong or the message is
   lost.

Candidate 3 is *weakened but not excluded* by the data: a permanently wrong dest
would time out **every** I/O, and most succeed. Candidates 1 and 2 are both
consistent with an intermittent, bursty rate.

**No fix ships here.** §NON-NEGOTIABLE 3 wants a named mechanism from a real
failing artefact; there is now an artefact and a named *routing* mechanism, but
the three candidates above are not yet discriminated, and changing MSI-X routing
or the deadline on a guess is how the earlier B#3 hypotheses became folklore.

## 7. Next measurement (specified, not applied)

Record, at each timeout, which CPU the waiter was on and whether the target AP
had taken **any** interrupt recently — e.g. print `unit=`, `dest_cpu=`, and that
AP's `pc->ticks` delta beside the existing message. `pc->ticks` is already
maintained per CPU in `sched_tick`. That separates candidate 1/2 (the AP is alive
but late, or halted) from candidate 3 (the AP never sees anything).

Not applied in this DDR because the uniprocessor arm of §5.1 is measuring the
current `kernel.bin`, and R1 requires one kernel hash per measurement.

## 8. Backlog consequences

- **B#3's row is substantially right and should not have been softened.** Its
  mechanism ("`-smp 4` virtio-blk completion stall") matches this evidence. What
  was wrong in it is the sub-clause "timer/IRQ delivery under SMP" being treated
  as already-diagnosed, and DDR-974's §4 plan to restate the row as a lost-thread
  issue — that plan is withdrawn.
- **`smoke-blk-timeout` should gain an `-smp 4` arm.** A gate that exists to
  prove a deadline does not fire, which never runs in the configuration where it
  does fire, is the vacuous-gate pattern this project keeps re-finding
  (DDR-973 §6, DDR-880's detector, DDR-959).
- **Item 47 is separately not-reproduced** (DDR-974 §3.1, 0/20). The two are
  different defects and this DDR does not merge them.
