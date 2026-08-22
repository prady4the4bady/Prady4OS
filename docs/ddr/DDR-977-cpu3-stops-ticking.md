# DDR-977 — B#3 root-caused: **CPU 3 stops taking timer interrupts**; virtio-blk is the victim

Status: ACCEPTED (root-cause localisation + instrument). **No fix in this DDR.**
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).

**Answers:** DDR-976 §7's specified measurement.
**Supersedes:** DDR-976 §6's three candidate explanations — all three are refuted.

---

## 1. Result

The instrument DDR-976 §7 specified now prints, at every completion timeout, the
absolute tick counter of **every** CPU. Four `-smp 4` boots, 60 timeouts:

```text
[vblk] compl wait timeout unit=2 dest_cpu=3 dest_dticks=0 dest_abs=369
       bsp_abs=916  dest_present=1 ticks[916,872,870,369]  on_cpu=0 lba=200
[vblk] compl wait timeout unit=2 dest_cpu=3 dest_dticks=0 dest_abs=369
       bsp_abs=1416 dest_present=1 ticks[1416,1372,1370,369] on_cpu=0 lba=208
```

| boot | timeouts | `ticks[cpu0,cpu1,cpu2,cpu3]` at first → second timeout |
|---|---|---|
| 1 | 8 | `[916,872,870,`**`369`**`]` → `[1416,1372,1370,`**`369`**`]` |
| 2 | 20 | `[859,841,839,`**`336`**`]` → `[1359,1341,1339,`**`336`**`]` |
| 3 | 23 | `[818,800,800,`**`298`**`]` → `[1318,1300,1299,`**`298`**`]` |
| 4 | 9 | `[841,822,822,`**`320`**`]` → `[1341,1322,1321,`**`320`**`]` |

**CPUs 0, 1 and 2 advance by exactly +500 between consecutive timeouts — one
deadline's worth. CPU 3 does not advance at all, in any boot.**

`pc->ticks` is incremented in `sched_tick()`, which an AP reaches only from its
own LAPIC timer interrupt (`idt.c:293`). A frozen counter therefore means
**CPU 3 is not taking its timer interrupt.** It is not slow, not busy, not
descheduled: it is not being interrupted.

CPU 3 froze at 369 / 336 / 298 / 320 of its own ticks — a few hundred in, varying
per boot, while the other three were at ~800-900. So it ran normally for a while
and then stopped. `[smp] cpu 3 up id=3` / `locks OK` / `percpu OK` / `tss OK` all
print, and `[smp] cpus online=4/4`: it comes up fine and dies later.

## 2. The three DDR-976 §6 candidates are all refuted

1. **"the AP is halted and MSI-X is not waking it promptly"** — refuted as
   stated. This is not about MSI-X delivery: CPU 3's *own LAPIC timer* also stops
   arriving, and that has nothing to do with the block device's vector.
2. **"the AP is busy or briefly interrupt-masked"** — refuted. "Briefly" is
   wrong by orders of magnitude: the freeze is permanent for the rest of the boot
   (thousands of ticks), and a *busy* CPU would still take timer interrupts.
3. **"the programmed destination APIC ID is occasionally wrong / the message is
   lost"** — refuted. A wrong MSI-X destination cannot stop the target CPU's
   timer interrupt.

The real shape is a fourth thing none of them named: **one specific AP stops
taking interrupts entirely, part-way through boot.**

## 3. Why this presents as a block-device bug

`virtio_blk.c:342` routes each device's completion vector to an AP:
`dest_idx = 1 + (unit % (ncpu - 1))`. At `ncpu = 4`, unit 2 → **CPU 3**. So the
one device whose completions are pointed at the dead CPU is the one that times
out, and `unit=2 dest_cpu=3` is invariant across all 60 timeouts.

That is why DDR-878 found the block layer clean and was right to: **the block
layer is clean.** It is the only subsystem that *notices*, because it is the only
one that blocks on a 5-second deadline waiting for that CPU to do something.

**A discriminator I ran because the obvious inference was not safe.** "Only
unit 2 times out" is not by itself evidence that CPU 3 is the sick one — unit 2
is the SFS scratch disk the self-tests hammer, so it could simply have been the
only device with enough traffic to expose a fault all the APs shared. Dumping
*every* CPU's counter settles it: CPUs 1 and 2 keep ticking. The fault is CPU 3's
alone, not a shared AP fault seen through the busiest device.

**A corroboration I tried and had to withdraw.** The heartbeat's `rqcpus` field
looked like independent confirmation (4 → 3 early in one boot). It is not: a
0-timeout boot showed `rqcpus` at 3 and even 2, while a 23-timeout boot showed
mostly 4. That field tracks something else and is uncorrelated with the freeze.
Recorded so nobody re-derives it as evidence.

## 4. Consequences for the backlog

- **B#3's symptom is real and its subsystem attribution is wrong.** It is not a
  "virtio-blk completion stall"; it is an AP-liveness defect that the block layer
  reports. Renaming it matters: every previous attempt to fix it inside
  `virtio_blk.c` was looking in a subsystem that is behaving correctly.
- **It is `-smp`-specific because APs are** (DDR-976 §5: 0/10 at `-smp 1`,
  17/20 at `-smp 4`). At `-smp 1` there is no AP to wedge and the destination is
  the BSP.
- **The 5 s × N stall is a real cost even when gates pass** — up to 28 timeouts
  in one boot, each a failed I/O returning `-EIO` (DDR-976 §2).

## 5. What is still unknown — and why no fix ships here

*Why* CPU 3 stops taking interrupts. Candidates, none yet discriminated:

- it is spinning inside a `cli` region that never re-enables (a lock held across
  a path that does not restore flags);
- it took a fault whose handler does not return, leaving it wedged with
  interrupts masked;
- its LAPIC timer got masked or its LVT reprogrammed;
- it halted in a state where its LAPIC no longer delivers.

These need different fixes and the next instrument is a different one — e.g. have
each AP record its last-known RIP/lock state, or have the BSP NMI a CPU whose
`pc->ticks` has not advanced for N ticks and dump where it is. §NON-NEGOTIABLE 3
applies: there is now a named mechanism (CPU 3 not interrupted) but not yet a
named *cause*, and this project has twice paid for fixing the first without the
second.

**Why CPU 3 and not CPU 1 or 2** is itself the sharpest available lead: it is the
highest-indexed AP and the last brought up.

## 6. The instrument ships

`kernel/drivers/blk/virtio_blk.c` — the timeout message now carries `unit=`,
`dest_cpu=`, `dest_dticks=`, `dest_abs=`, `bsp_abs=`, `dest_present=`, `ticks[…]`,
`on_cpu=` and `lba=`.

It is **failure-path only**: the healthy path pays one `percpu_get` read and
prints nothing. That property is deliberate — DDR-947 records an instrument heavy
enough to move the failure rate it was measuring (2/12 → 9/14), and this one
cannot, because it executes only after a deadline has already expired.

The two gates that treat `[vblk] compl wait timeout` as a `FORBIDDEN_SENTINEL`
(`smoke-blk-timeout`, `smoke-rename-sfs`) match on substring, and the new line
still begins with that exact text, so both keep working unchanged.
