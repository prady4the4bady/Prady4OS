# DDR-1106 — Closing DDR-1105's idle-thread coverage gap

**Status:** BUILT. No defect found and none alleged; **no fix**, and OPEN-2 does
not move. What changes is what the *next* occurrence can be caught on.

**The hold is RELEASED and the result it waited on is recorded.** The build was
deliberately held until CI ruled on DDR-1105 (`c204ed0`), because that was a
kernel change on the hottest path and stacking a second one on an unjudged first
makes attribution impossible (DDR-1042's failure mode as a *process* rather than
as a mutation). **DDR-1105 is green:** binary `3933fa5f60ae607c` took **four**
full suites across two SHAs — `33c5be6` push (run 34686501068) and
pull_request (34686502896), then `7c558f0` push and pull_request
(34688536035 / 34688538214) — **40 shard jobs, every one `success`**, with each
shard's two DDR-1035 hash assertions (`Assert this shard got the binary the
build job produced`, and `Assert the binary did not change while the gates ran`,
the second under `if: always()`) green. **Zero `[schedcheck]` lines**: that
pattern is in `GLOBAL_FORBIDDEN`, so a fire reddens whichever gate boots, and
none did. Both docs-only commits leave the binary untouched, which is why all
four suites are observations of the same kernel.

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

## §6 — NOT CLAIMED (written at design time; §9 supersedes where they differ)

- **No fix.** OPEN-2 still has no named cause and this does not name one
  (§NON-NEGOTIABLE 3); it widens what the *next* occurrence can be caught on.
- **No defect is found in `init_idle`, `smp_start_aps` or `pmm_alloc_pages`**,
  and none is alleged — `init_idle` leaving `kstack_base` at 0 was correct for a
  thread with no allocated stack, and §2 verified the order/count pairing agrees.
- **Not built.** This is design only, held behind CI on DDR-1105 (§preamble).
- **The `rsp & ~(STACK_SIZE-1)` derivation is refused**, not deferred (§3).
- **Nothing is claimed about whether idle frames are ever actually corrupted** —
  that is the open question, and covering the path is what makes it askable.

---

## §7 — WHAT SHIPPED

Four files, and the check itself is **untouched** — same four clauses, same
order, same mask (§5 stands).

1. **`arch/x86_64/boot.asm`** — `global kernel_stack`. It was not exported;
   `boot.asm:19` declared `global kernel_entry` and nothing else. One directive,
   for exactly one reader.
2. **`kernel/proc/sched.c`** — `extern uint8_t kernel_stack[];`, and
   `init_idle()` takes the base as a **parameter** rather than leaving the
   `memset`'s 0. `base == 0` stays legal and still means *"not covered"*.
3. **`kernel/apic/smp.c` / `.h`** — `g_ap_stack_base[PERCPU_MAX]`, written in
   `smp_start_aps` **before the SIPI** (the allocation is out of scope by the
   time `init_idle` runs, and it runs *on the AP*), plus
   `smp_ap_stack_base(idx)`, which returns **0 for an unknown index** — the safe
   answer, degrading to "not covered" rather than to a window from a bogus base.
4. **`sched_free_tcb`** — see §7.1. This is the part that was not in the design.

### §7.1 — A coupling the design did not name, found by enumerating the readers

`sched_free_tcb` ends `if (t->kstack_base) kfree(...)`. **Giving an idle tcb a
real `kstack_base` arms that branch for idle threads — and neither idle stack
came from `kmalloc`:** the BSP's is `boot.asm`'s `.bss` `kernel_stack`, and each
AP's is a `pmm_alloc_pages(2)` block. `kfree()` on either hands the slab
allocator an address it never issued.

**No idle can reach that path today, and that was ENUMERATED rather than
assumed:** the reaper (`sched.c:2216`) selects only `THREAD_ZOMBIE`, which an
idle never becomes (`init_idle` sets `THREAD_RUNNING` and an idle never calls
`sched_exit`); and every `sched_destroy` caller — `sys_wait.c:75`, `sched.c:1270`
and `:2274`, `main.c:212/213/274/275/345/346/347/3387` — passes either a tcb it
created itself via `sched_create*` (which never returns an idle) or a zombie
child found by pid (idles have `pid == 0`).

**So nothing is broken and nothing is fixed here.** What changes is that a
property of the *callers* would silently become load-bearing for the *allocator*.
The guard is `!t->is_idle`, one clause, using a field both writers already
initialise explicitly — so §NON-NEGOTIABLE 10 does not arise, as it would for a
new field. It states the invariant where the `kfree` is, not in a comment three
files away.

### §7.2 — The keying was verified at every hop, not assumed

`g_ap_stack_base` is indexed by `i` from `smp_start_aps`'s loop. That `i` is
written to `OFF_MB_IDX` (`smp.c:384`), loaded into `edi` as SysV arg0 by
`ap_boot.asm:64`, arrives as `smp_ap_entry(idx)`, is handed to
`percpu_init_cpu(idx)` which sets `p->cpu_idx = idx`, and is read back by
`sched_ap_enter` as `this_cpu()->cpu_idx`. **Same index end to end** — checked by
reading each hop, because an AP reading a neighbour's entry would produce a
window that is wrong but plausible, which is the worst available failure.

### §7.3 — A stated comment correction, in the same commit as the change

DDR-1105's own comment in `schedule_locked` read: *"init_idle() memsets the whole
tcb and never assigns kstack_base, so EVERY idle thread ... has base 0 ... The
cost is a stated coverage limit: a corrupt rsp on an idle tcb is not covered."*
**This commit falsifies that sentence**, so it is corrected here rather than left
to be found — the DDR-1084 §1 / DDR-1107 §2 pattern, applied to a source comment
instead of a checklist row. The skip itself **stays**, because `base == 0` still
means "no window can be derived", which is the safe answer for any future tcb
arriving without one.

---

## §8 — PROOF: the same mutant, silent before and firing after

§4 said the only arm that convicts is a **caught idle frame**, run in both
directions. One line, inserted immediately before the check so it corrupts
exactly the value the check reads (`next->rsp -= 7` — the pointer wrong while
the frame stays put, DDR-1105 M2's shape), on four recorded hashes:

| mutant | tree | kernel | gate | `[schedcheck]` |
|---|---|---|---|---|
| **M1** `if (next->is_idle)` | **pre-fix (DDR-1105)** | `f2ec0c054b8020f9` | `smoke-shell` (1 CPU) | **0 — SILENT** |
| **M2** `… && next->on_cpu != 0` | **pre-fix (DDR-1105)** | `87c7e47874bfb997` | `smoke-smp` (`-smp 4`) | **0 — SILENT** |
| **M1** (identical line) | **fixed (DDR-1106)** | `59308c5c18bbf7fe` | `smoke-shell` (1 CPU) | **1 — FIRES** |
| **M2** (identical line) | **fixed (DDR-1106)** | `ca03bedf94effbbb` | `smoke-smp` (`-smp 4`) | **7 — FIRES** |

**The pre-fix tree is the control, not a synthetic defect** (the DDR-1066 M1/M2
shape): it is literally `c204ed0`'s four files, `git stash`'d back. Both pre-fix
runs still fail their gate (`rc=2`) — the machine took the corrupt frame and
died — **with nothing naming why.** That is DDR-1099 §6's "undiagnosable jump
into nowhere", and converting it into a named line is the entire deliverable.

### §8.1 — The printed `base=` proves BOTH mechanisms, separately

This is more than §4 asked for, and it is the part worth keeping:

- **M1-post (BSP):** `tid=0 rsp=0xFFFFFFFF80144C69 base=0xFFFFFFFF801411C0
  rflags=0x0000000000000000` — a **higher-half** base, i.e. `boot.asm`'s `.bss`
  `kernel_stack`. `rsp − base = 0x3AA9` = 15,017, inside `[0, 16384)`, and
  `rsp & 7 == 1`, so **clause 2** tripped and clause 3 did not.
- **M2-post (APs):** **three distinct low bases** — `0x07F9C000`, `0x07FA0000`,
  `0x07FA4000` — one per AP on a 4-CPU boot. They are **exactly 0x4000 apart and
  each 16 KiB-aligned**, which is the `pmm_alloc_pages(2)` allocation pattern,
  and every `rsp − base` (0x3901, 0x3549, 0x3901) lands inside **its own**
  window.

**That last fact is what proves the §7.2 keying, not an argument about it.** Had
an AP read a neighbour's entry, its rsp would fall outside that base's window and
**clause 3** would have tripped instead — a different clause, distinguishable in
the same printed line. Every fire tripped clause 2.

`rflags=0x0000000000000000` on every fire is the ordering claim measured again:
the RFLAGS slot is still at its initialiser because **clause 4 was never
reached** — alignment and bounds come first, which is what makes the load safe
(DDR-1079's defect, not repeated).

### §8.2 — The negative, and why it is the weaker half

On the clean kernel `0eb965428942d5cf`: `smoke-smp`, `smoke-smppreempt`,
`smoke-rqstress`, `smoke-blk-integrity` and `smoke-shell` are **all rc=0 with
ZERO `[schedcheck]` and ZERO `[apfreeze]`**, hash pinned and re-verified
identical after the last run (DDR-1060 §9). Four of those boot `-smp 4`, so
**four idle threads per boot are now watched** and none fired.

**§4 predicted exactly why that is not the proof**, and it holds: a base wrong in
the *forgiving* direction (left 0) would skip precisely as before and every gate
would still pass. The negative rules out the *strict* direction — where idle,
being switched to constantly, would have reddened every gate immediately — and
§8's mutants rule out the forgiving one. Neither alone would do.

### §8.3 — A refused derivation that the measurement would have permitted

§3 refused computing the AP base as `rsp & ~(STACK_SIZE - 1)` because it is
correct **only if** a buddy order-2 block is 16 KiB-aligned, which nothing states,
asserts or tests. §8.1's three bases are all 16 KiB-aligned, **so that derivation
would have worked on this boot.** It stays refused, and the measurement is the
reason the refusal is worth recording rather than an argument against it: what
was refused was *depending* on an allocator property, and one boot of one
allocator is not that property being guaranteed. A future allocator change would
have reintroduced false positives silently, on the hottest path.

### §8.4 — Revert, and a vacuity note about the size

Removing both mutants and rebuilding returns `0eb965428942d5cf` **bit-for-bit**,
verified by rebuild rather than assumed. And `kernel.bin` is **1,315,210 B — the
same size as DDR-1105's `3933fa5f60ae607c`**, the additions fitting inside
existing page padding. So the size/headroom pair and `ci-docstate-check` are
unaffected — **and a size comparison could not tell these two binaries apart at
all.** Only the hash discriminates, which is DDR-1097's finding arriving again.

---

## §9 — NOT CLAIMED (final)

- **NO FIX, and OPEN-2 does not move.** No cause is named (§NON-NEGOTIABLE 3)
  and no rate is claimed. What changes is the set of frames the *next*
  occurrence can be caught on.
- **NO defect is found and none is alleged.** `init_idle` leaving `kstack_base`
  at 0 was correct for a thread with no allocated stack; `smp_start_aps`'s
  `pmm_alloc_pages(2)` / `4 * PAGE_SIZE` pairing was verified against `pmm.h:23`
  and **agrees** (§2); `sched_free_tcb` was correct for every tcb that could
  reach it (§7.1). Nothing here reports a bug.
- **NOT exonerated in advance** (DDR-1042). This changes what happens on the
  hottest path for the most-switched-to thread in the system. If the OPEN-2
  signature moves, this commit is a candidate, and *"the diff is elsewhere"* is
  not an argument.
- **The check is UNCHANGED** — same four clauses, same order, same mask. So are
  `STACK_SIZE`, the window arithmetic, the halt-and-name behaviour,
  `GLOBAL_FORBIDDEN` (**77**, `[schedcheck]` already present) and the gate count
  (**179**). **No gate arm**, for DDR-1105 §8's reason unchanged: the triggering
  condition cannot be manufactured in product, and asserting the *absence* of a
  rare intermittent is unfalsifiable at any N this project can afford (DDR-1082
  costed that shape and refused it).
- **The `rsp & ~(STACK_SIZE-1)` derivation is REFUSED, not deferred** — and §8.3
  records that the measurement would have permitted it, which does not change
  the reason.
- **NOTHING is claimed about whether idle frames are ever actually corrupted.**
  That is the open question; covering the path is what makes it askable.
- **`kernel.bin` size is UNCHANGED** (1,315,210 B), so the size/headroom pair and
  `ci-docstate-check` are unaffected — and §8.4 records that a size check could
  not distinguish this binary from DDR-1105's at all.
