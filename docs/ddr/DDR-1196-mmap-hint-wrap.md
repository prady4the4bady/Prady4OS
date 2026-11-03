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

So the check passes. `vma_overlaps` computes the same wrapped end, so it sees no overlap either. The loop then maps page `i` at `base + i * PAGE_SIZE`, which puts:
- **page 0 at `0xFFFFFFFFFFFFF000`.** This is in the kernel half, under PML4 entry 511, the entry every address space shares. The page is created with `VMM_USER | VMM_RW`.
- **page 1 at virtual address 0.**

Any ring-3 process can reach this with one `mmap` call. The result is a user-writable page inside the kernel's shared upper mapping, plus a mapping at the null page.

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

- **Vacuity.** On the tree before the fix the call succeeds and returns `0xFFFFFFFFFFFFF000`, so the line is never printed and the gate is red. The kernel may also fault when it maps the kernel-half page; either way the sentinel is missing.
- **Mutant W1** restores the wrapping sum. It must be red on `SYSMMAP WRAP REJECTED`, and is shown in CI before merge.

## 4. Where else it applies

- #80 (C1.3 P3) moved this check into `mmap_pick_base`. The anonymous, private-file and `FD_SHM` paths use it, and so does #103's file path. The same fix is ported there.
- `shmat` takes no hint (P2 refuses a non-zero address) and checks its own `mmap_next`-based range, so it is not affected.
- #80's `6943d97` already bounds `len` (DDR-1178 A3.4). That is a separate defect, and this record does not depend on it.

## 5. Honesty labels

- **Gated** by `smoke-sysmmap`, `SYSMMAP WRAP REJECTED`. QEMU TCG only, **not verified on real hardware**.
- **Not gated:** what the unfixed kernel does after mapping the kernel-half page (fault, overwrite or nothing). The gate only checks that the call is refused.
