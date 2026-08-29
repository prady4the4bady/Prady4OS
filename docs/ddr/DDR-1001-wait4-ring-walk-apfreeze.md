# DDR-1001 — `[apfreeze]` is back: an unlocked ring walk in `sys_wait4`

**Status:** FIXED, gated, M1 mutation-checked. Kernel `60b35c96d70253f5`.
**Reopens:** OPEN-2 / B#3, which DDR-981 closed. CLAUDE.md's own instruction is
*"Reopen on the first `[apfreeze]` line in CI."* This is that line.
**Distinct from DDR-981:** same *symptom*, different *site* — this path never
calls `yield()`, so DDR-981's fix cannot apply to it.

---

## 1. The artefact

`smoke-smpuser`, shard 5, run 33239100401, commit `0c22334`:

```
[apfreeze] cpu=1 ticks=342 rip=0xFFFFFFFF8000A39E cs=0x08 rflags=0x02 if=0
  rsp=0x07D97CC0 lvt=0x00020030 masked=0 svr=0x1FF swen=1 tpr=0
  isr48=0 irr48=1 pid=48 shot=1
  bt=0xFFFFFFFF8000027A,0xFFFFFFFF80019C4F,0xFFFFFFFF80016608,0xFFFFFFFF8000035A
```

The register block is DDR-981's signature exactly: **LVT unmasked** (`masked=0`),
**LAPIC enabled** (`swen=1`), **no stuck in-service vector** (`isr48=0`), **timer
pending and undelivered** (`irr48=1`), **not blocked by priority** (`tpr=0`).
`if=0` is the only remaining blocker. The CPU is not halted and not starved — it
is running with interrupts masked and cannot be preempted.

## 2. Where, resolved against the right binary

§NON-NEGOTIABLE 18 first: an address does not identify a binary. `0c22334` is
the last commit touching `kernel/`, `user/`, `arch/`, `boot/` or the lwIP port —
`git diff --name-only 0c22334..HEAD` over those paths is **empty** — so the local
`build/kernel.elf` (`5349db4d791cc2ab`) is that build.

| frame | symbol |
|---|---|
| `RIP  0x…8000A39E` | `isr_dispatch + 0xc0e` (the NMI probe itself) |
| `bt[0] 0x…8000027A` | `isr_common.gs_kernel_in + 0x8` |
| `bt[1] 0x…80019C4F` | **`sys_wait4 + 0x4f`** |
| `bt[2] 0x…80016608` | `syscall_dispatch + 0x128` |
| `bt[3] 0x…8000035A` | `syscall_entry + 0x8a` |

And `+0x4f` is precise, not approximate:

```
ffffffff80019c4a: callq  0xffffffff80019d50 <find_zombie_child>
ffffffff80019c4f: movq   %rax, -0x58(%rbp)          <-- bt[1]
```

`+0x4f` is the instruction *after* the call, i.e. a **return address**. The CPU
was **inside `find_zombie_child`**.

## 3. What `find_zombie_child` does

```c
struct tcb *t = parent->next;
while (t != parent) {
    ...
    t = t->next;
}
```

An **unbounded walk of the all-threads ring**, and `sys_wait.c` takes **no lock
anywhere** — `grep -n 'spin_lock\|g_sched_lock\|irq_save' kernel/syscall/sys_wait.c`
returns nothing but comment text.

Meanwhile the file's own header says, deliberately:

> "Runs with IF clear (SYSCALL entry), so the check-then-block sequence cannot
> lose a wakeup."

That reasoning is sound for the *wakeup* race it was written about. It also means
that if the walk fails to terminate, **nothing can ever interrupt it**.

## 4. The race — and a correction I nearly published

My first reading was that `sched_destroy`'s unlink is guarded only by
`irq_save()`, i.e. local IRQ disable, giving no cross-CPU exclusion. **That is
wrong**, and checking rather than asserting is what caught it:

```c
static inline uint64_t irq_save(void) { return spin_lock_irqsave(&g_sched_lock); }
```

It is a **real spinlock**. The writer is correctly serialized.

The defect is the other side: **the writer takes `g_sched_lock`, the reader takes
nothing.** A lock only excludes participants who take it. So CPU A can be walking
`->next` in `find_zombie_child` while CPU B relinks the ring under
`g_sched_lock` in `sched_ring_unlink`:

```c
struct tcb *p = t->next;
while (p->next != t) p = p->next;
p->next = t->next;              /* the store the reader can race */
```

A reader that observes the ring mid-relink can follow a pointer that no longer
leads back to `parent`, and then walks forever.

## 5. Established vs. hypothesised — kept apart on purpose

**Established by the artefact:**
1. The NMI caught CPU 1 inside `find_zombie_child` (return address, disassembled).
2. That walk is unbounded and takes no lock.
3. `if=0` with the timer pending, so no preemption could break the loop.
4. `ticks=342` — the ring holds at most tens of threads. This is **stuck, not slow**.

**Hypothesised, NOT established:**
- That a *concurrent relink* is what derailed this particular walk. There is no
  capture of the ring's state at the time, so the mechanism is inferred from the
  code, not observed. A stale `->next` from some other cause would look identical.

Recording the split because DDR-985's Claim A was written by not making it.

## 6. Why this is not DDR-981 recurring

DDR-981 fixed `yield()` — the choke point shared by `mnt_lock`, both pipe waits,
the blocking console read and `sys_yield`. **`sys_wait4` never calls `yield()`**;
it calls `sched_block()`, and the freeze is not in the block path at all but in
the *scan* that precedes it. DDR-981's fix is correct and unaffected; its
detector did its job by naming a second, independent site.

So `[apfreeze]` is doing exactly what CLAUDE.md's Group A row hoped for after
`smoke-smp`/`smoke-rqstress` measured 20/20 while B#3 was live: *"the GATES DID
NOT CATCH IT; the only evidence was `[vblk] compl wait timeout` sitting in a
serial log nobody asserted on."* This time the sentinel caught it.

## 7. The fix, and why it is not in this commit

Two changes, in increasing order of risk:

1. **Bound the walk.** More iterations than the maximum possible thread count
   means the ring is inconsistent; report it (a named sentinel) and return
   `-ECHILD` rather than spinning forever with IF clear. This converts a silent
   unbreakable hang into a bounded, attributable failure — the DDR-994 shape.
2. **Make the reader take `g_sched_lock`**, so the ring walk is serialised
   against relinks. Correct, and the larger change: it puts a lock acquisition on
   a syscall path, and `sys_wait4` currently relies on IF-clear semantics for its
   wakeup argument (§3), which interacts with `spin_lock_irqsave`.

Neither is implemented yet, and the reason is mechanical rather than a judgement
call: the OPEN-1 E1 campaign (DDR-1000 §5) currently owns the machine's only QEMU
(§NON-NEGOTIABLE 12), and `campaign_chunk` rebuilds the kernel on every run — so
editing `kernel/` right now would silently change the kernel under a 60-run
campaign, which is the mistake already recorded once in this session. The fix
lands when the campaign completes, with its own gate and a mutation check.

## 8. Not claimed

That this is OPEN-1. OPEN-1 route 1 is a **hang in `sys_read`/`vfs_read` with no
output at all**; this is a hang in `sys_wait4` that *announces itself* through
`[apfreeze]`. Different syscall, different path, and this one has a detector that
fired. They may share the "unbounded unlocked walk" shape — `mnt_lock` is DDR-994's
candidate for route 1 — but a shared shape is not a shared defect, and treating
it as one is the colour-matching this file's neighbours keep warning about.


---

## 9. The fix — and the project already had the right pattern

§7 planned "bound the walk, then lock the reader". Reading further changed the
order and the emphasis, for the better: **`sched_snapshot` already walks this
exact ring under `g_sched_lock`** (`sched.c`, for `SYS_GETPROCS`). The project
knows how to traverse the all-threads ring safely; `sys_wait4` simply wasn't
doing it.

So the fix is not a new invention, it is making wait4 follow the existing
convention:

* `find_zombie_child` is **deleted from `sys_wait.c`**.
* `sched_find_child()` replaces it **in `sched.c`**, walking under
  `irq_save()` (= `spin_lock_irqsave(&g_sched_lock)`), exactly as
  `sched_snapshot` does.
* The walk had to move files at all because `g_sched_lock` is `static` to
  `sched.c` — `sys_wait.c` could not have taken it where it was.

The bound (`WAIT4_RING_MAX = 1024`, ~10x any observed live-thread count) stays,
but its role is now belt-and-braces rather than the fix: under the lock the ring
is consistent and the walk terminates. If it ever doesn't, the ring is corrupt
for some other reason, and `[ringwalk] wait4 ring inconsistent pid=<n>` reports
that instead of wedging a CPU.

### 9.1 M1 — mutation-checked

| build | kernel | result |
|---|---|---|
| fixed | `60b35c96d70253f5` | `smoke-smpuser` PASS, `smoke-shell` 5/5 PASS |
| **M1** — `WAIT4_RING_MAX = 1` | `8cb987c18ddebb17` | `smoke-smpuser` **FAIL**, and the sentinel fires: `[ringwalk] wait4 ring inconsistent pid=39` |

Distinct hashes both ways. The detector is live and reports; it is not
decoration.

### 9.2 `[ringwalk]` added to `GLOBAL_FORBIDDEN`

Deliberately, and with the DDR-994 lesson in mind. `[yieldstall]` was *removed*
from that list because a resolved stall is survivable and it reddened four shards
on a signal never shown to be fatal. `[ringwalk]` is different: under the lock,
exceeding a bound set at ~10x any observed thread count cannot happen unless the
ring is genuinely corrupt. It is fatal by construction, so a recurrence should
name itself rather than hide.

### 9.3 What is NOT fixed, and is not claimed to be

The pointer `sched_find_child` returns is used **after** the lock drops —
`live->waiter = self`, `child->exit_status`, `sched_destroy(child)`. A concurrent
reap could still free it in that window. That lifetime question is **pre-existing,
separate, and untouched here**; this change stops the CPU wedging, which is what
the artefact showed.

Nor is the *race* itself mutation-checkable by a deterministic gate: the failure
needed a concurrent relink at a precise moment and appeared once in CI across
many runs. M1 proves the bound and its sentinel work; the lock is justified by
matching `sched_snapshot`'s established pattern, not by a reproduction. Saying
which of the two is demonstrated and which is reasoned matters more than the
green result.
