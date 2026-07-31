# DDR-810 (§S5 / F#68) — metric lockbox: three blockers found in the tree first

**Status:** §Design — **blocked on decisions, no code.** Three problems with the
specification are recorded below; two are design-level and one is a missing
primitive.
**Date:** 2026-07-31
**Relates to:** DDR-795 (`metric_page`, already shipped), ADR-021 (W^X),
§J-03 (audit chain, which stores its head here).

## Tree check first — part of this is already built

`kernel/aether/metric_page.c/.h` exists and ships a **sealed objective-function
root**: one physical frame, kernel-writable through the low identity map, mapped
into every user address space **read-only + NX** at `METRIC_USER_VA`. A ring-3
store faults and the kernel converts it into a clean process kill, reusing the
path ADR-021's `wxviol` probe already exercises.

So F#68's tamper-resistant anchor is not missing. What §S5 adds is *content* —
boot count, uptime, gate tallies, agent liveness — and a read syscall.

## Blocker 1 — storing the lockbox in SFS defeats its own purpose

§S5 specifies `/metric/lockbox` as an SFS region. DDR-795's header already
argues against exactly this, and the argument is correct:

> F#68's Python lockbox keeps the metric content AND its hash chain in one file,
> so anything able to write that file can rewrite the entries and recompute the
> chain over them. The chain proves internal consistency, not that the history
> was not replaced.

A hash chain stored beside the data it protects is forgeable by anyone who can
write both. §S5 says "not writable by any user process even with
`CAP_SOVEREIGN`" — but the VFS gates writes on **`CAP_FS_WRITE` alone**
(`vfs.c:91,112,140`). There is no path-based deny, and a sovereign process
holds `CAP_FS_WRITE`. As specified, the lockbox would be writable by precisely
the processes it is meant to be protected from.

Adding a path guard is possible but weak: it is a namespace check, and namespaces
have aliases (relative paths, future mounts, any rename primitive). An invariant
enforced by string comparison is not an invariant.

**Proposed instead:** the authoritative record lives in the **`metric_page`
frame**, which ring 3 structurally cannot write — the guarantee comes from the
page tables, not from a path check. SFS may hold a *mirror* for convenience, but
the mirror is explicitly non-authoritative, and `SYS_METRIC_READ` returns the
page's copy. If the mirror disagrees with the page, that is a detection event,
which is the whole point.

The current `metric_page_t` has 4032 bytes of padding — enough for the §S5 record
set without a layout change beyond claiming some of it.

## Blocker 2 — there is no hash primitive in this kernel

`grep -rln "blake3\|BLAKE3\|sha256\|SHA256" kernel/ tools/` returns **nothing**.
§S5 requires "a BLAKE3 hash of the record" and verification on read; §J-03
requires `BLAKE3(prev || current)` for the audit chain. Neither can be built
until a hash exists in the kernel.

This is a prerequisite slice, not a detail of §S5, and it needs its own DDR
because a hand-rolled hash is **worse than no hash**: it presents as integrity
while providing none, and nothing downstream would reveal the difference. Its
gate must validate against **published test vectors** — an implementation that
merely "produces 32 consistent bytes" passes a naive gate and fails its purpose.

## Blocker 3 — BLAKE3 contradicts the tree's existing convention

`metric_page.h` declares `METRIC_ROOT_LEN 32 /* SHA-256, raw bytes */`. §S5 and
§J-03 specify BLAKE3. Both are 32 bytes, so the mismatch is invisible at the type
level and would surface only as a verification failure between the kernel and
the already-shipped Python side.

This needs a decision, not a coin flip: either the tree moves to BLAKE3 and
DDR-795's field comment plus the Python producer change with it, or §S5/§J-03
adopt SHA-256. **Recommendation: SHA-256**, because the shipped side already
produces it, it is the smaller change, and the security difference is irrelevant
at these sizes for a tamper-evidence use case. BLAKE3's speed advantage does not
matter for one 4 KB record per boot.

## What is NOT blocked

The record layout, the boot/shutdown write points, and `SYS_METRIC_READ`'s
sovereign gating are all designable now and unaffected by the above. They are
deliberately left unwritten in this document until the three decisions land,
because writing them against `/metric/lockbox` and BLAKE3 would produce a design
that has to be discarded.

## Gate obligations (unchanged in intent)

`smoke-metric-lockbox`, `FORBIDDEN_SENTINEL: LOCKBOX_STUB`, three arms:
absent → `-ENOENT`; corrupted hash → `-ETAMPER`; correct → readable and verified.
The mechanism metric must be that the gate **recomputes the hash itself** and
compares, not merely that the syscall returned success — otherwise the gate
passes against an implementation that never verifies anything.

A fourth arm is required by Blocker 1 and is the one that matters: a
`CAP_SOVEREIGN` process attempts to write the lockbox and **must fail**. Against
the SFS design that arm would fail today.
