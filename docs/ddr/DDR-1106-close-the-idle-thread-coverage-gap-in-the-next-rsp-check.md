# DDR-1106 — Closing DDR-1105's idle-thread coverage gap

**Status:** DESIGN. No code, no gate, no defect found and none alleged.
**Build is deliberately HELD** until CI rules on DDR-1105 (`c204ed0`): that was a
kernel change on the hottest path and stacking a second one on top of an
unjudged first would make an attribution impossible (DDR-1042's failure mode as
a *process* rather than as a mutation).

---

## §1 — The gap is one DDR-1105 created and stated, not one it hid

DDR-1105 §3 records a coverage limit in as many words: the check begins
`if (next->kstack_base)`, and **every idle thread has `kstack_base == 0`**, so
idle threads are skipped. `init_idle()` (`sched.c:879`) `memset`s the whole tcb
and assigns `name`, `state`, `quantum`, `caps`, `is_idle`, `on_cpu` and the FPU
template — and never `kstack_base`, because an idle thread has **no `kmalloc`'d
kernel stack**: it runs on the stack it was *entered* on.

The skip was not optional and is not regretted — without it the check would have
fired on nearly every switch and reddened all 179 gates on the first boot. What
this DDR asks is narrower: **those stacks do exist and their bases are knowable,
so the tcb can simply say where they are.**

**Why it is worth doing at all:** idle is not a marginal case. It is the thread
every CPU falls back to whenever its runqueue drains, so across a boot it is
plausibly the **most-switched-to thread in the system** — and it is therefore
the thread whose frame a stale or recycled `next->rsp` is most likely to select.
Leaving it uncovered leaves the busiest target unwatched.

---

## §2 — Both idle stacks are exactly `STACK_SIZE`, measured

This is the fact that makes the change small: the existing window arithmetic
(`rsp + 64 <= base + STACK_SIZE`) works unchanged, with **no new constant** and
no per-thread size field.

- **BSP idle** runs on `kernel_stack` in `arch/x86_64/boot.asm`'s `.bss`, where
  `KSTACK_SIZE equ 16384` — **identical to `sched.c:16`'s `STACK_SIZE 16384u`**.
  `boot.asm:25` sets `rsp` to `kernel_stack_top` before entering C, and the BSP
  never leaves that stack: at `sched_init` it *is* `idle0`, so `idle0->rsp` is
  first written by `context_switch`'s save, on this very stack.
- **AP idle** runs on `pmm_alloc_pages(2)` (`smp.c:347`), and **the argument is
  an ORDER, not a count** — `pmm.h:23` declares `pmm_alloc_pages(unsigned order)`
  — so it is 2^2 = 4 pages = **16,384 B**, and `smp.c:354` writes the mailbox top
  as `stack + 4 * PAGE_SIZE`, exactly the end of that block.

**CHECKED RATHER THAN ASSUMED, and the result is that the code is correct:** the
`(2)` / `4 * PAGE_SIZE` pairing is the shape that would be an 8 KiB overrun if
the argument were a count, so it was verified against the declaration rather
than read past. It agrees, and `smp.c:347`'s own comment (`/* 16 KiB AP stack */`)
is accurate. **No defect is found here and none is alleged** — recorded because
an audit that only reports errors is not an audit.

---

## §3 — The plumbing, and where each base comes from

- **BSP:** `kernel_stack` is currently **not exported** — `boot.asm:19` declares
  `global kernel_entry` and nothing else — so this needs one `global
  kernel_stack` directive plus an `extern` declaration on the C side.
- **AP:** the base is known in `smp_start_aps` at the moment of allocation
  (`smp.c:347`), *before* the SIPI, while `init_idle` runs later **on the AP
  itself** (`sched.c:1003`, inside the AP's own entry path). So the base must
  travel from the BSP to the AP: a small `g_ap_stack_base[]` array in `smp.c`
  written before the SIPI, with an accessor read from the AP path.

**A DERIVATION IS AVAILABLE AND IS REFUSED.** The AP is *running on* the stack
when `init_idle` is called, so the base could be computed as
`rsp & ~(STACK_SIZE - 1)` — correct **only if** a buddy allocator's order-2 block
is 16 KiB-aligned. That is almost certainly true and is exactly the kind of
premise this project has been bitten by: it would make the check's correctness
depend on an allocator property nothing states, asserts or tests, and a future
allocator change would silently reintroduce false positives on the hottest path.
**Carry the base explicitly.**

---

## §4 — THE VACUITY TRAP, and it is the whole reason this needs a DDR

The obvious evidence is *"the gates still pass and nothing fires."* **That is
worthless here, and worse than worthless because it looks conclusive.**

If the new bases were wrong in the *forgiving* direction — left 0, or assigned
0 by some path — **the check would skip exactly as it does today**, and every
gate would pass. So a clean run is equally consistent with "idle is now
correctly covered" and with "idle is still being skipped". The passing negative
cannot tell those apart, and it is the reading a session would naturally take.

**What only a real fix can produce is a CAUGHT idle frame.** The discriminating
proof is therefore a mutant that corrupts an **idle** thread's saved `rsp`:

- **Before DDR-1106** that mutant is **SILENT** — `kstack_base == 0`, the check
  skips, and the machine takes the corrupt frame.
- **After DDR-1106** the same mutant must print
  `[schedcheck] next->rsp invalid tid=…` and halt.

Same mutant, two outcomes, on two recorded hashes — the DDR-1066 M1/M2 shape,
where the pre-fix tree *is* the control. That is the only arm that convicts, and
it must be run in **both** directions or the change ships unproven.

**The other half is still owed and is still the load-bearing negative:** the real
gates must show the check **never fires** on a healthy boot. But note the
asymmetry — here that negative is *stronger* than it was for DDR-1105, because
idle is switched to constantly, so a wrong base in the *strict* direction would
redden essentially every gate immediately and loudly. The dangerous direction is
the forgiving one, and §4's mutant is what covers it.

---

## §5 — Cost, and what is NOT changed

The check itself is **untouched** — same four clauses, same order, same mask.
What changes is that two tcbs per CPU stop taking the `kstack_base == 0` skip.
So the per-switch cost is unchanged for every thread that was already covered,
and for idle it becomes the same three loads, add, five compares and branches
DDR-1105 §5 costed.

**Not changed:** `STACK_SIZE`; the window arithmetic; the mask; the halt-and-name
behaviour; `GLOBAL_FORBIDDEN` (77, `'[schedcheck]'` already present); the gate
count (179 — **no gate arm, for DDR-1105 §8's reason unchanged**: the triggering
condition cannot be manufactured in product).

---

## §6 — NOT CLAIMED

- **No fix.** OPEN-2 still has no named cause and this does not name one
  (§NON-NEGOTIABLE 3); it widens what the *next* occurrence can be caught on.
- **No defect is found in `init_idle`, `smp_start_aps` or `pmm_alloc_pages`**,
  and none is alleged — `init_idle` leaving `kstack_base` at 0 was correct for a
  thread with no allocated stack, and §2 verified the order/count pairing agrees.
- **Not built.** This is design only, held behind CI on DDR-1105 (§preamble).
- **The `rsp & ~(STACK_SIZE-1)` derivation is refused**, not deferred (§3).
- **Nothing is claimed about whether idle frames are ever actually corrupted** —
  that is the open question, and covering the path is what makes it askable.
