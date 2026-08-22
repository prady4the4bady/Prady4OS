# DDR-964 — OPEN-10 `rc=-1`: the create-then-init race on `->arg`

Status: ACCEPTED. Written before the code it governs (R16).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).

## 1. The trigger — a recurrence that carried something new

Commit `8184897` set a stopping rule: a recurrence of a characterised
intermittent gets written up **only** if it carries a different signature, a new
gate family, or evidence bearing on a root cause.

`aec6ad1`, shard 2, `smoke-percpu-sched` (check run 96856099938) failed with the
familiar OPEN-10 signature:

```text
[sfs] churn FAIL op=create iter=0 rc=-1
[sfs] btree churn FAIL
```

That alone would be a silent recurrence. But 40 lines above it, in the same
boot:

```text
[boot-load] PRISM.ELF t=158
[user] SFS write failed for PRISM.ELF
```

`[boot-load] PRISM.ELF` appears in 20+ retained logs under `build/gatelogs/`
(`blkint_*`, `bug1_serial_*`, `e961_serial_1..20`). **In none of them has that
SFS write ever failed.** Two independent SFS create/write failures in one boot,
one of them never before observed, is evidence bearing on root cause. The
stopping rule is cleared and this DDR is owed.

## 2. `rc=-1` is the capability branch — already established, re-confirmed

DDR-888 split `vfs_create`'s precondition failure from the driver's return
(`vfs.c:146-163`):

```c
if (!cap_ok(cap, CAP_FS_WRITE)) return -EPERM;   /* EPERM == 1, so this IS -1 */
if (!m || !m->fs->create)       return -EINVAL;  /* -22 */
```

`-EPERM` is the only branch in the whole path that yields exactly `-1`.
So the churn probe's capability did not authorize. That is the fact to explain.

## 3. `iter=0` is not a coincidence — it is the loop telling us *when*

Two captures, both `iter=0`. Under a uniform per-iteration race across 40
iterations that is ~1/1600, which would be reason to distrust a race hypothesis.

It is not a coincidence. The loop is (`main.c:2156`):

```c
for (int i = 0; churn_ok && i < 40; i++) {
```

`churn_ok` is cleared by the first failure, so the loop **stops at the first
failure**. Any failure whatsoever reports the lowest iteration reached.
`iter=0` therefore does not mean "the first create is special" — it means
**the capability was already bad before the loop started**, and stayed bad.

This eliminates every *transient* mechanism (a migration window inside
`cap_ok`, a momentarily stale `this_cpu()->current`). Those would fail at a
random iteration. The handle is bad from the thread's first instruction.

## 4. Where the handle comes from

`fs_test_thread` reads its capability from its own argument (`main.c:1084`):

```c
cap_t cap = (cap_t)(uintptr_t)arg;
```

and `thread_trampoline` supplies it (`sched.c:659`):

```c
current_thread->entry(current_thread->arg);
```

The spawn site is `main.c:2411-2415`:

```c
struct tcb *fst = sched_create(fs_test_thread, 0, "fs");
if (fst)
    fst->arg = (void *)(uintptr_t)cap_create(fst->caps, RES_FILE, FS_RES_ID, ...);
```

The capability is minted into `->arg` **after** `sched_create` returns. The
guard is four lines up (`main.c:2399-2401`):

```c
/* Create the demo threads with interrupts masked so each thread's capability
 * (->arg) is fully set before the timer can schedule it. */
__asm__ volatile("cli");
```

**That comment is single-CPU reasoning.** `cli` masks the BSP's own timer. It
says nothing about the other three CPUs.

## 5. The race

`sched_create` → `sched_create_state(..., THREAD_READY)`, which ends
(`sched.c:938-942`):

```c
if (initial_state == THREAD_READY) {
    struct percpu *pc = this_cpu();
    rq_push(pc ? (int)pc->cpu_idx : 0, t);
}
return t;
```

The thread is on a run queue **before `sched_create` returns** — before the
caller has written the capability into `->arg`. Under `-smp 4` any other CPU may
take it from there via `rq_steal` and enter `thread_trampoline` first.

What it reads is the `arg` the create was called with: `sched_create_state` does
`t->arg = arg` (`sched.c:882`), and all eight sites pass `0`. So the thread
starts with `cap == CAP_NULL` — a well-defined zero, not stack residue. (§10
records this as a correction: the first draft of this DDR predicted uninitialised
garbage per §0.6, and the reproduction refuted it.)

`resolve` (`cap.c:41-53`) rejects it on `!s->in_use`, so `cap_authorize` returns
0, so `vfs_create` returns `-EPERM`, **forever, from the thread's first create**.
That is the signature exactly.

### The counter-evidence that looked decisive and was not

The failing log shows `[sched] steal local=796 remote=0`, which reads as "no
cross-CPU stealing happened". It does not. In `steal_pass` (`sched.c:579-601`)
`same` compares **NUMA nodes**, not CPUs:

```c
for (int c = 0; c < PERCPU_MAX; c++) {
    if (c == self || !g_rq[c].head) continue;   /* every steal is cross-CPU */
    ...
    if (same) g_steal_local++; else g_steal_remote++;
}
```

Every steal counted is from another CPU's queue. QEMU presents one NUMA node, so
`remote=0` is the expected reading and `local=796` means **796 cross-CPU steals
in that boot**. The counter-evidence inverts: it confirms the exposure.

## 6. The codebase already solved this — for user threads only

`sched_create_state`'s own header (`sched.c:838-842`):

> READY for kernel threads (arg-in-insert — no post-init) and BLOCKED for user
> threads (the caller sets cr3/user_rip/authority, THEN `sched_unblock` —
> closing the create-then-init race against a second scheduling CPU;
> DDR-SMP-3c-cap-2a D3).

The race is known and the remedy is established: **create BLOCKED, finish
initialising, then `sched_unblock`.** The kernel-thread path assumed
"arg-in-insert — no post-init", and eight call sites break that assumption.

## 7. Every exposed site

All in `main.c`, all minting a capability into `->arg` after `sched_create`:

| Site | Thread |
|---|---|
| 211, 212 | `ipc_demo` — recv, send |
| 267, 268 | `ring_demo` — prod, cons |
| 331, 332, 333 | `bus_demo` — a, b, publisher |
| 2413 | `fs_test_thread` — the OPEN-10 probe |

## 8. A second, independent defect found in the same read

`cap_table_create()` is a `kmalloc` and returns NULL on failure. At
`sched.c:885` the result is **unchecked**:

```c
t->caps = cap_table_create();
```

A thread with `->caps == NULL` runs normally, and every `cap_authorize` against
a NULL table returns 0 — the same `-EPERM`/`-1` outcome, silently and
permanently. `sizeof(struct cap_table)` is 1536 B, at or under `MAX_SLAB_OBJ`
(2048), so it is a slab allocation that fails when the slab cannot grow.

This is the §0.6 class (unchecked allocation result) and is a defect by
inspection whether or not it is OPEN-10's mechanism. It is **not** the likely
cause of the captured failures: at `main.c:2411` the heap is young, and the
16 KiB kernel-stack allocation immediately before it succeeded, so a 1536 B
slab allocation failing there is implausible. It is fixed because it is wrong,
and it is recorded separately because conflating it with §5 would be exactly the
§6.0-C error.

## 9. What ships, and what is *not* claimed

Ships:

1. `sched_create_blocked()` — kernel-thread create that returns BLOCKED, so the
   caller can finish `->arg` before any CPU can run it. All eight sites in §7
   convert to create → init → `sched_unblock`.
2. `cap_table_create()` NULL checked at `sched.c:885`; the create fails the way
   the kernel-stack allocation above it already does, and callers already print
   a loud marker on NULL (DDR-949).
3. A failure-path-only instrument at the churn site printing the raw handle
   (index and generation) and the running thread's tid. No writable globals
   (DDR-826); no hot-path volume (DDR-790).

**Not claimed:** that OPEN-10 is now closed. OPEN-10 is intermittent in CI and
has never reproduced locally on demand, so this change cannot be proven to fix
it by a local green. What §5 establishes is that the race is *real* — read off
the code and confirmed live by `local=796` — and that it produces precisely the
observed signature. Closure requires the CI evidence in §10.

## 10. Reproduced on demand, locally, for the first time

OPEN-10 had only ever been seen in CI. It has now been reproduced deliberately.

Mutation: revert the `fs_test_thread` spawn to `sched_create` (READY on return)
and widen the window between create and mint with a spin delay. Nothing else
changed; the instrument stayed in.

```text
[sfs] churn FAIL op=create iter=0 rc=-1 h=0 idx=0 gen=0 tid=11
[sfs] btree churn FAIL
```

That is the CI signature exactly — `op=create iter=0 rc=-1` — produced on
demand. Reverting the mutation restored a kernel byte-identical to the fixed
build (`sha256:c5f76441babbaf91`), and `smoke-percpu-sched` then ran green with
`[sfs] btree churn OK` in **3/3** runs that reached the churn block (plus the
first post-fix run: 4 total).

The mechanism in §5 is therefore measured, not merely read.

### A correction this measurement forced

The handle printed `h=0`, not garbage. §5 as first drafted predicted garbage
from an unzeroed TCB, but `sched_create_state` writes `t->arg = arg`
(`sched.c:882`) and every one of the eight sites passes `arg = 0`. A
stolen-early thread therefore reads a well-defined `CAP_NULL`, not stack
residue. The §5 race and the §8 NULL-table path produce the **same** printed
handle, so `h=0` alone does not separate them.

What separates them is §9.2's marker: the §8 path now prints
`[fs] FAILED to spawn fs_test_thread` or `[fs] FAILED to mint fs capability`
before any churn line. So:

| Evidence | Reading |
|---|---|
| `h=0` **with** a `[fs] FAILED to …` marker | §8 — allocation failure |
| `h=0` **without** that marker | §5 — the create-then-init race |
| well-formed handle, still refused | neither; cap table or `current_thread` is wrong — new DDR |

The reproduction above showed `h=0` with no `[fs] FAILED` marker, which is the
§5 reading.

## 11. What would still reopen this

The fix is proven against the reproduction, not against CI's intermittent. Two
things would reopen it:

- the signature recurring **with** a `[fs] FAILED to …` marker → §8 is live
  after all, and the heap, not the race, is the story;
- the signature recurring with a well-formed handle → a third mechanism.

Closure is the promotion requirement already in force: three consecutive CI
greens on the same tip (§3). Until then OPEN-10 stays open in
`docs/build_status.md`, with this DDR as its diagnosis.

## 12. Audit — the eight sites were the complete set

A fix for one instance of a race class is worth little if siblings remain, so
the whole kernel was swept for the pattern: a thread created READY (runnable on
return) whose fields are then written by the caller.

Method: for every `X = sched_create…(…)` in `kernel/**/*.c`, look ahead 30 lines
for an assignment `X->field =`, excluding comparisons (`==`, `!=`, `<=`, `>=`)
and excluding creates that already return BLOCKED (`sched_create_blocked`,
`sched_create_user`, `sched_create_user_clone`).

Result: **clean** — after this change no READY-created thread has any field
assigned after creation. The eight sites in §7 were the complete population.

One near-miss is worth naming because a careless sweep flags it: `main.c:624`
is `while (g_cw_thread->state == THREAD_BLOCKED …)`, a comparison, not an
assignment. A regex matching `->\w+\s*=` reports it as a write on a running
thread — a false positive, and a reminder to exclude comparison operators before
believing such a list.

The sweep covers direct writes through the returned pointer. It does not prove
the absence of initialisation performed indirectly (passing the fresh pointer to
a helper). That path is already governed for the one caller that uses it:
`user_boot_from_sfs_rooted` sets `->root_mnt` *before* unblocking, internally,
and DDR-957 records why a caller assigning it on the returned pointer would race
(`main.c:1831`).
