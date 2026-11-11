# DDR-1202 — #112: `mprotect` copies a shared private page before it adds write

**Status:** implemented in this PR. A kernel change (page tables). **Security boundary.**
**Decision:** the operator, #45 comment 6761846242 item 2: option (b) of #112, copy first, with `PTE_SW_SHMEM` pages exempt, checked against the code first.
**Tree:** every reading below is at `97f0397` (`dev/phase1`), before this change.
**Honesty labels:** QEMU TCG on GitHub-hosted runners only. Nothing here is verified on real hardware. §7 holds the run links.

---

## 1. The defect (issue #112)

1. `vmm_fork_address_space_cow` shares a **read-only** private user page as it is: the child maps the same frame, no `PTE_SW_COW` is set, and `pmm_incref` adds a reference (`vmm_cow.c:97-106`, "Read-only (e.g. text): share as-is, no COW needed").
2. `vmm_protect_range` refused `VMM_RW` only on a `PTE_SW_COW` page (`vmm.c:289`). A read-only page with two references and no COW bit passed.
3. So `mprotect(PROT_READ|PROT_WRITE)` gave one process a writable PTE on a frame the other still mapped. Each then saw the other's writes through a **private** mapping.

Read-only was the only thing keeping the two processes apart on that frame. Program text is shared the same way.

**Before this PR:** found by reading, not run. **In this PR:** arm F runs the exact interleaving, and mutant P1 (§6) is the unfixed behaviour.

## 2. The fix

In `vmm_protect_range`, between the validation pass and the apply pass, when `VMM_RW` is asked for:

```c
if (e & (VMM_RW | PTE_SW_SHMEM | PTE_SW_SHARED)) continue;
if (pmm_refcount_get(frame) <= 1)                 continue;
copy = ptnode_alloc();  memcpy(copy, frame, 4096);
*pte = (e & ~PTE_ADDR) | copy;        /* same bits, new frame */
invlpg(p);  tlb_shoot(pml4, p, 1);  pmm_free_page(frame);
```

- **Which pages are copied:** a page that is not writable now, is private (neither `PTE_SW_SHMEM` nor `PTE_SW_SHARED`), and whose frame has more than one reference. `PTE_SW_COW` pages never get here; they are refused in the pass before, as today.
- **A sole owner is not copied.** With one reference nobody else maps the frame, so write is granted in place. This keeps arm C of `smoke-mprotect` as it was: its child has exited and been reaped before the parent adds write back.
- **Order.** The new frame is installed and every CPU running this address space is flushed **before** our reference to the old frame is dropped. After the drop the other holder may free the frame, and it may be handed out again.
- **All or nothing still holds.** This pass changes frames, not permissions. If an allocation fails part-way the call returns `-ENOMEM` (`-3` inside the kernel). The pages already copied hold the same bytes with the same permissions, so the caller cannot see a half-applied change.
- **Races between the two processes** (by reading):
  - both add write at once: each sees two references, each copies, each drops one. The old frame is freed. Correct.
  - one copies first: the other then sees one reference and keeps the frame. Correct.
  - neither can write the old frame before its own copy or sole ownership, so the copy source is stable.

## 3. `PTE_SW_SHMEM` is exempt, and why the operator's example is not the case that needs it

- A `PTE_SW_SHMEM` page is a `MAP_SHARED` or SysV mapping. Sharing is what was asked for, so it must not be copied.
- **Checked against the code, as asked.** `smoke-shm` arm E does `mprotect(PROT_RW)` on a page that is **already writable**. No write is added, so this pass skips it with or without the exemption. Arm E would not have caught a missing exemption.
- The case that needs the exemption is a shared page that is read-only **now** and gains write. No gate did that. **Arm H does**, and mutant P2 removes the exemption.

## 4. Found while checking: a read-only kernel view could gain write

- `PTE_SW_SHARED` marks a view of a frame the address space does not own: the vDSO page, the metric page, a surface, the framebuffer. The vDSO and metric pages are mapped **read-only** in every process, from one frame, with no extra reference (`vdso_page.c:29`, `metric_page.c:41`).
- `vmm_protect_range` kept the `PTE_SW_SHARED` bit but did not refuse write. The reference count is 1, so the copy rule of §2 does not cover it either.
- `sys_mprotect`'s address bound does not stop the call (§5). **So any process could make the vDSO page writable, and its writes would reach every process.** By reading; mutant P3 (§6) is this behaviour.
- **Now:** adding write to a read-only `PTE_SW_SHARED` page is refused with `-EACCES`.
  - This is the safe option and I took it without asking (6761846242 item 6); it is reported on #45.
  - Cost: a process that removes write from its own surface or framebuffer view cannot add it back. No program in the tree does that.

## 5. Found while checking: the address bound wraps (issue #119, not changed)

- `sys_mprotect_locked` tests `len > (VMM_USER_MAX - addr)`. For an address above `VMM_USER_MAX` the subtraction wraps, so the test passes. `mprotect` therefore reaches the stack, the vDSO page and the metric page.
- Not changed here. Whether the window should end at `VMM_USER_MAX` is a policy question, tracked as #119.
- Arm G reaches the vDSO page **through** this bound. If #119 is fixed by tightening the bound, arm G must change with it.

## 6. Gate: three new arms in `smoke-mprotect`

`user/mprotecttest.c`, after the existing arms A to E.

| Arm | Required line | What it does |
|---|---|---|
| **F** isolation | `PRADYOS_MPROT_ISO child_up=1 parent_up=1` | A page is written, made read-only, then shared by fork. **F1:** the child adds write and stores; the parent must still read the old value. **F2:** the parent adds write and stores while the child is alive; the child must still read the old value. |
| **G** kernel view | `PRADYOS_MPROT_VIEW rc=-13` | `mprotect(PROT_READ\|PROT_WRITE)` on the vDSO page is refused. Nothing is stored into it. |
| **H** shared stays shared | `PRADYOS_MPROT_SHKEEP shared=1` | A `MAP_SHARED` page is made read-only and shared by fork. The child adds write and stores; the parent must see the new value. |

- F2 needs the child to read after the parent's store. The turn is passed through one `MAP_SHARED` page, which fork shares by design (DDR-1178).
- **Would each arm pass on the unfixed tree?** F: no, both stores land in the shared frame (`child_up=0 parent_up=0`). G: no, the call returns 0. H: yes. H guards the fix, not the old defect, so its mutant is on the fix.

**Mutants.** Each must be red in CI on its own arm.

| Mutant | Change | Expected |
|---|---|---|
| **P1** | the copy is skipped (the code before this PR) | arm F red |
| **P2** | `PTE_SW_SHMEM` pages are not exempt | arm H red (`shared=0`) |
| **P3** | a read-only `PTE_SW_SHARED` page is not refused | arm G red (`rc=0`) |

**Not gated.**
- The flush before the old reference is dropped (§2, order). A wrong order needs another CPU of the same process to read through a stale entry in a very short time.
- The dropped reference itself. If it were not dropped the old frame would leak; no arm counts frames here.
- `-ENOMEM` when the copy cannot be allocated.
- Program text (the same code path as arm F, with an executable page).
- The two-processes-at-once races of §2.

## 7. Evidence

**Not verified yet.** No CI run of the gate or of mutants P1 to P3 is recorded here. Until this section names the runs, every statement above about what the arms or mutants show is a design expectation, not a result. The merge waits for this section (#45 comment 6594914166).

## 8. What this record does not claim

- That the vDSO write (§4) was ever used. It is by reading; mutant P3 is to show it in CI (§7).
- Anything about `PTE_SW_COW` pages: adding write to one is still refused, as DDR-1031 §3b decided.
- Anything about real hardware.
