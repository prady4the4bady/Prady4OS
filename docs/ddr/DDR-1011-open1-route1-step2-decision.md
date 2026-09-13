# DDR-1011 — OPEN-1 route 1: the STEP 2 decision, and the discriminator that is now armed

**Status:** DECISION. Route 1 is **NOT closed** and cannot be closed on today's
evidence. What changed is that the next occurrence will settle it, which was not
true this morning.

PR #17 STEP 2: *"Route 1 remains a CI-only hang with no captured artefact yet. If
it cannot be closed with real evidence before the deadline, say so explicitly in
the release notes rather than leaving it ambiguous."* This file is that
statement.

---

## 1. Where route 1 stood, and the one thing that moved

OPEN-1 is three signatures (DDR-990 §12). Routes 2 and 3 are closed — route 2 at
95% power, 60/60 clean on one kernel hash (DDR-1000 §9); route 3
mutation-proven both ways (DDR-990 §9). **Route 1 is the CI-only one**: a hang
inside `sys_read`/`vfs_read`, recorded as printing nothing at all.

DDR-1009 §2 moved it, on a capture from `81274f4`, shard 6, `smoke-msixap`:

```
SYSOPEN OK
SYSFSTAT OK

*** NEXUS KERNEL PANIC ***
[boot_test] FAIL — capture kept ...
qemu-system-x86_64: terminating on signal 15 from pid 5785 (timeout)
```

Route 1's recorded stopping point, on a **different gate**, and **it panicked**.
So "route 1 prints nothing, therefore no panic-based detector can address it"
(DDR-994) is too strong as stated.

## 2. The tempting inference — and why it does not hold

DDR-1010 reproduced OPEN-2 locally, and its capture stops in the *same short
stretch* of `systest`'s syscall sequence:

```
199: SYSIO EBADF OK
200: SYSIO EFAULT OK
201: [fd] write EBADF pid=4026597203        <- garbage pid, ROM-resolved
202: [percpu] gs FAIL (syscall ctx)
205: *** NEXUS KERNEL PANIC ***
```

Two captures, two gates, both dying inside the same handful of `systest`
syscalls, both ending in a panic. The obvious reading is that OPEN-1 route 1 and
OPEN-2 are one defect — the SWAPGS corruption DDR-1010 names.

**That reading is not supported, and the reason is precise.** Here is a healthy
boot (campaign run 1, kernel `4b3181f13b2d76aa`):

```
200: SYSIO EFAULT OK
201: SYSOPEN OK
202: SYSFSTAT OK
203: SYSREAD OK
206: [percpu] gs OK (syscall ctx)
207: [percpu] current OK (syscall ctx)
```

The DDR-SMP-3a probe is **one-shot on the first `sys_getpid`**, and that lands
**after `SYSREAD OK`**. So:

| capture | died at | had the probe run? |
|---|---|---|
| DDR-1010 local (OPEN-2) | between `SYSIO EFAULT OK` and `SYSOPEN OK` | **yes** — and it said FAIL |
| DDR-1009 CI (route 1) | between `SYSFSTAT OK` and `SYSREAD OK` | **no** — died first |

The route-1 capture died **before the only GS detector in that kernel ever ran.**
Its silence about GS is therefore not evidence that GS was fine; it is not
evidence of anything. Attributing route 1 to the SWAPGS defect on a shared
stopping point would be colour-matching — the exact error DDR-975 §7, DDR-966,
DDR-969 and DDR-973 each had to retract.

Note also that the two stopping points are **not** the same: OPEN-2's is two
syscalls earlier, and OPEN-2's boot never reached `SYSOPEN`. "Same neighbourhood"
is not "same point".

## 3. What is now armed, and why the next occurrence settles it

DDR-1010 §7 moved the check from one-shot in `sys_getpid` to **every syscall**, at
the top of `syscall_dispatch`, before anything dereferences `current_thread`.

That changes route 1's evidentiary position completely. A boot dying between
`SYSFSTAT OK` and `SYSREAD OK` now runs the check **on `SYSFSTAT` and on every
syscall before it**. So the next route-1 occurrence discriminates:

- **`[percpu] gs FAIL (syscall ctx) apic=N num=M …` present** → route 1 *is* the
  OPEN-2 SWAPGS defect, and `num` says which syscall lost GS.
- **absent** → route 1 is **not** OPEN-2, and the probe having run and stayed
  quiet is real evidence rather than a gap.

Either way it is a result. That was not true of any previous route-1 capture.

## 4. The decision

**Route 1 is OPEN at the deadline, and should be stated as open in the release
notes.** Specifically, and this is the wording the notes should carry:

> A CI-only hang (OPEN-1 route 1) remains unexplained. It has been observed
> twice, both times in CI, stopping inside a fixed window of the boot self-test's
> syscall sequence; the second occurrence panicked without printing a register
> block. Routes 2 and 3 of the same issue are closed on measured evidence. A
> continuous SWAPGS-discipline probe now ships in the kernel and will, on the
> next occurrence, determine whether route 1 shares OPEN-2's cause.

It is **not** closed, and it is **not** claimed to be harmless. What is claimed:
its rate is low (2 occurrences across the CI history examined), routes 2 and 3
are closed with real power, and the next occurrence is now diagnostic rather than
mute.

### 4.1 What would change this decision

A route-1 occurrence carrying `[percpu] gs FAIL` before the deadline would merge
it into OPEN-2 and put it behind DDR-1010's root cause. Nothing else available in
the remaining time closes it: the campaign that could raise the local rate is
running against `smoke-blk-integrity` (OPEN-2's gate), and route 1 has **never**
reproduced locally — 60/60 clean in DDR-1000's campaign, which is why DDR-1000
§9 declined to claim it in the first place.

## 5. What this file does NOT claim

- Not that route 1 and OPEN-2 are the same defect. §2 is explicitly a refusal to
  make that inference.
- Not that route 1 is rare enough to ignore. Two occurrences is a count, not a
  rate; the denominator (how many CI suite-runs executed the affected gates) was
  not computed here, and DDR-1009 §1 shows how much such pooling can change a
  picture.
- Not that the discriminator will fire. It fires only if route 1 recurs, and
  route 1 has been seen twice in the examined history.
