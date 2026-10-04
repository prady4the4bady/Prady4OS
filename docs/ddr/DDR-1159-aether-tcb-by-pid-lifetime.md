# DDR-1159 — AETHER `tcb_by_pid`: find + act under g_sched_lock (Fix 3)

Operator-approved, PR #27 comment 5977642031 ("Fix 3 = audit F2"). This is the
unfixed sibling of DDR-1157 (Fix 2) and mirrors its helper pattern exactly. Audit
item F2 is `docs/AUDIT_LIFETIME_2026-10.md` §F2.

## §1 Defect (confirmed from source)

`kernel/syscall/sys_aether.c` has a file-local lookup that returns a **raw
`struct tcb *` from an UNLOCKED ring walk**, and three callers that then use that
pointer:

```c
static struct tcb *tcb_by_pid(uint32_t pid) {   /* sys_aether.c:42 — NO lock */
    struct tcb *t = current_thread;
    do {
        if (t->is_user && t->pid == pid &&
            t->state != THREAD_ZOMBIE && t->state != THREAD_DONE)
            return t;
        t = t->next;
    } while (t != current_thread);
    return 0;
}
```

The callers (every one, enumerated — operator's request):

1. **`sys_kill_agent` (`:320`, NSI 36, CAP_AGENT) — a WRITE, the sharpest.**
   `t = tcb_by_pid(a1); if (!t) return -ESRCH; t->sig_pending |= (1ull<<SIGKILL);`
   The `|=` lands in the found TCB after the lookup returns.
2. **`sys_agent_metrics` (`:276`, NSI 64) — READS.** `t = tcb_by_pid(pid);`
   then reads `t->run_ticks`, `t->dispatches`, `t->state`, `t->mem_used`.
3. **`roster_active` (`:55`, used by NSI 53) — the UNLOCKED WALK ALONE.** It only
   tests `tcb_by_pid(pid) != 0`, no field deref — but the *walk itself* is the
   hazard here (see below), not a dereference.

This is the **same SMP use-after-free class as DDR-1157 / DDR-996**, reached
through the AETHER syscalls instead of checkpoint/resume:

- **The dereference after the lookup (callers 1-2).** The reaper frees a TCB as
  `irq_save(); sched_ring_unlink(t); irq_restore(fl); sched_free_tcb(t);`
  (`reaper_thread`, and `sched_destroy`) — the unlink is under `g_sched_lock`,
  the `kfree` is **outside** it. Between `tcb_by_pid` returning `t` and the
  caller writing `t->sig_pending` / reading `t->run_ticks`, another CPU can
  unlink and free `t`. The write or read then lands in freed, `PMM_POISON`-
  stamped memory. `sys_kill_agent`'s `|=` is a **write into freed memory** — the
  worst of the three.
- **The walk itself (all three, incl. `roster_active`).** `tcb_by_pid` follows
  `->next` with no lock while the reaper relinks that ring under `g_sched_lock`
  (`sched_ring_unlink`, caller "MUST hold g_sched_lock"). A walk racing an unlink
  can follow a `->next` mid-rewrite. So even the non-dereferencing
  `roster_active` must move under the lock.

Confirmed by reading `sys_aether.c`, `tcb_by_pid`, `sched_ring_unlink`,
`sched_destroy`, `reaper_thread` and `sched_free_tcb` directly. No reproduced
crash is claimed — see §6.

## §2 Why holding g_sched_lock across find + act closes it

Identical to DDR-1157 §2. The free is **preceded by `sched_ring_unlink` under
`g_sched_lock`**, so while a critical section holds `g_sched_lock`:

- nothing can be unlinked (the unlink needs the lock we hold);
- a TCB still in the ring is not yet unlinked, hence not yet freed (free runs
  only after the unlink, serialised by the lock);
- a TCB already unlinked is off the `->next` ring, so the walk never returns it.

A find + use performed entirely inside one `g_sched_lock` critical section only
ever touches a live, unfreeable TCB — exactly what the operator prescribed ("do
the lookup and the act … under `g_sched_lock`, using the same helper pattern as
Fix 2").

## §3 The fix

Three helpers in `sched.c` (because `irq_save`/`irq_restore` — the `g_sched_lock`
acquire/release wrappers — are file-local to `sched.c`, as DDR-1157 established),
each holding `g_sched_lock` across the whole find + act. `find_user_pid_locked`
is `tcb_by_pid`'s body (the `is_user` predicate kept) called **with the lock
held**:

```c
static struct tcb *find_user_pid_locked(uint32_t pid) { /* lock HELD by caller */
    if (pid == 0) return 0;
    struct tcb *t = current_thread;
    do {
        if (t->is_user && t->pid == pid &&
            t->state != THREAD_ZOMBIE && t->state != THREAD_DONE)
            return t;
        t = t->next;
    } while (t != current_thread);
    return 0;
}

int sched_user_pid_alive(uint32_t pid) {                 /* roster_active     */
    uint64_t fl = irq_save();
    int alive = find_user_pid_locked(pid) != 0;
    irq_restore(fl);
    return alive;
}

int sched_user_pid_snap(uint32_t pid, struct sched_user_snap *out) { /* metrics */
    uint64_t fl = irq_save();
    struct tcb *t = find_user_pid_locked(pid);
    int rc = -ESRCH;
    if (t) {
        out->run_ticks  = t->run_ticks;  out->dispatches = t->dispatches;
        out->mem_used   = t->mem_used;   out->state      = t->state;
        rc = 0;
    }
    irq_restore(fl);
    return rc;
}

int sched_signal_user_pid(uint32_t pid, int sig) {       /* sys_kill_agent    */
    uint64_t fl = irq_save();
    struct tcb *t = find_user_pid_locked(pid);
    int rc = -ESRCH;
    if (t) { t->sig_pending |= (1ull << sig); rc = 0; }
    irq_restore(fl);
    return rc;
}
```

`struct sched_user_snap { uint64_t run_ticks, dispatches, mem_used; uint32_t state; };`
is declared in `sched.h`. `sys_aether.c` calls these three helpers; `tcb_by_pid`
is **deleted** (no remaining caller — a dead static would fail `-Werror
-Wunused-function`). Every other line of each syscall — the CAP check, the
`THREAD_BLOCKED → 2` state mapping, the retained-counts dead-slot branch, the
audit records — stays in the syscall layer, unchanged. The `state` mapping is
kept in `sys_aether.c` (`snap.state == THREAD_BLOCKED ? 2 : 1`), not pushed into
the helper, so the policy stays where it belongs.

### §3.1 Relation to DDR-1157's `find_pid_locked`

DDR-1157 (on `dev/phase1-seyp3n`, PR #29) adds a `find_pid_locked` **without**
the `is_user` predicate, for checkpoint/resume. This DDR adds a *separate*
`find_user_pid_locked` **with** it, because the AETHER callers select only user
threads (an agent pid is always a user process; a kernel thread must never match
`sys_kill_agent`). The two statics have different names and predicates, so when
both Fix 2 and Fix 3 reach `dev/phase1` there is no symbol collision and no
behavioural overlap. The minor walk-body duplication is deliberate — merging them
behind a predicate flag would couple two independently-approved fixes.

### §3.2 Calling under the lock is safe

`sys_kill_agent`/`metrics`/`roster` do no blocking or rq work inside the act;
they set a bit or copy scalars. No `sched_unblock` is called here (unlike
DDR-1157 §3.1), so there is not even a leaf-lock ordering question. Hold time is
trivial and these are cold observability/operator paths.

## §4 Gate — a NEW gate is justified here

`smoke-killagent` (new; `user/killagenttest.c`), because **NSI 36
(`SYS_KILL_AGENT`) has NO existing probe or gate** — `grep -rn KILL_AGENT user/
Makefile tools/ci/gate_shards.txt` is empty. This is *not* the DDR-1039/1070 "a
second gate booting an OS to drive the same syscall buys nothing" case that let
DDR-1157 reuse `smoke-checkpoint`; there is zero coverage of the write path to
reuse. (`sys_agent_metrics` and `roster_active` keep their existing coverage —
`smoke-agentmetrics`, `smoke-agents` — which the refactor leaves green.)

One ELF, two roles self-identified by the `SYS_KILL_AGENT` return code (the
`ckpttest.c` pattern):

- **controller (sovereign)** — `SYS_KILL_AGENT(1,…)` returns `-ESRCH` (init pid 1
  is not `is_user`, so `find_user_pid_locked` returns NULL). Drives the arms.
- **target KA_T (plain user)** — `SYS_KILL_AGENT(1,…)` returns `-EPERM` (no
  CAP_AGENT). Asserts that denial (`PRADYOS_KILLAGENT_DENY_OK`), then spins
  (compute + `SYS_YIELD`) forever so it can be found alive and killed.

Arms (controller): **A1** an unknown pid → exactly `-ESRCH`; **A2** find KA_T by
name (`SYS_GETPROCS`), `SYS_KILL_AGENT(tpid)` → `0`, then poll `ps` until KA_T is
gone or `THREAD_ZOMBIE/DONE` — asserting on observed **state**, never elapsed
time (DDR-837 gate discipline). Success prints `PRADYOS_KILLAGENT_OK`; any
failure prints `KILLAGENT FAIL: <why> rc=<n>` and exit 1.

## §5 Mutation check (two mutants — both arms shown non-vacuous)

This gate is new and asserts a **write**, so both its arms are proven, not just
one (stronger than DDR-1157, which reused a proven gate).

- **M-ESRCH (discriminator for A1).** `int rc = 0;` instead of `-ESRCH` in
  `sched_signal_user_pid` when the pid is absent. A1 fails: `KILLAGENT FAIL: kill
  of unknown pid was not -ESRCH rc=0`, exit 2. Proves A1 is non-vacuous for the
  `-ESRCH` contract (a helper silently reporting success for a pid it never found
  would pass a gate that did not check this). Mirrors DDR-1157 M2.
- **M-WRITE (discriminator for A2).** Drop the `t->sig_pending |= (1ull<<sig)`.
  KA_T is never signalled, never dies; A2's poll exhausts its bound and fails:
  `KILLAGENT FAIL: target still alive after kill`, exit 2. Proves A2 actually
  exercises the kill *write path* reaching a live thread, not merely the `0`
  return.

Each mutant is one line, built on top of the shipped Fix-3 kernel; reverting each
returns the shipped kernel bit-for-bit (hashes recorded in the PR). Baseline Fix-3
kernel passes `smoke-killagent` exit 0; each mutant fails it exit 2 on its own arm.

### §5.1 Regression (scheduler-touching change)

The fix adds a lock-free ring-walk helper and three `g_sched_lock` critical
sections to `sched.c` and reroutes three AETHER syscalls, so the scheduler and
block paths are re-run on the shipped kernel, one QEMU at a time
(NON-NEGOTIABLE 12): `smoke-shell` 5/5 (global-forbidden scan clean, 77
patterns), `smoke-blk-integrity`, `smoke-blkmq`, `smoke-rqstress-liveness`,
`smoke-agents`, `smoke-agentmetrics` — all rc=0. Static: `ci-shard-check`,
`ci-probe-rodata-check`, `ci-start-align-check` OK. (Exact kernel hash, size and
gate count recorded in the PR body once built — the new probe ELF changes the
size and the gate count by one.)

## §6 Not claimed — stated plainly

**The gate does NOT reproduce the UAF, and no gate can, deterministically — the
race is strictly SMP**, identical to DDR-1157 §6. On a single CPU there is no
yield between `tcb_by_pid` returning and the dereference, so no reaper free can
run in that window; `smoke-killagent` runs single-CPU and would pass a
lock-removed mutant. So:

- The UAF closure rests on the **structural argument in §2** (free is preceded by
  an unlink under `g_sched_lock`; the helpers now hold that lock across find +
  act), confirmed by reading the reaper and free paths — not on a reproduced-
  then-fixed artefact. The operator approved Fix 3 on exactly that read.
- **No rate is claimed.** The window needs the sovereign/agent to kill (or read
  metrics for) a pid at the instant that thread is being reaped on another CPU.
- **U1, U2, U3, OPEN-1 and OPEN-2 do NOT move**; no mechanism is named for any of
  them. This removes the AETHER `tcb_by_pid` UAF and nothing more.

## §7 Other callers / similar unlocked ring walks (operator's request)

Every caller of `tcb_by_pid` is handled above (the three in §1). Beyond it:

- **`sys_kill` (POSIX, `kernel/proc/signal.c:33`, NSI 23) — THE SAME DEFECT,
  NEWLY FOUND, NOT IN THIS FIX'S SCOPE.** It walks `current_thread->next`
  unlocked, finds `is_user && pid`, then does `target->sig_pending |= (1ull<<sig)`
  — byte-for-byte the same unlocked-walk-then-WRITE UAF as `sys_kill_agent`, and
  **more exposed**: it is reachable by *any* ring-3 process (no CAP gate;
  `user/killtest.c` drives it), where `sys_kill_agent` needs CAP_AGENT. **This
  was missed by `AUDIT_LIFETIME_2026-10.md` §F2** (which named only the AETHER
  callers) — recorded here honestly as a gap in that audit. It is **not fixed in
  this PR**: Fix 3 was approved as "`tcb_by_pid` in `sys_aether.c`", and the
  operator's process (own branch / own DDR / own PR per issue; "treat as PROPOSED
  and report before changing kernel behaviour") means an out-of-scope kernel
  change needs its own approval. The `sched_signal_user_pid(pid, sig)` helper
  this DDR adds is deliberately general (takes `sig`, not hard-wired SIGKILL)
  **so that a future approved fix closes `signal.c:33` by rerouting one line** —
  `return sched_signal_user_pid(pid, sig);` — with no new helper. Flagged to the
  operator as a proposed "Fix 5".
- **`sched_find_pid` (`sched.c:2504`) — Fix 2's subject (DDR-1157).** Its
  checkpoint/resume callers are rerouted by DDR-1157. Its other callers are the
  `main.c` bounded poll loops (`:2250`, `:2819`, `:3480`:
  `while (sched_find_pid(pid) && g_ticks < dl)`) which **only test non-NULL** and
  never dereference — the `roster_active` class (racy walk, no deref). Out of
  scope here; noted for completeness.
- **`sys_wait4` (`find_zombie_child`) — examined SAFE** (`AUDIT_LIFETIME` §
  examined-safe): its ring walk runs *under* `irq_save()` already, so it does not
  share this defect.

No other unlocked `->next` ring walk that dereferences or writes a found TCB
exists in `kernel/syscall/` or `kernel/aether/` (`grep -rn` for `->next` ring
walks returns only `tcb_by_pid`, now removed, besides the locked/noted cases
above).

## §8 DDR number

DDR-1159 verified free in `docs/ddr/` and `docs/decisions/` on `dev/phase1` and
on `dev/phase1-seyp3n` (where DDR-1154..1158 are consumed), and absent from every
ref (`git log --all`) — NON-NEGOTIABLE 8. The CLAUDE.md DDR-free-range carriers
(§INV.4) live on `dev/phase1-seyp3n`; their reconciliation to 1161+ is left to
the branch that owns the current free-range text, to avoid a cross-branch
CLAUDE.md conflict on this focused fix branch. Fix 4 takes DDR-1160.
