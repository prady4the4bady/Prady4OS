# DDR-1031 — `SYS_MPROTECT` (NSI 97)

Status: **IMPLEMENTED + GATED + mutation-checked (M1/M3; M2 landed on NO arm — §7)**
(§1–§5 landed as design before the code, per §NON-NEGOTIABLE 5; §5 grew a fifth
arm and §6's M2 prediction was WRONG — see §7.)
Group D. NSI **97** — verified free against `kernel/syscall/syscall.h`, whose
current max is 96 (`SYS_KEY_POLL`, DDR-991), per §INV.14's instruction to check
the header rather than trust a carried-forward number.

---

## 1. What exists and what does not

`vmm.h` exposes `vmm_map`/`vmm_map_in` (map a frame with flags), `vmm_unmap`,
`vmm_resolve` (VA→PA) and `vmm_user_range_ok` (validate). **There is no
range-protect primitive** — nothing changes a mapping's permissions while
keeping its frame. That is the piece this DDR adds; the syscall on top is thin.

## 2. The two software PTE bits, and why a naive implementation corrupts them

A PTE here carries more than hardware permissions:

| bit | name | meaning |
|---|---|---|
| 9 (`0x200`) | `PTE_SW_COW` | copy-on-write (IMP-D) |
| 10 (`0x400`) | `PTE_SW_SHARED` | deliberately double-mapped frame (DDR-1003) |

An `mprotect` that rebuilt the PTE as `frame | new_flags` would silently clear
both. Clearing `PTE_SW_SHARED` breaks the invariant DDR-1003 audited across all
nine `vmm_map_in` sites; clearing `PTE_SW_COW` makes `vmm_cow_fault` return
early (`vmm_cow.c:115` tests exactly that bit) and the page is then never
copied. **Both bits are preserved verbatim**, along with the cache attributes
`VMM_PWT`/`VMM_PCD`.

## 3. Three refusals, each with a reason

**(a) `PROT_WRITE | PROT_EXEC` → `-EACCES`.** W^X is the posture of this kernel
(DDR-757, and `map_core` already applies `VMM_NX` when the flag is set). A
syscall that let ring 3 request a writable-executable page would be a hole
straight through it.

**(b) `PROT_WRITE` on a page tagged `PTE_SW_COW` → `-EACCES`.** COW's copy
trigger *is* the hardware read-only bit: `fork` downgrades both address spaces
to RO and tags them (`vmm_cow.c:89`), and the next write traps. Granting write
here would not "make the page writable" — it would let one process write into a
frame another still shares, with no copy and no fault. Refusing is the only
option that does not require teaching `mprotect` to perform the copy itself,
which is a larger change than this DDR takes. Downgrading a COW page (removing
write) is harmless and **is** allowed.

**(c) `PROT_NONE` → `-EINVAL`.** Making a user page not-present collides with
the demand-paged stack path (`vmm_stack_fault`, ADR-038), which faults absent
user pages *in* rather than reporting them. Distinguishing "absent because
mprotect said so" from "absent because it has not been faulted yet" needs a
fourth state that does not exist today. Named, not built.

## 4. All-or-nothing

The range is validated in a first pass and mutated in a second. A range that is
partly unmapped, or that contains one COW page under `PROT_WRITE`, changes
**nothing** — a half-applied protection change is worse than a rejected one,
and POSIX callers do not expect to unwind it.

## 5. The probe and its arms

`user/mprotecttest.c`. The enforcement arm cannot simply write to a read-only
page — that kills the probe. It **forks**, lets the child take the fault, and
reads the outcome from `wait4`, which is the only way to observe a fatal fault
without becoming it.

| arm | sentinel | what it would catch |
|---|---|---|
| A | `PRADYOS_MPROT_RO rc=0` | the syscall or the range walk is broken |
| B | `PRADYOS_MPROT_ENFORCED sig=…` | the PTE was rewritten but the CPU still permits the write — protection not actually applied |
| C | `PRADYOS_MPROT_RESTORED rc=0 val=…` | one-way: RO→RW does not restore, or the frame was lost (value must survive) |
| D | `PRADYOS_MPROT_WX rc=-13` | W^X not enforced |

C is what catches a range-protect that preserves permissions but loses the
frame: it asserts the **value written before the RO transition is still there**
after restoring RW, so a rebuilt-from-zero PTE fails even though the mapping
"works".

None of the four is implied by another, checked against the dead-arm rule this
project has now hit six times (DDR-1016 §5, 1017 §4, 1018 §3, 1020 §5 twice,
1023 §5, 1026 §4).

## 6. Arm E — the one the design missed

§5 shipped with four arms, and none of them could see the software bits §2 is
about: A–D never touch a shared page. `vmm_protect_range` is reached only
through `mprotect`, so `smoke-cowfork` cannot see a dropped `PTE_SW_COW` either
— the original M1 plan (validate against a different gate) was **unrunnable for
the same reason it was proposed**.

The observable consequence is available, though, and it is exactly the asymmetry
§3b creates. Removing write from a COW page is allowed; adding it is refused. So:

```
q is writable across the fork  ->  fork tags it COW in both address spaces
mprotect(q, PROT_READ)             ->  0        (allowed: downgrade)
mprotect(q, PROT_READ|PROT_WRITE)  ->  -EACCES  (refused: still COW)
```

If the first call had dropped `PTE_SW_COW`, the second would return **0**, and
the page would then be writable with no copy — silently corrupting a frame the
child's address space still points at. `PRADYOS_MPROT_COW rc=-13` is that arm,
and it is the only one that can see it.

## 7. Mutation results

| mutant | change | kernel | outcome |
|---|---|---|---|
| — | clean | `0bf4d1df5502b2cb` | **PASS**, all five arms |
| **M1** | drop `PTE_SW_COW`/`PTE_SW_SHARED` preservation | `d7dce7a13f82d86c` | **FAIL at arm E only** — `COW tag lost across mprotect rc=0` |
| **M3** | drop the W^X refusal | `e1239532af6f99db` | **FAIL at arm D only** — `W^X not enforced rc=0` |
| **M2** | drop `invlpg` | `a5b1e4dbd1107888` | **PASSED — no arm caught it** |

### 7.1 M2 passed, and §6's prediction was wrong

The design predicted arm B would fail without `invlpg`. It does not, and the
reason is structural rather than a gap in the arm:

- **Arm B is decided by the child's page tables, not the parent's TLB.** `fork`
  copies the already-updated PTE, and the address-space switch flushes; the
  child faults correctly whether or not the parent invalidated anything.
- **Arm C passes under a stale entry too.** It writes after RO→RW, and a stale
  *writable* TLB entry permits exactly that.

**The `invlpg` is therefore not covered by this gate, and cannot be covered by a
probe of this shape.** A missing invalidation is only observable as *a write
that should have faulted and did not* — and the process that observes it is the
one that dies. The usual escape is a `SIGSEGV` handler, which this kernel does
not have on this path: a ring-3 fault goes straight to `sched_exit(-1)`
(`idt.c:703`), never to a user handler. Recorded as an uncovered line, not
claimed as tested.

