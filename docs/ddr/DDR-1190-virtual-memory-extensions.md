# DDR-1190: Swap, page replacement, page cache, working sets, `mlock`, huge pages (checklist C2.1)

Status: design ahead (#45 comment 6436706603, item 2). C1.3 is DONE (#103
`8d69135`), so `Needs: C1.3` is met. Amendment 1 (below) rechecks this record
against `dev/phase1` = `3890e4c` and is the design P1 is built from; where they
differ, Amendment 1 wins.

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
| P4 | `mlock` (locked pages are never reclaimed; a limit for unprivileged processes) and 2 MiB huge pages for anonymous `MAP_HUGETLB` mappings. Today every user page-table walker refuses or skips PS entries (`vmm_user_range_ok` fails closed on them; the fork and unmap walkers skip them; P1's reclaim walker, when built, also skips them, per the candidate rule in section 2), so P4 first makes each of those walkers handle a PS entry. Until all of them do, `MAP_HUGETLB` falls back to 4 KiB pages and returns that in `mincore`-style reporting, rather than half-working |

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

- it is user, present, writable-private anonymous memory, and its address is
  in the mmap arena (`[VMM_MMAP_BASE, VMM_MMAP_TOP)`) or the user stack
  (`[USER_STACK_BOT, USER_STACK_TOP)`). ELF image pages are excluded by
  address: today `kernel/exec/elf.c` accepts a `PT_LOAD` anywhere in
  `[USER_MIN, USER_STACK_BOT - PAGE_SIZE)`, which includes the arena, so P1
  first makes the loader refuse (`-ENOEXEC`) any `PT_LOAD` that overlaps the
  arena. With that, the range check is exact and needs no per-process segment
  list. Arm L checks the refusal;
- it carries none of `PTE_SW_SHARED` (vDSO and other frames shared across
  address spaces, never refcounted and never freed), `PTE_SW_SHMEM` (shm), or
  `PTE_SW_COW`;
- it is not a PS (huge) entry;
- its frame's reference count is exactly 1.

Anything else stays resident. That is recorded as a limit: shared and COW pages
are never swapped in P1. Arm K below checks the vDSO case.

The `PTE_SW_SHARED` exclusion is defence in depth today: every
`PTE_SW_SHARED` mapping on the tree is either read-only (the vDSO) or outside
the arena and stack (the framebuffer view at `FB_USER_VA` =
`0x87_0000_0000`, `kernel/syscall/sys_fb.c`). A mutant that removes only that
check is therefore equivalent and cannot be killed, so it is not used. Arm K's
mutant M11 removes the region, writable and marker conditions together, which
makes the vDSO page a candidate.

**Unmapping in another address space.** `vmm_unmap_range_flush(pml4, ...)`
takes a pml4, but its second pass calls `vmm_unmap(p)`, which walks
`read_cr3()`. It is correct only for the active address space, which is how
every current caller uses it. Reclaim unmaps pages of address spaces that are
not active, so P1 adds a pml4-aware `vmm_unmap_in(pml4, va)`, makes the
helper's second pass use it, and does not switch CR3 to reclaim.

## 3. Decisions (delegated; my recommendations, taken)

- **V1. Swap is authenticated encryption.** Each page is sealed with the
  kernel's AEAD (`aead_seal`/`aead_open`, `kernel/crypto/aead.h`) under a
  per-boot random key that never leaves memory.
  - **Nonce:** a 64-bit write counter, global per boot and never reused, is
    stored in the slot's metadata. The 12-byte nonce (`AEAD_NONCE_LEN`) is
    four zero bytes followed by the counter in little-endian order (bytes 4
    to 11), built by one helper that swap-out and swap-in share. No two writes
    under the key share a nonce (RFC 8439 requires that; a per-slot nonce
    would repeat when a slot is reused).
  - **Tag:** the 16-byte tag is stored with the slot's metadata (a metadata
    area at the start of the partition, 24 bytes per slot: counter and tag).
  - **AAD:** the slot index and the counter.
  - **Trusted counter:** the counter written to disk is not trusted on the way
    back. The non-present PTE holds only the slot index (15 bits for 128 MiB of
    4 KiB slots). The trusted counter lives in kernel memory, in a per-boot
    table `swap_ctr[slot]` (8 bytes per slot, 256 KiB for 128 MiB of swap),
    written when the slot is filled and cleared when it is freed: on swap-in,
    on unmapping the swapped page, and on address-space teardown. Swap-in
    builds the nonce and AAD from `swap_ctr[slot]`, never from the on-disk
    metadata. Restoring an
    older sealed page together with its older metadata therefore fails the tag
    check, as does moving a sealed page to another slot.
  - **No wrap:** at the next write after the counter reaches 2^64 - 1, swap-out
    is refused (the page stays resident), so a nonce is never reused under the
    key.
  - **Failure:** `aead_open` checks the tag before it writes plaintext. A
    failed check kills the faulting process (`SIGBUS`); the kernel does not
    map unauthenticated data and does not panic.

  After power-off the per-boot key is gone, so swap content at rest is
  unreadable to anyone. Tampering is detected only within the boot that wrote
  the slot; a later boot never reads old swap (it starts with an empty slot
  table), so it has nothing to verify. No key management is needed. Swap is not hibernation (hibernation would need a
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
| C | P1 | the raw swap partition does not contain A's pattern, and flipping one byte of a swapped-out page on disk, or restoring a slot's earlier sealed page and metadata, makes the next touch kill the process with `SIGBUS` (V1) | M3, M3b, M3c | pages written in the clear; tag not checked on swap-in; nonce taken from the on-disk counter |
| D | P1 | a shm segment mapped by two processes keeps its data through the pressure of A (V2) | M4 | shared pages reclaimed |
| E | P2 | a second read of the same file causes no `blk_read` (block-read counter unchanged) | M5 | the cache is bypassed on read |
| F | P2 | after `write` + `fsync`, the data is on the device (read raw through the block layer) | M6 | `fsync` does not write back |
| G | P3 | of two equal processes, the one over its soft limit has more pages swapped out | M7 | the limit is ignored by reclaim |
| H | P4 | an `mlock`ed region takes no major fault under A's pressure | M8 | locked pages reclaimed |
| I | P4 | a `MAP_HUGETLB` mapping is 2 MiB aligned and backed by one order-9 block, and every user consumer handles it: a `read()` into it succeeds (`vmm_user_range_ok`), a forked child sees its data, `munmap` returns its 512 frames, and reclaim leaves it resident | M9, M9b | the mapping falls back to 4 KiB pages silently; a walker still skips PS entries |
| J | P1 | with swap full, the allocating process is killed and a second probe still runs (V4) | M10 | the fault path panics or hangs |
| K | P1 | under A's pressure, the vDSO clock still advances in a process that has been idle (its `PTE_SW_SHARED` page was not reclaimed) | M11 | the candidate rule is reduced to "user, present, refcount at most 1" (region, writable and marker checks removed), which makes the vDSO page a candidate |

| L | P1 | an ELF probe with a writable `PT_LOAD` inside the mmap arena is refused with `-ENOEXEC` | M12 | the loader's arena-overlap check skipped |
| M | P1 | a thread pinned to CPU 1 (`SYS_SCHED_CPU`) reads a page holding pattern P1 in a loop, while reclaim on CPU 0 swaps that page out and its freed frame is reused by a sibling that writes pattern P2. The page's swap-out count is at least 1 (so the arm fails on a tree without reclaim), and every read on CPU 1 returns P1, never P2 | M13 | the reclaim unmap skips `tlb_shoot` (local `invlpg` only), so CPU 1 keeps reading the reused frame |

`smoke-mempressure` passes only when every arm A-M passes.

Every arm is checked for vacuity before it is written. A, B, C and J fail today
because there is no swap (A's process is killed at the first failed fault). E
and F fail because there is no cache. L fails because the loader accepts the
segment today, and M fails because its swap-out count is 0 without reclaim.

## 5. Risks

- **Reclaim touches every process's page tables.** It must take each address
  space's `mm` lock (DDR-1181 §3.3) and must not run in interrupt context. A
  thread faulting on a page under write-out waits on the page, not on a global
  lock.
- **TLB shootdown.** Unmapping a page another CPU may have cached needs the
  IPI shootdown. It is built (`kernel/mm/tlb.c`, `tlb_shoot`, used by
  `vmm_unmap_range_flush` and `vmm_cow.c`; merged as `5e4ccf5` per DDR-1178),
  although DDR-1177's own header still reads "design only". Reclaim is the
  first caller that unmaps pages of a process running on another CPU. Arm M
  shows that a page reclaimed while its owner runs elsewhere is refetched
  rather than read through a stale TLB entry (mutant M13 drops the
  shootdown).
- **Disk errors on swap-in** kill the process (`SIGBUS`), never the kernel.
- **OPEN-1/2/12/13.** Memory pressure changes timing on paths those defects
  live on. Any new hang or `[apfreeze]` signature is a stop.

## 6. Honesty labels

- Nothing here is built. Each part is **not verified** until its arms are
  green and its mutants are shown red in CI.
- Emulated (QEMU TCG, virtio-blk) only, and **not verified on real hardware**.
  Swap speed under TCG says nothing about real disks; no performance claims.
- This record does not close OPEN-1/2/12/13 or U1-U3.

## Amendment 1: recheck against `3890e4c` (C1.6 and C1.7 merged), before P1 is built

The body above was written at `7981cd4`. Since then, C1.3 P4 (file-backed
`MAP_SHARED`), C1.6 (user ASLR, static PIE, `PT_INTERP`, the L1 write-history
bit) and C1.7 (KASLR) have merged. This amendment rechecks each premise P1
depends on, and records six changes. Nothing here is built yet.

### A1.1 Premises that still hold

- **Shared pages are marked.** Every shared user page (anonymous `MAP_SHARED`,
  SysV, `shm_open`, file `MAP_SHARED`) is mapped `PTE_SW_SHMEM`
  (`kernel/syscall/sys_mmap.c`), so the candidate rule's `PTE_SW_SHMEM`
  exclusion covers all of them.
- **File `MAP_PRIVATE` is eager** (DDR-1112): the frame is filled from the file
  before it is mapped, and is then an ordinary private anonymous page. It is a
  candidate under the section 2 rule, and swapping it out is correct.
- **Windows outside the arena.** The PIE window `[USER_PIE_BASE, USER_PIE_TOP)`,
  the interpreter window `[USER_PIE_TOP, USER_IMAGE_TOP)` and the framebuffer
  view `FB_USER_VA` all lie below `VMM_MMAP_BASE` (`0x88_0000_0000`), so the
  arena range test excludes them.
- **The block layer** registers several devices and MBR partitions
  (`blk_register`, `blk_part_create`, `blk_mbr_parse`), so V3's `0x82` swap
  partition on a second virtio-blk disk needs no driver change.
- **KASLR** (DDR-1179) does not touch user memory. Reclaim's new printers of
  kernel code addresses unslide like every other (`kaslr_unslide`, A4.9).

### A1.2 Arm L is dropped: its premise is already enforced

Section 2 planned to make the loader refuse a `PT_LOAD` that overlaps the
arena. DDR-1189 Amendment 2 already bounds every image to
`[USER_MIN, USER_IMAGE_TOP)` (`kernel/exec/elf.c`, the "one bound for every
image" check), and `_Static_assert(USER_IMAGE_TOP <= VMM_MMAP_BASE)` pins the
layout. Arm L would pass on the unfixed tree, so it is vacuous and is not
written; mutant M12 goes with it.

### A1.3 P1 reclaims the arena only, not the stack

With user ASLR the stack is `[stack_top - USER_STACK_SIZE, stack_top)`, with
`stack_top` drawn per process (`USER_STACK_LOWEST_BOT`, DDR-1189 A1 P1-3). It is
also demand-paged (ADR-038), with an eager window of `USER_STACK_EAGER_PAGES`
because `vmm_user_range_ok` refuses untouched stack pages. **P1 drops the stack
from the candidate rule**, so a candidate must lie in
`[VMM_MMAP_BASE, VMM_MMAP_TOP)`. A stack is at most 8 MiB per process, and
leaving it resident keeps P1 out of the ADR-038 fault and eager-window logic.
This is a limit, recorded here and in the P1 PR.

### A1.4 Syscall buffers in a swapped-out page: a swap-in pre-pass

Section 2 did not consider the user-pointer path. `vmm_user_range_ok` is
ADR-022's "never allocates and never faults" check, and it returns 0 for any
leaf that is not present. After P1, a `read()` into a buffer whose page was
swapped out would therefore fail with `-EFAULT` instead of faulting the page
back in.

- **The fix leaves `vmm_user_range_ok` unchanged** and adds
  `swap_prefault(cr3, va, len)`. It brings every swapped-out page of the range
  back in before the check, under the caller's mm lock.
- **Callers.** There are seven call sites: `copyin`, `copyout` and `copyinstr`
  in `kernel/mm/uaccess.c`; `ring_frame` in `sys_io_uring.c`; and the
  three checks in `sys_thread.c`. Each calls `swap_prefault` first (A1.10
  point 2 makes this a pin taken under the mm lock). A swap-in
  that fails is handled as A1.10 point 4 says (`SIGBUS`, or the out-of-memory kill).
- **io_uring is safe under this design.** `ring_frame` resolves the ring's
  frame under the mm lock on every `io_uring_enter`, and reclaim takes the same
  lock (A1.6), so the frame cannot be reclaimed between the check and its use.
- **New arm N** (P1): a `read()` from a file into an arena buffer whose page was
  swapped out returns the full count, and the data is right. **Mutant M14:**
  `swap_prefault` is a no-op, so the `read()` returns `-EFAULT`.

### A1.5 The swap entry keeps the page's flags, `PTE_SW_EVERW` included

The L1 W^X-across-time rule (DDR-1189 A3, A4) keeps a per-page write history in
`PTE_SW_EVERW` (bit 53). `mprotect(PROT_EXEC)` on a page that was ever writable
is refused. If swap-out dropped bit 53, a written page brought back by swap-in
would look never-written, and could then be made executable. That is a W^X
bypass, so the encoding preserves every flag:

- **The entry.** A swapped-out page's PTE keeps its flag bits, `VMM_USER`,
  `VMM_RW`, `VMM_NX`, `PTE_SW_EVERW`, with `PTE_PRESENT` clear. A new marker,
  `PTE_SW_SWAP` (bit 54, free today; 52 and 53 are taken), is set, and the
  address field holds the slot index instead of a frame. A non-present PTE
  without `PTE_SW_SWAP` stays "never mapped", as today (the stack's demand
  paging relies on that).
- **Swap-in** clears the marker, sets `PTE_PRESENT` and puts the new frame in
  the address field, and changes nothing else.
- **New arm O** (P1): a page that was written, swapped out and swapped in is
  still refused by `mprotect(PROT_READ | PROT_EXEC)`. **Mutant M15:** swap-in
  rebuilds the PTE as `VMM_USER | VMM_RW | VMM_NX` and drops bit 53.

### A1.6 Every user page-table walker handles a swap entry

Each walker that skips non-present leaves would treat a swap entry as "nothing
here". That is right for some walkers and wrong for others:

| Walker | Today | With swap |
|---|---|---|
| fork (`vmm_fork_address_space_cow`) | skips non-present leaves | **would drop the page from the child.** Fork first brings every swapped-out arena page of the parent back in, under the parent's mm lock; if that cannot get frames, fork fails with `-ENOMEM`. Sharing a slot between parent and child is not built in P1 (a limit). |
| unmap (`vmm_unmap_range_flush`, `MAP_FIXED` replacement) | skips | frees the slot and clears `swap_ctr[slot]` |
| teardown (address-space free) | skips | the same: frees every slot (arm B) |
| `mprotect` | skips non-present | updates the flag bits kept in the entry, so the next swap-in maps the new protection. The L1 check reads bit 53 from the entry as it does from a present PTE (arm O covers the refusal). |
| `vmm_user_range_ok` | refuses | refuses; the A1.4 pre-pass runs first |
| `vmm_dirty_op` (`msync`) | -1 for a non-present page | unchanged: shared pages are never candidates |
| reclaim (new) | — | skips everything outside the candidate rule, and every non-present leaf |

- **New arm P** (P1): after a page of the parent is swapped out, a forked child
  reads that page's pattern. **Mutant M16:** the fork pre-pass is removed, so
  the child sees the page as unmapped and is killed.
- **Lock order.** Reclaim runs in the faulting thread, which holds its own mm
  lock, and takes other address spaces' mm locks with a try-lock. An address
  space whose lock is busy is skipped in that pass, so two reclaiming threads
  can never deadlock. The `LOCKDEP=1` build (DDR-1209) is run over arm A to
  show that no inversion is recorded.

### A1.7 Size

`kernel.bin` is 1,728,920 B against the 1,769,472 B gate, which is 40,552 B of
headroom. #72 (DDR-1182), once merged, takes 8,192 B of that (1,737,112 B on its head `1e31557`), and the
binding `RCU_TORTURE=1` build has 24,168 B before #72. P1 adds reclaim, the swap
entry, the walker changes, `swap_prefault` and the AEAD glue. The AEAD itself is
already in the tree. If P1 does not fit, the window is raised in the same PR
under a DDR-1183 amendment, as Amendments 1 and 2 did; the gate is not loosened.
The 256 KiB `swap_ctr[]` table comes from the PMM at swap-on, not from
`kernel.bin`.

### A1.8 The gate after this amendment

Arms A to K and M stand as in section 4 (A to D, J, K and M are P1). Arm L is
dropped (A1.2). P1 adds:

| Arm | Part | Assertion | Mutant | What the mutant breaks |
|---|---|---|---|---|
| N | P1 | a `read()` into a swapped-out arena page returns the full count, with the right data | M14 | `swap_prefault` is a no-op |
| O | P1 | a page written, swapped out and swapped in is still refused `PROT_EXEC` | M15 | swap-in drops `PTE_SW_EVERW` |
| P | P1 | a forked child reads a page the parent had swapped out | M16 | the fork pre-pass is removed |

**Vacuity.** Each of N, O and P needs a swap-out count of at least 1 on the
page it checks, so on a tree without swap each fails at its first step,
exactly as A does.

### A1.10 Review fixes (CodeRabbit and Codex on #172)

These five points are part of the design P1 is built from. Where they differ
from A1.4 to A1.8, A1.10 wins.

**1. Bit 54 is a software bit (Codex 4726669313; AGENTS.md: cite the SDM).**
- **Present leaf.** Intel SDM Vol. 3A, chapter 4 (4-level paging), in the
  table "Format of a Page-Table Entry that Maps a 4-KByte Page": bits 58:52
  are ignored by the processor. Bits 62:59 are protection-key bits when
  CR4.PKE = 1, and ignored otherwise. AMD APM Vol. 2, section 5.4 (long-mode
  page-translation entries), likewise leaves bits 58:52 available to software.
  Bit 54 is inside 58:52 under both manuals and does not touch the
  protection-key field. Table numbers vary between editions, so the citation
  names the table, not its number. Not checked on real hardware.
- **Not-present leaf.** With P = 0, the processor ignores bits 63:1 (the same
  chapter's "not present" entry format). So the slot index in bits 12 and up,
  and the kept flags, are never interpreted by the hardware.
- **Two non-present kinds.** A non-present leaf with `PTE_SW_DYING` (bit 11)
  is a frame awaiting its TLB-shootdown acknowledgement (DDR-1177). A
  non-present leaf with `PTE_SW_SWAP` (bit 54) is a swap entry. The two are
  never set together: reclaim does not choose a leaf that is dying, and a
  swap entry has no frame to shoot down.
- **Carrying the bit.** Like bits 52 and 53, bit 54 and the slot field are
  carried by every rebuild of a user leaf that masks flags. The only rebuild
  of a swap entry is `mprotect` (A1.6).

**2. The buffer stays resident through the copy (CodeRabbit 4726661216).**
`copyin`, `copyout` and `copyinstr` do not hold the mm lock while they copy,
so a page brought back by a bare pre-pass could be reclaimed before the copy.
The pre-pass and the check are therefore one call, made under the lock, with
a pin that reclaim honours:
- **`uaccess_pin(cr3, va, len, writable)`:**
  1. takes the mm lock;
  2. swaps in every swapped-out page of the range;
  3. runs `vmm_user_range_ok`;
  4. on success, raises `mm->uaccess_active`;
  5. releases the lock.

  `uaccess_unpin` lowers the count.
- **Reclaim** holds the target mm's lock (taken by try-lock, A1.6) and skips
  any address space whose `uaccess_active` is above zero.
- **`copyin`, `copyout` and `copyinstr`** become pin, copy, unpin. The three
  `sys_thread.c` checks use `uaccess_pin` followed at once by `uaccess_unpin`.
- **io_uring.** `ring_frame` already holds the mm lock, so it calls
  `swap_prefault_locked` (no lock taken) before its check. Reclaim needs the
  same lock, so the ring frame cannot be reclaimed while it is in use.

**3. Not under a spinlock (CodeRabbit 4726666561).** A swap-in can wait on a
disk read, so it must never run inside a spinlock. `ledger_sign`
(`kernel/syscall/sys_ledger.c`) calls `copyout` while holding
`g_ledger_lock`, which guards its `static` output buffer.
- **The rule, made explicit in the API:** a caller that copies under a
  spinlock pins the user range before it takes the lock, unpins after
  releasing it, and copies with `copyin_pinned` / `copyout_pinned`. Those
  never pre-pass, never take the mm lock, and never sleep. In every build
  they check that the **calling thread** holds a pin and that every page of
  the range is resident, and otherwise refuse with `-EFAULT`. A nonzero
  per-mm count alone could be another thread's pin, which that thread could
  drop in the middle of this copy, so the check reads a per-thread count:
  `uaccess_pin` raises both `t->uaccess_pins` and `mm->uaccess_active`, and
  `uaccess_unpin` lowers both. `t->uaccess_pins` is a new `struct tcb` field,
  with an explicit initialiser in `sched_create_state` (`kmalloc` does not
  zero). No spinlock detection is needed for any of this. The audit's fix at each site is therefore visible in the
  code, as a `_pinned` call with a pin around its lock. `ledger_sign` becomes
  `uaccess_pin(ubuf)`, `spin_lock`, `copyout_pinned`, `spin_unlock`,
  `uaccess_unpin`.
- **What the default build cannot see (a limit).** In the default build,
  `spin_lock` is a bare test-and-set (`kernel/include/spinlock.h`). It keeps
  no held-lock depth and does not mask interrupts, so a plain `copyin` or
  `copyout` cannot tell that it runs under a spinlock. A site the audit
  misses, one still using the plain call under a lock, would sleep there. Detection is in the `LOCKDEP=1` build only, the same posture
  DDR-1209 takes for lock order. Adding a held-lock count to every
  `spin_lock` in every build is rejected: it would put a cost on the hottest
  lock path to catch what the audit and the `LOCKDEP=1` gate already catch.
- **`ledger_sign`** calls `uaccess_pin(ubuf, sizeof out)` before
  `spin_lock(&g_ledger_lock)`.
- **The audit.** The P1 PR reads every `copyin`, `copyout` and `copyinstr` call
  site reached with a spinlock held, not only grep's matches, and lists each
  one here with the same fix.
- **The detector.** In the `LOCKDEP=1` build, a plain `copyin`, `copyout`
  or `copyinstr` reached with a spinlock held (DDR-1209 `ld_held`) records a
  lockdep report and refuses with `-EFAULT`, without swapping in. It fires
  whether or not the range is resident, so a missed site is found on any
  run that reaches it. `smoke-lockdep`
  forbids the report, so a missed site that meets a swapped-out page turns
  that gate red.
- **Mutant M14c:** `ledger_sign` goes back to an unpinned plain `copyout`
  under `g_ledger_lock`, and the `LOCKDEP=1` run of arm N3 must show the
  report.
- **Lock order.** A copy made while holding a `kmutex` now takes the mm lock
  after it. `mmap` of a file holds the mm lock and then reads the file, so
  the `LOCKDEP=1` run (A1.6) is also the check for an inversion between file
  locks and the mm lock. An inversion found there is fixed the same way, by
  pinning before the outer lock.

**4. What a failed swap-in does (CodeRabbit 4726661220).** A1.4's "as an
unmapped page does today" is withdrawn. The pre-pass and a ring-3 fault
behave the same:
- **A failed tag check or a disk error** sends `SIGBUS` to the process (V1,
  section 5). The syscall returns `-EFAULT` with the signal pending, so the
  process dies before it can use the result.
- **No frame** is V4's out-of-memory case: the process is killed, as a ring-3
  fault with no frame is today (`vmm_stack_fault`).

**5. Gate additions.**

| Arm | Part | Assertion | Mutant | What the mutant breaks |
|---|---|---|---|---|
| N2 | P1 | a `read()` into an arena buffer on CPU 1 runs while reclaim on CPU 0 targets that buffer's pages, and returns the full count | M14b | reclaim ignores `uaccess_active` |
| N3 | P1 | under `LOCKDEP=1`, the probe swaps out the page of its own buffer `ubuf`, then calls `ledger_sign(ubuf)`. The call returns `LEDGER_MSG_BYTES`, the data matches, and no copy-under-spinlock report is recorded | M14c | `ledger_sign` uses an unpinned plain `copyout` under `g_ledger_lock`: a report, and `-EFAULT` |
| C2 | P1 | a `read()` whose buffer page fails the tag check kills the process with `SIGBUS`; it does not get `-EFAULT` and continue | M3d | the pre-pass maps a tag failure to a plain `-EFAULT` |

**Vacuity.** N2 needs that page's swap-out count above 0 during the run. N3
needs the swap-out count of `ubuf`'s page above 0 before the call. C2 needs a swapped-out page whose sealed copy was corrupted. Each
condition fails on a tree without swap.

### A1.9 Honesty labels

Unchanged from section 6. Nothing in this amendment is built. It closes no open
defect (OPEN-1/2/12/13, U1-U3). QEMU (TCG) only, not verified on real hardware.
