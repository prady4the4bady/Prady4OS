# DDR-954 — `vfs_unmount` frees the FS context while a syscall is still using it

Status: ROOT CAUSE NAMED AND EVIDENCED. Fix designed, NOT yet implemented.
Supersedes the "which caller" question left open in DDR-953 §f / §h.

## a. The capture

`tools/ci/gate_rate.sh smoke-aether-sfsroot 10` — run 1 of 10 (kernel md5
`6f4641973f0a5dd62612950cf78e6a0c`, unchanged across all ten runs):

```
[sfs] umount ctx=0x0000000007C3C000 caller=0xFFFFFFFF8002A025   <- vfs_unmount+0x55
*** NEXUS KERNEL PANIC ***  #PF  error=0x0
RIP=0xFFFFFFFF8002FBAF        alloc_run+0x9f
RDI=RSI=0x0000000007C3C000    <- THE CTX THAT WAS JUST FREED
RDX=0x0000000DEADBEEE0        <- KHEAP_DEBUG free poison
```

Backtrace, resolved with `sym_at.py` (exact integers, §S21):

```
syscall_entry → syscall_dispatch → sys_unlink → vfs_unlink
              → sfs_unlink → bt_insert → bt_insert_rec
              → alloc_block → alloc_run   (#PF)
```

## b. What this proves

Three facts line up and leave no room for interpretation:

1. The freed pointer and the in-use pointer are **the same value**
   (`0x07C3C000` in the umount trace, and in `RDI`/`RSI` at the fault).
2. `RDX` holds **KHEAP_DEBUG's free poison**, so the chunk was genuinely on
   the free path — not merely aliased.
3. The faulting stack originates at **`syscall_entry`**, i.e. a ring-3 thread
   was *inside a syscall* using the context at the moment it was freed.

So the holder DDR-953 §f could not name is: **the VFS mount entry's private
context pointer, dereferenced by an in-flight FS syscall.** `vfs_unmount` calls
`sfs_umount` → `kfree(ctx)` with no regard for threads currently executing
inside that mount.

## c. Why the earlier evidence pointed at `bt_insert_rec`

DDR-953 §h named `bt_insert_rec` as the caller. That was correct but shallow —
it is merely the frame that happened to dereference `c` first. The real defect
is two frames further out and one thread away: the lifetime of `mount->private`
is not coupled to the lifetime of the syscalls using it.

## d. Relationship to the superblock aliasing seen earlier

DDR-953 observed `bd == SFS_MAGIC` (a superblock aliasing the freed ctx); this
capture shows `bd` holding heap poison instead. Both are the SAME defect at
different points in the reuse timeline: immediately after `kfree` the chunk
holds poison; once recycled (a page-sized ctx handed back for a superblock read)
it holds `SFS_MAGIC`. The variable content is why the symptom looked like two
different bugs.

## e. Measured rate — this is an intermittent, quantified

`smoke-aether-sfsroot`, N=10, one kernel, hash verified unchanged each run:

| outcome | count |
|---|---|
| PASS | 9 |
| FAIL (this panic) | 1 |
| runs containing `[sfs-uaf]` | **0** |

Two things follow, and both matter:

- The failure rate is **~1 in 10**, not "always" and not "rare enough to ignore".
- **Zero `[sfs-uaf]` lines fired in any of the ten runs, including the failure.**
  The DDR-953 guard did NOT catch this one, because at this point in the
  timeline `bd` holds poison rather than a registered-device mismatch reached
  through `rd_block`. The guard covers a real but narrower window than assumed.
  It is not the safety net; it is a tripwire for one variant.

## f. The fix — designed, deliberately not yet written

The naive fix (NULL `mount->private` on unmount + NULL-guard the cast sites)
**narrows but does not close** this race. The failing thread had already loaded
the pointer before the free; NULLing the slot afterwards cannot retract a
pointer another CPU is holding in a register. That is a time-of-check /
time-of-use gap, and shipping it would convert a loud panic into a rarer one
while claiming the bug was fixed.

What the evidence demands instead:

1. **Refcount the mount context.** Every syscall entering a mount takes a
   reference; `vfs_unmount` marks the mount as going away, refuses new
   references, and frees only when the count reaches zero.
2. **NULL the slot and guard the cast sites** as defence in depth, so a missed
   path fails as `-EIO` on NULL rather than executing on poison.
3. Only then is the DDR-953 guard a redundant third layer.

Lock order (§R9) must hold: `mount → blk`. The refcount must not be taken under
a block-device lock.

## g. Gate

`smoke-sfs-uaf`, three arms: normal mount/umount; unmount racing an in-flight
`unlink`; and use-after-unmount, which must return `-EIO` rather than fault.
Acceptance is **N=20 green on one kernel hash** via `gate_rate.sh`, because a
1-in-10 defect trivially survives a 5-run check.
