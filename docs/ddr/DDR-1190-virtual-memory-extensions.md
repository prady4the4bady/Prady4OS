# DDR-1190: Swap, page replacement, page cache, working sets, `mlock`, huge pages (checklist C2.1)

Status: design ahead (#45 comment 6436706603, item 2). No code until C1.3 has
merged (`Needs: C1.3`): reclaim must know which pages are shared (P1's
`PTE_SW_SHMEM`, P2's segments).

## 1. Where the tree is (read from `dev/phase1` = `7981cd4`)

- **PMM.** A buddy allocator over `[16 MiB, 1 GiB)` (`kernel/mm/pmm.h`,
  ADR-003, ADR-005), orders 0-10, with a per-frame reference count
  (`pmm_incref`, `pmm_refcount_get`). CI's QEMU has 28 630 managed frames
  (`pmmtot` in the heartbeat), about 112 MiB.
- **No reclaim.** When frames run out, a COW or stack fault returns -1 and the
  thread is killed (`kernel/mm/vmm_cow.c`, "OOM -> real fault"). Nothing is
  ever written out.
- **No page cache.** FAT and SFS read and write through `blk_read`/`blk_write`
  (`kernel/drivers/blk/blk.h`) on every access.
- **4 KiB user pages only.** `kernel/mm/vmm.h` maps user memory at 4 KiB; a
  huge (PS) entry is only ever on a kernel path.
- **Memory caps exist for agents only** (`aether_mem_charge`).

## 2. Scope split

| Part | Content |
|---|---|
| P1 | Swap: an encrypted swap partition; clock (second-chance) reclaim of private anonymous pages by scanning each address space's page tables; swap entries in non-present PTEs |
| P2 | Page cache for file data, with a write-back thread for dirty pages; `fsync` and `sync` |
| P3 | Working-set control: a per-process soft limit (`memory.high`-like); reclaim takes from processes over their limit first |
| P4 | `mlock` (locked pages are never reclaimed; a limit for unprivileged processes) and 2 MiB huge pages for anonymous `MAP_HUGETLB` mappings. Today every user page-table walker refuses or skips PS entries (`vmm_user_range_ok` fails closed on them; the fork, unmap and reclaim walkers skip them), so P4 first makes each of those walkers handle a PS entry. Until all of them do, `MAP_HUGETLB` falls back to 4 KiB pages and returns that in `mincore`-style reporting, rather than half-working |

**Why page-table scanning, not a reverse map.** A reverse map (frame to every
PTE that maps it) is the Linux way, but it adds a per-frame list to every COW
fork and every shm attach, on code that DDR-1178 has only just made correct.
Scanning each address space's page tables in clock order needs no new
per-frame state.

**The page-table walk is authoritative, not the VMA list.** `tcb.vma[]` records
only anonymous `mmap` regions; the user stack is tracked separately
(`user_stack`, `USER_STACK_TOP`) and ELF segments by the loader. So reclaim
walks every present user PTE below the kernel half, and a PTE is a candidate
only if all of these hold:

- it is user, present, writable-private anonymous memory (mmap arena or the
  user stack), not an ELF image page (those are re-read in a later part, not
  swapped);
- it carries none of `PTE_SW_SHARED` (vDSO and other frames shared across
  address spaces, never refcounted and never freed), `PTE_SW_SHMEM` (shm), or
  `PTE_SW_COW`;
- it is not a PS (huge) entry;
- its frame's reference count is exactly 1.

Anything else stays resident. That is recorded as a limit: shared and COW pages
are never swapped in P1. Arm K below checks the vDSO case.

## 3. Decisions (delegated; my recommendations, taken)

- **V1. Swap is authenticated encryption.** Each page is sealed with the
  kernel's AEAD (`aead_seal`/`aead_open`, `kernel/crypto/aead.h`) under a
  per-boot random key that never leaves memory.
  - **Nonce:** a 64-bit write counter, global per boot and never reused, is
    stored in the slot's metadata. It forms the nonce, so no two writes under
    the key share one (RFC 8439 requires that; a per-slot nonce would repeat
    when a slot is reused).
  - **Tag:** the 16-byte tag is stored with the slot's metadata (a metadata
    area at the start of the partition, 24 bytes per slot: counter and tag).
  - **AAD:** the slot index and the counter, so a sealed page cannot be
    replayed into another slot or from an earlier write.
  - **Failure:** `aead_open` checks the tag before it writes plaintext. A
    failed check kills the faulting process (`SIGBUS`); the kernel does not
    map unauthenticated data and does not panic.

  Swap content at rest is unreadable and tamper-evident after power-off, and no
  key management is needed. Swap is not hibernation (hibernation would need a
  stored key, and is out of scope).
- **V2. What is never swapped:** kernel memory, page tables, shm, surface and
  `PTE_SW_SHARED` frames, COW pages, huge pages, `mlock`ed pages, and pages with
  a reference count above 1 (the candidate rule in section 2).
- **V3. Swap device.** A partition of type `0x82` on any block device, found at
  boot (`blk_mbr_parse`). With none present, P1 is inactive and OOM behaves as
  today.
- **V4. OOM with swap full.** The faulting process is killed, as today. The
  kernel never panics on user memory pressure.
- **V5. `mlock` limit.** 64 KiB per unprivileged process (Linux's default
  `RLIMIT_MEMLOCK`); more needs the same `RES_SCHED`-style capability DDR-1188
  K1 introduces for RT classes. Agents are refused `mlock`.
- **V6. Write-back interval.** Dirty pages older than 5 s are written back; the
  thread runs every second. `fsync` writes the file's pages and calls
  `blk_flush`.

## 4. Gate `smoke-mempressure` (probe `user/memtest.c`; a second virtio-blk disk with a 128 MiB swap partition)

| Arm | Part | Assertion | Mutant | What the mutant breaks |
|---|---|---|---|---|
| A | P1 | a process touches (free frames + 16 MiB) of anonymous memory with a pattern, then reads every page back correctly | M1 | swap-in returns a zero page |
| B | P1 | swap slots in use are above 0 after A and back to 0 after the process exits | M2 | slots not freed on exit |
| C | P1 | the raw swap partition does not contain A's pattern, and flipping one byte of a swapped-out page on disk makes the next touch kill the process with `SIGBUS` (V1) | M3, M3b | pages written in the clear; tag not checked on swap-in |
| D | P1 | a shm segment mapped by two processes keeps its data through the pressure of A (V2) | M4 | shared pages reclaimed |
| E | P2 | a second read of the same file causes no `blk_read` (block-read counter unchanged) | M5 | the cache is bypassed on read |
| F | P2 | after `write` + `fsync`, the data is on the device (read raw through the block layer) | M6 | `fsync` does not write back |
| G | P3 | of two equal processes, the one over its soft limit has more pages swapped out | M7 | the limit is ignored by reclaim |
| H | P4 | an `mlock`ed region takes no major fault under A's pressure | M8 | locked pages reclaimed |
| I | P4 | a `MAP_HUGETLB` mapping is 2 MiB aligned and backed by one order-9 block, and every user consumer handles it: a `read()` into it succeeds (`vmm_user_range_ok`), a forked child sees its data, `munmap` returns its 512 frames, and reclaim leaves it resident | M9, M9b | the mapping falls back to 4 KiB pages silently; a walker still skips PS entries |
| J | P1 | with swap full, the allocating process is killed and a second probe still runs (V4) | M10 | the fault path panics or hangs |
| K | P1 | under A's pressure, the vDSO clock still advances in a process that has been idle (its `PTE_SW_SHARED` page was not reclaimed) | M11 | the candidate rule ignores `PTE_SW_SHARED` |

Every arm is checked for vacuity before it is written. A, B, C and J fail today
because there is no swap (A's process is killed at the first failed fault). E
and F fail because there is no cache.

## 5. Risks

- **Reclaim touches every process's page tables.** It must take each address
  space's `mm` lock (DDR-1181 §3.3) and must not run in interrupt context. A
  thread faulting on a page under write-out waits on the page, not on a global
  lock.
- **TLB shootdown.** Unmapping a page another CPU may have cached needs the
  IPI shootdown. It is built (`kernel/mm/tlb.c`, `tlb_shoot`, used by
  `vmm_unmap_range_flush` and `vmm_cow.c`; merged as `5e4ccf5` per DDR-1178),
  although DDR-1177's own header still reads "design only". Reclaim is the
  first caller that unmaps pages of a process running on another CPU, so P1
  must show, with an arm, that a page reclaimed while its owner runs elsewhere
  is refetched rather than read through a stale TLB entry.
- **Disk errors on swap-in** kill the process (`SIGBUS`), never the kernel.
- **OPEN-1/2/12/13.** Memory pressure changes timing on paths those defects
  live on. Any new hang or `[apfreeze]` signature is a stop.

## 6. Honesty labels

- Nothing here is built. Each part is **not verified** until its arms are
  green and its mutants are shown red in CI.
- Emulated (QEMU TCG, virtio-blk) only, and **not verified on real hardware**.
  Swap speed under TCG says nothing about real disks; no performance claims.
- This record does not close OPEN-1/2/12/13 or U1-U3.
