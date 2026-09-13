# DDR-1006 — OPEN-2 REOPENS: `[apfreeze]` in CI, at a site DDR-981 does not cover

**Status:** ARTEFACT RECORDED. Root cause NOT established. No fix attempted.
**Blocks:** the `dev/phase1` → `main` promotion. `fa29506` has 2 greens, not 3.

---

## 1. The trigger fired exactly as written

CLAUDE.md's OPEN-2 row closed DDR-981 with a standing condition:

> "`[apfreeze]` is now in `GLOBAL_FORBIDDEN`, so a recurrence names itself
> instead of hiding in a flake. **Reopen on the first `[apfreeze]` line in CI.**"

That line has now appeared. CI run **33281593947** (`workflow_dispatch`, shard 4,
tip **`fa29506`**), failing at gate 1 of 14, `smoke-smppreempt`:

```
[apfreeze] cpu=2 ticks=70 rip=0xFFFFFFFF8000A4FE cs=0x08 rflags=0x2402 if=0
           rsp=0x07C2AB50 lvt=0x20030 masked=0 svr=0x1FF swen=1 tpr=0
           isr48=0 irr48=1 pid=11 shot=1 bt=0xFFFFFFFF8000027A
```

**OPEN-2 is reopened on its own documented condition.** This is not a judgement
call and not colour-matching: the row named the sentinel in advance.

## 2. It is the DDR-981 *signature*, at a site DDR-981 does not cover

The register line is DDR-981's diagnosis verbatim: `masked=0` (LVT unmasked),
`swen=1` (LAPIC enabled), `isr48=0` (no stuck in-service vector), **`irr48=1`**
(a timer interrupt **pending and undelivered**), `tpr=0`, and `if=0` as the only
remaining blocker. The LAPIC is innocent; the CPU is running with interrupts
masked.

But the *site* is new. Resolved against `kernel.elf` (the CI kernel is
byte-identical to the local one — `fa29506` is docs-only over `d0a85b5`, kernel
`bb9c6187a30bb0dd`):

| address | symbol |
|---|---|
| `0xFFFFFFFF8000A4FE` (RIP) | `isr_dispatch + 0xC0E` |
| `0xFFFFFFFF80010C01` | `schedule + 0x11` |
| `0xFFFFFFFF800121BA` | `sched_tick + 0x36A` |
| `0xFFFFFFFF80009B9A` | `isr_dispatch + 0x2AA` |
| `0xFFFFFFFF8000027A` | `isr_common.gs_kernel_in + 0x8` |
| `0xFFFFFFFF80043301` | `smp_ap_entry + 0x221` |

So an **AP**, inside its **timer ISR**, called `sched_tick` → `schedule()`, and
is stuck there with IF clear.

**DDR-981 fixed `yield()`** — the ring-3-reachable spin path (`mnt_lock`, the
pipe waits, the blocking console read, `sys_yield`). This backtrace never enters
`yield()`. DDR-981's fix cannot apply, exactly as DDR-1001 could not.

## 3. §INV.1 is NOT reverted — checked, not assumed

The obvious first suspicion is that DDR-887's window was lost. It was not.
`kernel/proc/sched.c:712` still carries

```c
__asm__ volatile("sti; pause; cli" ::: "memory");
```

inside `schedule()`'s wait-for-other-CPU loop, and `sched_tick` still skips its
`schedule()` call while `g_in_switch[cpu]` is set. Both halves of §INV.1 are
present. Whatever is wrong is not a reintroduction of DDR-887.

## 4. Why this is a freeze and not a sampling artefact

`if=0` at a single sampled RIP would be unremarkable — the `cli` in that window
is one instruction wide, and an NMI can land on it. What rules that out is the
tick counter: **`ticks=70` on cpu 2 while the BSP heartbeats reach `t=1500`**.
CPU 2 stopped advancing for ~1400 ticks. A one-instruction sampling window does
not produce that.

The downstream damage is the DDR-977 §8.2 chain, present in the same log:

```
[vblk] compl wait timeout unit=1 dest_cpu=2 dest_dticks=0 dest_abs=70
       bsp_abs=571 dest_present=1 ticks[590,571,70,568] on_cpu=1 lba=2050
```

`dest_cpu=2`, `dest_abs=70` — the block layer waiting on the frozen CPU's own
tick count. Frozen AP → its MSI-X vector routed at it → `compl wait timeout`.

## 5. Rate, and what it does to the promotion

On tip `fa29506`, three independent runs:

| run | event | result |
|---|---|---|
| 33279970481 | push | **green**, 0 of 15 failed |
| 33279992304 | workflow_dispatch | **green**, 0 of 15 failed |
| 33281593947 | workflow_dispatch | **FAILED** — this artefact |

**1 in 3.** So `dev/phase1` has **two** greens on its tip, not three, and
§NON-NEGOTIABLE 1 is not satisfied. The promotion to `main` does not proceed on
this evidence.

This is the rule earning its keep. Two greens looked like a finished release;
the third found a live SMP defect on the exact candidate commit.

## 6. What is NOT claimed

- **No root cause.** The RIP is inside `isr_dispatch`, reached through
  `schedule()`; whether the CPU is spinning in the §INV.1 wait loop (and the
  window is somehow not admitting the timer), or is stuck elsewhere in dispatch,
  is **not** established from one line.
- **Not attributed to DDR-1004.** That change is confined to `smpresched_proof()`
  in `main.c`, a BSP-side boot self-test; it does not run in an AP's timer ISR.
  Attributing this to it because it is the most recent kernel change would be
  the post-hoc reasoning this project has caught itself in before. It is also
  not exonerated — nobody has measured it either way.
- **Not a flake.** §NON-NEGOTIABLE 3 forbids a fix without a named mechanism,
  and the same discipline forbids dismissal without one. "Flake" is not a root
  cause.

## 7. The next step, named

Reproduce locally: `smoke-smppreempt` at N=20 on kernel `bb9c6187a30bb0dd` via
`tools/ci/campaign_chunk.sh`, with the serial capture snapshotted per run (pass
the gate's real `SERIAL_LOG` path — the default is a per-PID file under
`build/gatelogs/`, and passing a path that does not exist yields an unscannable
campaign, as DDR-1000 §10 records). Count boots carrying `[apfreeze]`, and for
each, resolve the RIP.

If it does not reproduce in 20 local boots — as the DDR-1004 defect did not —
then this is CI-only like OPEN-1 route 1, and the honest next instrument is more
state at the freeze (which lock, which loop iteration), not another campaign.
