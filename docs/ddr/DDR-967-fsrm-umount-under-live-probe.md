# DDR-967 — FSRM: the SFS root is umounted (and reformatted) under live ring-3 probes

Status: ACCEPTED. Written before the code it governs (R16).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§0.4).
Implements the fix designed in `docs/build_status.md` ("FSRM root-caused" and
its two addenda).

## 1. The defect

`fs_test_thread` spawns ring-3 probes rooted at the SFS mount `smnt`:

| site | probe | gating |
|---|---|---|
| `main.c:1922-1928` | `FSRMTEST` | always |
| `main.c:1953-1961` | `FTRUNC` | `probe_enabled("ftruncate")` |
| `main.c:1970-1977` | `RENAMESFS` | `probe_enabled("rename-sfs")` |

Each does `->root_mnt = smnt; sched_unblock(...)`, so it is runnable from that
moment. Then, **further down the same thread**, `main.c:2093-2095`:

```c
/* Slice 4g: journal abort/commit/crash-replay (destructive —
 * reformats the disk, so release the VFS mount first). */
vfs_unmount(smnt);
```

The comment is accurate and that is the problem: the self-test **reformats the
volume**, and nothing waits for the probes that are rooted on it.

Observed in a passing log — mount ctx `0x7C48000` (line 170) → fsrm spawned
(293) → `PRADYOS_FSRM_OK` (304) → unpaired umount of that same ctx (358). Only
whether the probe finishes before line 358 separates pass from fail. **The
dangerous sequence runs on every boot, including green ones.**

The failing signature is exactly what that predicts: `fsrmtest` creates and
writes successfully, then its reopen fails — `FSRM FAIL: created file did not
persist` — because the volume beneath it was reformatted.

Two readings died getting here, recorded so they are not retried:
- It is **not** two SFS contexts coexisting. There is only ever one, which is
  why the `live=` instrument never exceeds 1. **Lifetime, not coexistence.**
- The `live=` discriminator table ranked "root ctx umounted" the *least* likely
  branch. It is the one that holds.

## 2. Why the obvious fix is a use-after-free

`main.c:611` already polls a TCB's state directly:

```c
while (g_cw_thread->state != THREAD_BLOCKED && g_ticks < dl) yield();
```

so the natural move is to hoist the three probe pointers and wait for
`THREAD_ZOMBIE`. **That is a use-after-free.** `sched_start_reaper()` reclaims
orphaned zombies, so the TCB can be freed while the waiter is dereferencing it.
The `crosswake_proof` precedent is safe *only* because its thread blocks and
never exits — which does not generalise to probes whose whole purpose is to
exit.

## 3. Decision — wait by pid, not by pointer

`sched_find_pid(uint32_t pid)` is documented as "live thread by pid, or NULL"
(`sched.h:235`), so it returns NULL once the thread is gone and never
dereferences freed memory on the caller's behalf. Record each probe's `pid` at
spawn; before the destructive block, wait until every recorded pid is gone.

Bounded with the `g_ticks + N` idiom already used throughout this thread, and
**falling through on expiry**: if a probe hangs, behaviour is exactly today's
(umount proceeds) rather than a new boot hang. A gate that used to fail
intermittently must not become a gate that wedges.

Ordering note: the wait must sit **before** `vfs_unmount(smnt)` and after all
three spawns, so it covers whichever probes the `probe_enabled()` flags actually
started. Pids of probes that were never spawned stay 0 and are skipped.

## 4. What this does NOT change

- Not a virtio-blk or SFS change. Only the order in which `fs_test_thread`
  tears down a mount relative to its own probes.
- The self-tests still run and are still destructive; they simply no longer run
  while a probe holds the root.
- No new writable global (DDR-826): the pids are locals in `fs_test_thread`.

## 5. Verification bar

`smoke-fsrm` **20/20** (the operator bar for this item), plus `smoke-shell` 5/5
and the §7 hygiene set. Also `smoke-ftrunc` and `smoke-rename-sfs`, since those
probes share `smnt` and the new wait covers them.

**Honest limit:** FSRM has never reproduced locally, so 20/20 green does not
prove the race is gone — it proves no regression and that the wait does not
hang. What makes this more than a hypothesis is that the dangerous sequence is
visible in *every* passing log (§1), so the fix removes a real ordering hazard
whether or not CI has yet caught it. Closure remains CI over time.

## 6. What would refute this

- `FSRM FAIL` recurring **with** the wait in place → the probe is not the only
  user of that mount, or the wait expired; the log will show which, because
  expiry is announced.
- A new boot hang at this point → the deadline is wrong, not the design; the
  fall-through exists precisely so this cannot happen silently.
