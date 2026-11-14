# DDR-1203: `map_core` refuses a user leaf under a kernel-shared top-level entry

Status: **design, before any code.** Proposed by DDR-1198 §3 ("a guard in `map_core` would close it for every caller at once ... proposed here, not done"). Listed by #45 6761846242 item 5 as a small task for while CI runs.
Read from the code at `dev/phase1` `adc4e5c`. Nothing here has run. Nothing is verified on real hardware.

## 1. The defect class

`descend` (`kernel/mm/vmm.c:202`) promotes every **existing** intermediate entry it walks through to `VMM_USER` when the leaf is a user mapping (`vmm.c:207-208`). Each address space's PML4 starts as a copy of the kernel master's top-level entries (`vmm_new_address_space`, `vmm.c:423`). Below the top level, those entries point at tables the whole system shares: the low identity map (slot 0), the kernel MMIO windows (slots 256 and up) and the higher half (slot 511).

So a user leaf whose VA lands in a slot the master populates:
- marks shared kernel tables user-walkable, in every address space;
- makes this address space's top-level copy differ from the master's, so `vmm_destroy_address_space` frees a shared kernel subtree at teardown (DDR-1196 §1).

DDR-1196 closed the one path that reached it (an `mmap` hint that wrapped). DDR-1198 §2 read every `vmm_map_in` caller and found none that can reach it now. Nothing stops the **next** caller that maps a user page at a VA it does not bound.

## 2. Decision

**D1. `map_core` refuses, before any `descend`, a leaf with `VMM_USER` when either:**
1. the kernel master has a present entry at the leaf's PML4 index (`table_at(g_kernel_pml4)[idx(virt, 4)] & PTE_PRESENT`); or
2. the root being written **is** the kernel master (`pml4_phys == g_kernel_pml4`). A user leaf in the master's empty slot 1 would populate it, and every address space created afterwards would share that subtree.

It returns -1, which every caller already handles as a failed map. It writes nothing, so no entry is promoted and no table is allocated.

**D2. It is reported, once.** Every refusal adds 1 to a counter, `g_vmm_user_guard_hits`. The first *unexpected* refusal prints `[vmm] FAIL: user leaf under a kernel-shared top-level entry va=<va> slot=<n>`, and a once-flag, `g_vmm_user_guard_reported`, stops it printing again. Only an unexpected refusal reads or sets that flag. A refusal made while the probe's expect latch (§4) is set prints the expected line instead and leaves the flag clear, so the probe's refusals cannot use up the report (CodeRabbit on #126). The line goes into `GLOBAL_FORBIDDEN` (`tools/qemu_runner/boot_test.sh`), appended before its final line as the rules require; the count rises from 77 to 78, and CLAUDE.md's number moves with it. Every boot gate then fails if any path reaches the guard. That is the intent: by DDR-1198 no product path should.

**D3. Before `g_kernel_pml4` is set** (early boot, `vmm.c:34`), the guard is skipped: there is no master to compare against, and no user mapping exists yet.

**D4. Not changed.** `descend`'s promotion rule stays: a user leaf in a private slot (1 or 255) still needs every level it walks to be user-walkable. Kernel mappings (`VMM_USER` clear) are not checked.

## 3. Callers (by reading, at `adc4e5c`)

- **`vmm_map_in` callers** are DDR-1198 §2's table. Every user leaf is in slot 1 or slot 255, and the master populates neither, so none of them is refused.
- **`vmm_map` (the current CR3)** has 14 call sites. None passes `VMM_USER` (`grep -rn "vmm_map(" kernel | grep USER` is empty), so D1 rule 2 refuses nothing today.
- **`vmm_fork_address_space_cow`** (`vmm_cow.c:79`, `:102`) copies the user leaves of slots that differ from the master. After D1 no slot the master populates can hold a user leaf, so fork has nothing to spread (DDR-1198 called it an amplifier).

So the guard changes no behaviour on any path the tree has today. That is also why it needs its own probe: no existing gate reaches it.

## 4. Gate: probe `mapguard` (a `QEMU_PROBES` kernel self-test)

The arms run on a fresh address space from `vmm_new_address_space`, never on a live process. Each arm is checked for vacuity on the unguarded tree.

| arm | what it does | asserts | on the unguarded tree |
|---|---|---|---|
| G-A | `vmm_map_in(as, <slot 511 VA>, frame, VMM_USER \| VMM_RW)` | returns -1; the master's slot 511 entry and the one below it are unchanged and carry no `VMM_USER`; `as`'s top-level entry still equals the master's | returns 0 and promotes both: **fails** |
| G-B | the same at a slot-256 VA (the self-test MMIO slot) | the same | fails |
| G-C | `vmm_map_in(g_kernel_pml4, <slot 1 VA>, frame, VMM_USER)` (D1 rule 2) | returns -1; the master's slot 1 stays empty | returns 0 and fills master slot 1: fails |
| G-D (control) | `vmm_map_in(as, <slot 1 VA>, frame, VMM_USER \| VMM_RW)`, then unmap it | returns 0 and the leaf is present: the guard does not block a legal user map | passes on both trees; it is the control that keeps G-A to G-C from passing by refusing everything |

`g_vmm_user_guard_hits` counts the three refusals. The probe expects exactly 3 and prints `PRADYOS_MAPGUARD a=1 b=1 c=1 d=1 hits=3`. Because the probe trips the guard on purpose, the `[vmm] FAIL` line of D2 would fail its own gate. So the probe sets a probe-only expect latch around the three calls, the way `fault_expect.h` (DDR-1040) does for expected faults, and the guard then prints `[vmm] guard refused (expected)` instead. The latch is cleared before the probe returns. Because expected refusals never set the once-flag (D2), the first refusal after that, whether or not the probe has run, prints the forbidden line.

The gate is `smoke-mapguard` (new, in a shard, `QEMU_PROBES=mapguard`). It does not rebuild the kernel, so it goes into a shard, not into `flag-gates`.

## 5. Mutants (each a one-change commit on the final head)

| mutant | change | red on |
|---|---|---|
| M1 | rule 1 removed (only rule 2 kept) | G-A, G-B |
| M2 | rule 2 removed | G-C |
| M3 | the guard runs after the first `descend`, not before it | G-A: the master's slot-511 entry is promoted even though the map returns -1 |
| M4 | the expect latch is not cleared after the probe | none of the probe's arms. It is stated **not gated**: the latch only changes which line a later refusal prints, and no later path refuses. |
| M5 | an expected refusal also sets the once-flag | none of the probe's arms, for the same reason as M4: only a later unexpected refusal would show it, and no path makes one. Stated **not gated**. |

## 6. Size

The default build gains one compare and one branch in `map_core`, and the report path. It is a few hundred bytes against 65,142 B of headroom (measured at `baac4dd`). The PR measures it.

## 7. Honesty labels

- Design only; nothing is implemented or run.
- QEMU only when built, not verified on real hardware.
- It closes no open defect (OPEN-1/2/12/13, U1–U3). DDR-1196's fix stays the fix for the path that existed; this guards the class.
