# DDR-1152 — The compositor acted on keystrokes out of order, and CI's slower renders turned it into an expired mode switch

**Status:** IMPLEMENTED + M1 two-sided. Design written before the code (NON-NEGOTIABLE 5). Results in §5.
**Date:** 2026-09-26
**Trigger:** shard 8 red on `dfb1179` in **both** the push and pull_request
suites (CI 36270543974 / 36270540894), `kernel.bin: OK` in both.
`smoke-superkey` arm D (DDR-1147 §1.5):

```
got:  pending to=0|TOGGLE from=1 to=0|pending to=1|cancel to=1|pending to=1|cancel to=1|
want: pending to=0|TOGGLE from=1 to=0|pending to=1|cancel to=1|pending to=1|TOGGLE from=0 to=1|
```

The third armed switch was cancelled where the `ret` typed right after it should
have committed it. The arm's `grep -o` strips the cancel `reason=`, so the CI log
does not say which cancel branch fired. That is itself fixed here (§3.2).

## 1. The mechanism

The compositor's main loop reads two views of the same keystrokes from the PS/2
driver (DDR-991):

- the structured event ring, `SYS_KEY_POLL`, drained **16 events per iteration**
  (`struct key_ev kev[16]`, `user/compositor.c`);
- the ASCII ring, `SYS_INPUT_POLL`, drained **completely**, in the same
  iteration, immediately after.

Both are filled by the same IRQ, so as long as each iteration drains everything,
the two views stay in step. They do not stay in step once more than 16 events are
queued. The ASCII drain then acts on keystrokes **later** than the last event the
event loop processed.

`smoke-superkey`'s injector replays `m meta_l-m ret meta_l-m esc meta_l-m ret
ctrl-c` four times at 0.25 s per key. A round is about 24 events. The first
commit runs `render_and_announce` (11 `animate_toggle` frames plus
`set_ambiance(…, 2)`, ~13 full-screen renders at DDR-1029's ~0.93 s each under
TCG). During that render every remaining round is queued. After it:

1. The 16-event batch ends right after the **third** `meta_l-m` arms a switch,
   before the `ret` that answers it is processed.
2. The ASCII drain then reaches rounds 2–4's plain `m` keystrokes. Each one runs
   the DDR-707 direct path `SYS_SET_MODE(0)` plus `render_and_announce`, another
   ~12 s of rendering per keystroke.
3. The next iteration processes the `ret`, finds `now - g_mode_pending_at >
   MODE_CONFIRM_SECS` (10 s), and cancels with `reason=expired`.

Locally the renders are faster, the elapsed time stays under 10 s, and the gate
passes. That is the DDR-1009 class: one binary, two outcomes, decided by host
speed.

**The defect is the ordering, not the timeout.** The 10 s expiry is doing what
DDR-1147 designed it to do. What is wrong is that the compositor acted on
keystroke N+10 (a plain `m`) before keystroke N (the `ret`). A real user typing
quickly during a render hits the same thing.

## 2. What is established and what is not

- Established by reading: the two drain sizes, the plain-`m` direct path, and the
  ~13-render cost of a commit.
- **Not yet established:** which cancel branch fired in CI. The arm strips
  `reason=`. `expired` is the only branch consistent with the mechanism; a
  `reason=key` would mean a non-`ret` keystroke intervened, which the event ring's
  own order does not produce. §3.2 makes the next red say which.

## 3. The change

### 3.1 Drain the event ring to empty before the ASCII ring

The `SYS_KEY_POLL` block becomes a loop that repeats while a poll returns a full
batch of 16. Every queued event is then processed, in order, before any ASCII
keystroke is. The loop has no `break`, so wrapping it is mechanical. It is
bounded by what the driver has queued; the ring holds 128 events and
drops-newest on full.

**Refused:** raising `kev[16]` to a larger number. That moves the batch boundary
and keeps the defect.

**Refused:** lengthening `MODE_CONFIRM_SECS`. That hides the ordering defect
behind a bigger number and weakens the property DDR-1147 §1.5 exists for (a late
Enter must not flip the mode).

### 3.2 Arm D prints the reason

The failure print shows the matching `PRADYOS_MODE_CONFIRM` lines **with**
`reason=`. The comparison itself is unchanged.

## 4. Proof plan

- Fixed tree: `smoke-superkey` locally. Pass alone is weak here, because the
  pre-fix tree also passes locally.
- **The discriminating run makes the pre-fix race deterministic on a fast host.**
  Mutant M1 lowers `MODE_CONFIRM_SECS` to 1 (a timing mutant, not a product
  change) on the **pre-fix** loop: arm D should fail with `reason=expired`.
- M1 on the **fixed** loop should pass, because the `ret` is processed right after
  its arm, so the 1 s window is enough.
- Without the fixed-loop run, "the fix works" and "the host was fast" are the same
  observation.
- Regression: `smoke-compositor`, `smoke-modkeys`, `smoke-alttab` and
  `smoke-ctrlaltt` (all consume the event ring), plus `smoke-shell` 5/5.

## 5. Results

Each row records its own kernel. `compositor.elf` is embedded, so the kernel hash moves with it.

| run | loop | `MODE_CONFIRM_SECS` | kernel | arm D |
|---|---|---|---|---|
| M1 | pre-fix (16 per iteration) | 1 | `236a9d08b63a163e` | **FAIL**, got-line identical in shape to CI, new print: `cancel to=1 reason=expired` |
| M1 | fixed (drain to empty) | 1 | `fbf49a911314a705` | **PASS** |
| shipped | fixed | 10 | `1915cdc6f501bb17` | **PASS** |

- **§2's open question is settled:** the cancel is `reason=expired`, which is the branch the mechanism predicts. §3.2's print is what made that readable.
- The M1 pair differs only in the loop, and the pre-fix row fails on a fast host. So "the fix works" and "the host was fast" are no longer the same observation.
- Regression on the shipped kernel, all `rc=0`: `smoke-superkey`, `smoke-compositor`, `smoke-modkeys`, `smoke-alttab`, `smoke-ctrlaltt`, plus `smoke-shell` 5/5.
- Build is warning-clean.
- `kernel.bin` is 1,450,378 B, **size unchanged**, so the size/headroom pair is unaffected. Only the hash moved, which is how the change is known to have applied (DDR-1097).

## 6. Not claimed

- No kernel change. The driver rings are correct, and dropping newest on full is
  the documented behaviour.
- The DDR-707 plain `s`/`m` keys still switch mode **without** confirmation. That
  is a pre-existing test path that `smoke-compositor` depends on, and it bypasses
  DDR-1147's "one stray key must not flip the mode". It is **recorded, not
  changed**: removing it changes another gate's contract and is its own decision.
- No claim about any other intermittent. This is shard 8's red on `dfb1179` and
  nothing else.
