# DDR-1196: an mmap hint near 2^64 wraps the arena check

Status: **implemented** in this PR. **Security boundary: review before merge.**
Found by: CodeRabbit on #80 (review 5743637985, "Reject mmap hints whose page range wraps past VMM_MMAP_TOP"). Checked against `dev/phase1` at `9306963`: the same check is there, so the defect predates #78 and #80.
Platform: QEMU on GitHub-hosted runners. Nothing here is verified on real hardware.

## 1. The defect

`sys_mmap_locked` (`kernel/syscall/sys_mmap.c`, `dev/phase1`) bounds a hinted mapping like this:

```c
base = addr;                                         /* page-aligned hint */
if (base < VMM_MMAP_BASE || base + npages * PAGE_SIZE > VMM_MMAP_TOP)
    return -EINVAL;
```

The sum is 64-bit and can wrap. For example, with `addr = 0xFFFFFFFFFFFFF000` and two pages:
- `base + 2 * 4096` is `0x1000`;
- `base < VMM_MMAP_BASE` is false;
- `0x1000 > VMM_MMAP_TOP` is false.

So the check passes. `vma_overlaps` computes the same wrapped end, so it sees no overlap either. The loop then maps page `i` at `base + i * PAGE_SIZE`.

**What that loop does on the unfixed tree** (corrected after CodeRabbit 4449499650; read from `vmm.c`, not run step by step):
1. **Page 0 goes to `0xFFFFFFFFFFFFF000`.** This is under PML4 entry 511, whose lower tables every address space shares.
   - `descend` promotes every existing entry it walks through to `VMM_USER` (`vmm.c:206-207`, "promote existing entry to user-walkable"). So the walk marks the shared kernel PDPT, PD and PT entries on that path user-accessible, **in every address space**.
   - The leaf is then written `VMM_USER | VMM_RW`.
2. **Page 1 goes to VA 0.** On a BIOS boot VA 0 is inside a 2 MiB identity page in the shared low PML4 entry, and `descend` refuses a huge page (`vmm.c:204-205`). So this map fails.
3. **The rollback unmaps page 0** with `vmm_unmap_range_flush`. That call also frees any page-table node that becomes empty, so it can free a node of the *shared* kernel table.
4. **The call returns `-ENOMEM`, not the hint.** No user mapping survives, but steps 1 and 3 have already changed kernel page tables that every process uses.

**Why the reasoning above is believed (W1 run 41553380786):** with the check restored and this probe call in `systest`, every boot shard that runs `systest` panics. Shard 5's panic is `#GP` in `list_remove` (`kernel/mm/pmm.c`, the buddy free list), at RIP `0xFFFFFFFF80011442` = `list_remove+0x32`. That RIP was resolved against a local build of the same commit `7b0c81b`, not against the CI artefact. A freed shared table node (step 3) would corrupt the allocator in exactly this way. That link is **likely, not proven**.

**A second hazard, at teardown (CodeRabbit 4450365007; read from `vmm.c`, not run).**
- **How a top-level entry gets promoted.** Each address space has its own PML4 page, whose kernel slots are copies of the kernel master's entries.
  - `descend` promotes the entry it walks through *in this copy* to `VMM_USER`. That happens for `PML4[511]` on page 0's walk, and for `PML4[0]` on page 1's walk, before the 2 MiB page refuses it.
  - The promoted slot then no longer equals the master's entry.
- **What teardown does with it.** `vmm_destroy_address_space` (`vmm.c:418-423`) frees the subtree of every slot whose entry differs from the master (`np[i] != kp[i]`), on the rule that such a slot is private.
  - So when this process's address space is destroyed, it frees the kernel's *shared* PDPT/PD/PT tables under `PML4[0]` and `PML4[511]`.
  - That needs no rollback at all, so it also follows from a wrapping call whose page 1 had mapped.
- **Not shown to be W1's cause.** No teardown trace from W1 shows this path either, so it is a second candidate cause for the panic. Neither is proven.

Any ring-3 process can do this with one `mmap` call. The result is user-accessible entries in the shared kernel page tables, and possibly a freed kernel table node.

## 2. The fix

Bound the base first, then compare the page count against the room left in the arena, so nothing can wrap:

```c
if (base < VMM_MMAP_BASE || base > VMM_MMAP_TOP ||
    npages > (VMM_MMAP_TOP - base) / PAGE_SIZE)
    return -EINVAL;
```

- Any hint the old check accepted correctly is still accepted.
- Only ranges that wrapped, or that end past `VMM_MMAP_TOP`, are refused.
- `vma_overlaps` is safe after this, because `base + npages * PAGE_SIZE` can no longer wrap.

## 3. Gate: `smoke-sysmmap` gains `SYSMMAP WRAP REJECTED`

`user/systest.asm` adds one call after the offset check: `mmap(0xFFFFFFFFFFFFF000, 8192, RW, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)`. It prints `SYSMMAP WRAP REJECTED` only if the call returns `-EINVAL`. The line is added to `smoke-sysmmap`'s sentinels.

- **Vacuity.** On the tree before the fix the call returns `-ENOMEM` (§1 step 4), not `-EINVAL`. So the line is never printed and the gate is red, whatever else the call corrupts.
- **Mutant W1** restores the wrapping sum. Run [41553380786](https://github.com/prady4the4bady/Prady4OS/actions/runs/41553380786) on `7b0c81b`: **red, though not at the designed sentinel.** All 10 boot shards fail with `NEXUS KERNEL PANIC`. In shard 5 the panic comes in the shard's first gate (`smoke-mprotect`), before `smoke-sysmmap` runs, so `SYSMMAP WRAP REJECTED` is never reached. The panic is **likely** §1 step 3's corruption, which is not proven (§1). The fixed tree is green on `996d67b` ([41554913417](https://github.com/prady4the4bady/Prady4OS/actions/runs/41554913417), 16 of 16 jobs).

## 4. Where else it applies

- #80 (C1.3 P3) moved this check into `mmap_pick_base`. The anonymous, private-file and `FD_SHM` paths use it, and so does #103's file path. The same fix is ported there.
- `shmat` takes no hint (P2 refuses a non-zero address) and checks its own `mmap_next`-based range, so it is not affected.
- #80's `6943d97` already bounds `len` (DDR-1178 A3.4). That is a separate defect, and this record does not depend on it.

## 5. Honesty labels

- **Gated** by `smoke-sysmmap`, `SYSMMAP WRAP REJECTED`. QEMU TCG only, **not verified on real hardware**.
- **Not gated:** what the unfixed kernel does to its page tables (§1 steps 1 and 3, and the teardown hazard). The gate checks only that the call is refused with `-EINVAL`.
- **Not fixed here, recorded:** `descend` promotes any entry it walks to `VMM_USER`. After this fix no user mapping can reach a kernel-half entry through `mmap`. Whether other callers of `vmm_map_in` can reach one is audited in DDR-1198: after this fix, none can.
