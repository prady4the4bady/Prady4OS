# DDR-1178: Shared memory (checklist C1.3)

Status: **P1 implemented** in PR #63 (anonymous `MAP_SHARED` across fork). Correct code was green on `80889fd` in run [39186711736](https://github.com/prady4the4bady/Prady4OS/actions/runs/39186711736), and mutants M1-M7 were red (§4). **P2-P4 are design only**: they wait on the §6 owner decisions, and this record holds no code for them. Platform: QEMU TCG only. **Not verified on real hardware.**

Checklist: `docs/V1_MASTER_CHECKLIST.md`, C1.3 — "`MAP_SHARED`, SysV/POSIX shared memory, shared file mappings with write-back". Its exit gate is named here as `smoke-shm`.

Authority: OPERATOR ORDER (#45 comment 6256077528), Order 2. Stop conditions: "a design decision not already covered by a recorded owner decision" and "anything that touches security boundaries … or capability checks". §6 lists every decision this record needs from the owner. **No code for a §6 item is written until the owner decides it.**

## 1. Current state (read from the tree at `dev/phase1` = `e5b273d`)

- **`sys_mmap` refuses `MAP_SHARED`** with `-EINVAL` (`kernel/syscall/sys_mmap.c:114`). The header names three reasons:
  - write-back;
  - a shared page cache;
  - a cross-CPU TLB shootdown.

  The shootdown now exists (DDR-1177, merged as `5e4ccf5`).
- **Frames are refcounted.** `pmm_incref` and `pmm_refcount_get` (`kernel/mm/pmm.h:45-50`) exist, and `pmm_free_page` releases a frame only at refcount 0. `ptnode_free` decrements `ptnode_in_use` only on a real release (DDR-1065).
- **Fork is a COW walk** (`kernel/mm/vmm_cow.c`):
  - A writable PTE becomes RO with `PTE_SW_COW` in both address spaces, and the frame is `pmm_incref`'d.
  - A PTE with `PTE_SW_SHARED` (bit 10) is copied verbatim **without a refcount**. That bit is the vDSO's, and teardown skips those frames.
- **Software PTE bits in use:** 9 (`PTE_SW_COW`), 10 (`PTE_SW_SHARED`) and 11 (`PTE_SW_DYING`). Bits 52-58 are ignored by hardware without PKE and are free.
- **`struct vm_area`** is `{base, npages}` with no flags (`kernel/proc/sched.h:40-43`). `munmap` frees whatever is mapped.
- **Write-back targets:**
  - FAT32 `write` takes an arbitrary offset (`kernel/fs/fat32/fat32.c:529`), so a mid-file overwrite is possible.
  - **SFS is append-only:** `off != in->size` is refused (`kernel/fs/sfs/sfs.c:1031`, DDR-1089). A writable shared mapping of an SFS file cannot be written back.
- **Free resources:** The highest defined NSI is 109 (`SYS_SIGACTION2`, C1.1), so the next free one is **110**. The "next free 108" line in CLAUDE.md is stale. **P1 adds no syscall:** arm C reads the existing `SYS_SYSINFO` (71). No existing gate asserts that `MAP_SHARED` is rejected: `smoke-sysmmap`'s `ND` arm is a different case (`user/systest.asm:319-330`). This is re-checked in the implementation PR.

## 2. Scope split (one PR per concern, in this order)

| Part | Concern | Needs an owner decision? |
|---|---|---|
| P1 | `MAP_SHARED \| MAP_ANONYMOUS`, shared across `fork` | No (§3) |
| P2 | SysV shared memory (`shmget`/`shmat`/`shmdt`/`shmctl`) | Decided in Amendment 1 (D1, D2) |
| P3 | POSIX `shm_open`/`shm_unlink` | Decided in Amendment 1 (D1, D3); designed in Amendment 3 |
| P4 | Shared file mappings with write-back (`msync`, plus write-back at `munmap`/exit) | Decided in Amendment 1 (D4, D5) |

P1 creates no new way for unrelated processes to reach each other's memory. Only the process tree that `fork`ed from the mapper shares the pages, which is what POSIX anonymous shared memory means. P2-P4 do create new channels; Amendment 1 decides how each is bounded.

## 3. P1 design: anonymous shared mappings

1. **New PTE bit `PTE_SW_SHMEM` = bit 52.** It means "refcounted shared frame". It is distinct from `PTE_SW_SHARED`, which means "not refcounted", and the two must not be merged. If the vDSO were refcounted, teardown would free a kernel page.
2. **`sys_mmap` with `MAP_SHARED|MAP_ANONYMOUS`** accepts the same prot/arena/hint rules as the private case. Frames are `ptnode_alloc`'d and mapped with `PTE_SW_SHMEM`. `MAP_SHARED` without `MAP_ANONYMOUS` stays `-EINVAL` until P4. `MAP_SHARED|MAP_PRIVATE` is `-EINVAL`, as POSIX requires.
3. **Fork:** a `PTE_SW_SHMEM` PTE is mapped into the child with **the same permission bits, no COW and no RO downgrade**, followed by `pmm_incref(frame)`. Both address spaces keep writing the same frame.
4. **Fork copies the mapping table.** `sched_create_user_clone` left the child's `vma[]` empty and `mmap_next` at the arena base (`kernel/proc/sched.c:1153`), found by review on #63. Fork now copies both.
   - **Before:** a child could not `munmap` an inherited region, and its first `addr==0` mmap reused the inherited VA, missed the overlap and overwrote a live PTE. A shared PTE overwritten that way leaks one reference.
   - **This defect predates P1** (it also hit private mappings). It is fixed here because P1 makes it a leak.
5. **`munmap`/exit:** unchanged. `vmm_unmap_range_flush` and `free_subtree` call `ptnode_free`, which drops one reference, and the frame is released at 0. Free-after-ack (DDR-1177 §3.4) already covers the cross-CPU case.
6. **`mprotect`** on a shared range changes only this address space's PTEs, as POSIX allows, plus the DDR-1177 shootdown. No change.
7. **COW fault safety.** `vmm_cow_fault` acts only on `PTE_SW_COW`, and a `PTE_SW_SHMEM` PTE never carries it. A write fault on a shared RO page (after `mprotect` RO) stays a real fault.
8. **AETHER memory cap:** the mapper is charged once, at `mmap`. The child is not charged for inherited shared frames, because they are not new memory. Recorded, not a decision: it matches how COW-shared frames are treated today.

## 4. Gate `smoke-shm` (P1 arms; P2-P4 arms are added with their parts)

A user probe `user/shmtest.c` (new `QEMU_PROBES=shm`) does the following:
- `mmap`s one shared anonymous page and one private anonymous page, and writes `0x11` to both;
- `fork`s; the child writes `0x22` to both and exits 0;
- after `wait`, the parent reads both.

| Arm | Sentinel | Pass condition | Catches | Vacuity check on the unfixed tree |
|---|---|---|---|---|
| A | `PRADYOS_SHM_A shared=0x22` | the parent sees the child's write through the shared page | **M1:** fork COWs shared PTEs like private ones (parent sees `0x11`) | Today `mmap(MAP_SHARED)` is `-EINVAL`. The probe prints `PRADYOS_SHM FAIL mmap=-22`, a forbidden sentinel, and A's line is absent. Red. |
| B | `PRADYOS_SHM_B private=0x11` | the private page still COWs | **M2:** fork treats every writable PTE as shared (parent sees `0x22`) | It would pass today (private COW works), but only together with A. This is a regression arm, not a feature arm; noted as such. |
| C | `PRADYOS_SHM_C cycles=256 leaked_ok=1` | The same 256-cycle loop (`mmap` 1 page, `fork`, child writes and exits, `wait`, `munmap`) runs twice, reading `SYS_SYSINFO.free_pages` before and after each loop: first with a **private** page (baseline), then with a **shared** one. `leaked_ok=1` iff the shared drop exceeds the private drop by < 128. A shared leak of one frame per cycle adds ≥ 256. | **M4:** shared frames skipped at teardown (leaked) | Today `mmap` fails, so the loop cannot run and the probe prints `PRADYOS_SHM FAIL`. It is evaluated only after arm A held. |
| E | `PRADYOS_SHM_E mprotect_shared=0x44` | `mprotect(PROT_READ\|PROT_WRITE)` on a shared page, then `fork`. The child writes `0x44` and the parent reads it. | **M6:** `vmm_protect_range` drops `PTE_SW_SHMEM`, so the next fork COWs the page (parent sees the old value) | Today `mmap` fails, so the arm is absent. Red. |
| F | `PRADYOS_SHM_F child_munmap_ok=1` | The first fork child `munmap`s the inherited shared page and exits 0 only if that returned 0. The parent still reads `0x22` afterwards (arm A), so a child unmap drops one reference and not the parent's frame. | **M7:** fork does not copy `vma[]`/`mmap_next` into the child (`sched_create_user_clone`), so the child's `munmap` is `-EINVAL` | Today `mmap` fails, so the arm is absent. Red. |
| D | `PRADYOS_SHM_D rejects=3` | `MAP_SHARED` without `MAP_ANONYMOUS`, `MAP_SHARED\|MAP_PRIVATE`, and `MAP_SHARED\|PROT_EXEC` all return `-EINVAL` | **M5:** the flag check dropped (a file-less shared map accepted) | It passes today (everything shared is rejected), so it is only meaningful with A. It is a boundary arm. |

**Why arm C uses a baseline.** A bare bound on the shared drop was measured too thin. In run [39177510529](https://github.com/prady4the4bady/Prady4OS/actions/runs/39177510529) (M5, all other code correct), 256 shared cycles with no leak dropped `free_pages` by **96**, against a bound of 128. That drop is ordinary per-fork kernel allocation, and any unrelated growth there would have flaked the arm. The private loop pays the same per-fork cost, so the difference isolates the shared frames.

**M2's definition.** M2 marks every **`sys_mmap`** mapping `PTE_SW_SHMEM`, private ones included. An earlier M2 shared every writable PTE at fork. That broke unrelated gates first (`smoke-auditchain` in shard 0, run 39177460441), so `smoke-shm` never ran, and that version proved nothing about arm B.

**Mutants:** M1 → arm A, M2 → arm B, M4 → arm C, M5 → arm D, M6 → arm E, M7 → arm F. That is six mutants on six different arms.

**M3 (no `pmm_incref` on the shared fork path)** is run too, but it lands on **arm A**. When the child exits, the frame's only reference is dropped through `pmm_free_page`, which frees it while the parent still maps it. The parent then reads something other than `0x22`. This is measured as an arm A failure, not as a test of poisoning. The free path stamps `PMM_POISON` (`0xDEADBEEFDEADBEEF`, `kernel/mm/pmm.c:23,227`), which would read as byte `0xEF`. The measured `0x23` therefore means the frame was reallocated and rewritten before the parent read it; which allocation did that is not established. So M3 is extra coverage on A, not a separate arm. Each mutant runs on its own `mut/ddr1178-*` branch, built on the P1 head `80889fd` (PR #63).

**Mutant evidence (all red in `smoke-shm`, shard 0; correct code green in run [39186711736](https://github.com/prady4the4bady/Prady4OS/actions/runs/39186711736)):**

| Mutant | Branch tip | Run | Arm failed | Measured line |
|---|---|---|---|---|
| M1 | `9652f89` | [39186847849](https://github.com/prady4the4bady/Prady4OS/actions/runs/39186847849) | A | `shared=0x11` |
| M2 | `5995a92` | [39186938666](https://github.com/prady4the4bady/Prady4OS/actions/runs/39186938666) | B | `private=0x22` (also red: `smoke-mprotect` shard 5, `COW tag lost across mprotect`, because the private page now carries `PTE_SW_SHMEM`) |
| M3 | `9eb5000` | [39186862620](https://github.com/prady4the4bady/Prady4OS/actions/runs/39186862620) | A | `shared=0x23` (not `0x22`: the child exit freed the only reference; where `0x23` came from is not verified) |
| M4 | `f2cf56c` | [39186877280](https://github.com/prady4the4bady/Prady4OS/actions/runs/39186877280) | C | `leaked_ok=0`; drop private=94, shared=256 |
| M5 | `1428520` | [39186892686](https://github.com/prady4the4bady/Prady4OS/actions/runs/39186892686) | D | `rejects=2` |
| M6 | `682cf77` | [39186908635](https://github.com/prady4the4bady/Prady4OS/actions/runs/39186908635) | E | `mprotect_shared=0x11` |
| M7 | `932d979` | [39186923799](https://github.com/prady4the4bady/Prady4OS/actions/runs/39186923799) | F | `child_munmap_ok=0` |

Arm C on correct code (the M5-M7 runs, where the shared path is unmutated): the absolute `free_pages` drop was 0 over the shared loop and 22-72 over the private loop. `leaked_ok` = (shared drop - private drop) < 128, which here is -22 to -72. Under M4 it was 256 - 94 = 162, so red. The earlier 96 (above) is also an absolute shared-loop drop, but it came from the previous probe, which ran the shared loop first. The current probe runs the private loop first; a plausible but **not verified** reading is that the private loop absorbs the one-time kernel allocations the shared loop paid before. The arm compares the two loops, so it does not depend on which loop pays them. **Tags not created:** pushing `evidence/ddr1178-mN` was refused (HTTP 403, reported on #45), so the evidence is the branch tips and run links above.

## 5. Risks

- **Bit 52 and NX.** NX is bit 63, so they don't collide. Every path that masks PTE flags with `0xFFF` (for example `vmm_cow.c` childflags) **drops bit 52**. Three such sites were found:
  - `vmm.c:245` (`map_core`: `flags & 0xFFF`, so **every** `vmm_map_in` would drop bit 52);
  - `vmm.c` `vmm_protect_range` pass 2 (preserves only `COW`, `SHARED`, `PWT` and `PCD`; found by review on #62);
  - `vmm_cow.c:80,95` (fork childflags).

  All three must carry `PTE_SW_SHMEM`, and arm E gates the `mprotect` one. The fork walk must carry `PTE_SW_SHMEM` explicitly, or the child's copy loses it and the next fork COWs it, which is exactly M1. The implementation PR lists every `0xFFF` mask site and how it is handled.
- **Refcount width.** It is 16 bits. A shared page inherited by more than 65,535 processes would wrap. Fork depth is bounded by the process table today; the bound is recorded in the implementation PR.
- **Huge pages.** None are used for user memory. `PG_PS` entries are skipped by the walk, unchanged.
- **Not covered by P1:** cross-process sharing between unrelated processes (P2/P3), file coherence (P4), `MAP_FIXED` replace, partial `munmap` of a shared range. **Correction (CodeRabbit 4441673086):** an earlier version of this line said a partial `munmap` is refused. It is not. `sys_munmap_locked` ignores the length and removes the whole region that contains `addr`, for every mapping. This was already so on `dev/phase1` before P1. For a P2 attachment, or a P3 mapping of an `FD_SHM` fd, that makes a partial `munmap` a full detach (A2.2, A3.2). Linux would unmap only the requested pages. The behaviour is unchanged here and is not gated; length checking is a separate item.

## 6. Owner decisions required before P2-P4 (stop condition, #45/6256077528); decided in Amendment 1

- **D1. Namespace and access control for named shared memory.** A SysV key or a POSIX name lets an unrelated process reach a segment, which is a new cross-process channel and a security boundary. Options:
  - (a) segments are reachable only by holders of a new capability right minted at creation and passed explicitly (IPC send, inheritance);
  - (b) Unix-style owner/mode bits checked against the caller's uid, which this kernel does not model today;
  - (c) `IPC_PRIVATE` / anonymous-name only, inherited through fork, with no global namespace.

  Recommendation: **(c) for v1**, then (a) once the capability model has a transferable memory right. (c) adds no new boundary.
- **D2. SysV lifetime.** `IPC_RMID` on an attached segment: POSIX says destroy it after the last detach. Is that acceptable, given that a segment can outlive its creator until every attacher exits? The alternative is to refuse `IPC_RMID` while attached.
- **D3. POSIX `shm_open` backing.** Options: a dedicated in-kernel object table, or files on a tmpfs-like mount. The second needs a new filesystem, which is not in C1.
- **D4. Write-back permission.** A writable shared file mapping writes the file at `msync`/`munmap`/exit, possibly long after the fd is closed. Should write-back reuse the capability held by the fd at `mmap` time (the DDR-1112 model, `fe->cap`), or re-check at write-back? Reuse matches POSIX. Re-checking would need a revocation model.
- **D5. Filesystems without overwrite.** For SFS (append-only), `MAP_SHARED|PROT_WRITE` on an SFS file should be refused with `-EACCES` (or `-ENODEV`), while read-only shared maps of SFS files stay allowed. A shared page cache, needed so that two mappers of one file see each other's writes, is required for P4. Options:
  - a per-`(mount, inode, page)` frame table, refcounted;
  - limiting P4 to one mapper per file.

  Recommendation: the frame table. One mapper per file is not POSIX.

## Amendment 1 (2026-10-20 UTC; Order 4 is dated 2026-10-21 +04): D1-D5 decided; C1.4 proceeds on P1

**Authority.** Order 3 (#45/6282431738) and Order 4 (#45/6301435736) delegate every decision in §6 to this record: take the recommended option, or the one safer for memory isolation where §6 gave none. Each choice below names its reason. P2, P3 and P4 are each **security boundary: review before merge**. They add ways for processes to share memory, so the PR for each says so.

| # | Decision | Reason |
|---|---|---|
| D1 | **(c) No global namespace in v1.** SysV `shmget` accepts only `IPC_PRIVATE`; any other key is `-EACCES`. POSIX `shm_open` accepts only `SHM_ANON` (the FreeBSD form); any name is `-EACCES`. A segment is reachable only through what the creator holds: the shmid for SysV, the fd for POSIX. Those reach other processes only by fork inheritance or explicit fd passing. | This adds no new cross-process channel: an unrelated process cannot name a segment. It is the §6 recommendation. A named namespace returns with the transferable memory right, option (a), and needs its own DDR. |
| D2 | **POSIX lifetime.** `IPC_RMID` marks the segment. The segment is destroyed at the last detach, and a marked segment refuses new attaches with `-EIDRM`. | With D1(c), only processes that already hold the shmid can attach, so a creator-outliving segment is reachable by nobody new. Refusing `IPC_RMID` while attached would break the common create, attach, `IPC_RMID` idiom. |
| D3 | **A dedicated in-kernel object table** (refcounted page list per object), no tmpfs. | D1(c) needs no namespace, so no filesystem is needed either. A tmpfs is not in C1. |
| D4 | **Reuse the capability captured at `mmap`** (`fe->cap`, the DDR-1112 model) for write-back at `msync`/`munmap`/exit. | It matches POSIX. A re-check would equal the same check, because the kernel has no revocation model. If revocation arrives, that DDR revisits this. |
| D5 | **`MAP_SHARED\|PROT_WRITE` on an SFS file is `-EACCES`;** read-only shared maps of SFS files are allowed. P4 adds a refcounted per-`(mount, inode, page-index)` frame table, so two mappers of one file see each other's writes. | SFS is append-only, so overwrite-in-place cannot be honoured. One mapper per file is not POSIX. The frame table follows the §6 recommendation. |

**Order of work.** First P2 (SysV) and P3 (POSIX `SHM_ANON`). They share the D3 object table: P2 adds the table and P3 reuses it. P4 (the page cache plus write-back) comes after them. Each part gets its own gate arms and mutants, and is shown red in CI before it merges.

**C1.4 starts on P1 alone (Order 4 item 1).** The checklist lists C1.3 as a dependency of C1.4. What C1.4 actually consumes is shared address spaces (`CLONE_VM` threads) and shared futex words across fork. P1 supplies the second, and C1.4 builds the first itself. Nothing in C1.4 needs a SysV or POSIX segment or a shared file mapping. P2-P4 stay open in the checklist and proceed in parallel with C1.4, while their own implementation order stays P2 → P3 → P4. If a C1.4 part later turns out to need one of them, that part waits and the DDR for C1.4 records it.

## Amendment 2 (2026-10-20 UTC, after Amendment 1): P2 design, SysV shared memory under D1(c)

**Security boundary: review before merge.** P2 lets two processes share memory through a segment id.

### A2.1 The id must be unforgeable, or D1(c) does not hold

D1(c) says a segment "is reachable only through what the creator holds: the shmid". A SysV shmid is normally a **global integer**, so any process could try small integers until `shmat` succeeds. That is a namespace with extra steps, and it is exactly the cross-process channel D1(c) refuses.

**Decision:** a shmid names a **capability in the caller's capability table**. The table already has the properties needed:
- it is per process, and shared by its threads (DDR-1181 §3.6);
- it is copied verbatim by `fork` (`cap_fork`), so a child that inherits a shmid holds a valid handle to the segment;
- it is never readable or writable by another process;
- a handle carries a generation, so a stale id cannot reach a reused slot.

**Reachability in P2 is fork only.** No syscall transfers a `RES_SHM` capability to another process. `cap_delegate` exists only as a kernel helper, and its one caller is a boot self-test (`kernel/main.c:100`). So a process that learns only the integer gets `-EINVAL`. D1(c)'s "explicit fd passing" applies to P3's POSIX fd form, not to a SysV shmid. If a transfer operation is wanted later (for example over the IPC door), it needs its own amendment. It must install the capability in the receiver's table, and ids must never become globally resolvable.

- `shmget(IPC_PRIVATE, size, flags)` creates a segment object. It mints `cap(RES_SHM, object, R|W)` in the caller's table and returns `shmid = (generation << 6) | slot`.
  - The generation is limited to 24 bits, so the id stays a positive `int`. If a slot's generation passes 2^24, the call returns `-ENOSPC`.
  - The slot fits in 6 bits because `CAP_SLOTS` is 64.
- `shmat`/`shmctl` resolve the shmid **only in the caller's own table**. A process whose table never received the capability gets `-EINVAL` for every id, so guessing is pointless.
- Any `key != IPC_PRIVATE` returns `-EACCES` (D1(c), unchanged).

### A2.2 Objects, frames and lifetime (D2, D3)

- **The object table.** It is in-kernel (D3): `struct shm_obj { npages, frames[], nattch, marked, used }`, `SHM_MAX` = 32 objects of at most 256 pages (1 MiB) each, guarded by one spinlock.
  - Frames are allocated zeroed and eagerly at `shmget`. The object holds one reference per frame.
- **Attach.** `shmat(shmid, 0, flags)` maps the frames at the next arena address.
  - Each frame gets `pmm_incref` and `PTE_SW_SHMEM`, so a later `fork` shares the pages rather than COWing them (P1's mechanism).
  - `SHM_RDONLY` maps the pages read-only and marks the VMA read-only-shm.
  - The mapping is recorded in the VMA (`vm_area.shm`), and `nattch` is incremented.
  - A non-zero address hint returns `-EINVAL` in P2.
  - A marked segment refuses new attaches with `-EIDRM` (D2).
- **Detach.** The attachment ends through any of these paths, and each one decrements `nattch`:
  - `shmdt(addr)`;
  - `munmap` of the same region;
  - the address space's last `mm_put`;
  - `exec` (which replaces the address space).
- **Fork.** `fork` copies the VMA table (DDR-1178 M7) and increments `nattch` for every shm VMA it copies.
- **Destroy.** `shmctl(shmid, IPC_RMID, 0)` marks the segment. The object also counts its **holders**: the capability-table slots that name it. Minting a capability adds one; `fork` copies one for each RES_SHM slot it copies; revoking a capability, or freeing the last reference to a table, removes one. The object is destroyed when `nattch == 0 && (marked || holders == 0)`. On destruction it drops its own frame references, and the frames go back to the PMM once no page table holds them. Without the holders rule, a creator that exits without `IPC_RMID` would pin one of the 32 slots forever, and any process could exhaust the table that way. The rule is the same as for a file whose last descriptor has closed. `IPC_STAT`/`IPC_SET` return `-EINVAL` in P2; that is recorded and not implemented.
- **`IPC_RMID` is not a detach** (found by `smoke-shm` arm J in CI runs 39516558534 and 39518678592). The first build's `shm_mark` re-evaluated reclaim through the attach path, so marking a segment that was still mapped dropped `nattch` from 1 to 0. That reclaimed the segment under its mapping, released the slot for reuse, and the next `shmat` returned `-EINVAL` instead of `-EIDRM`. The re-evaluation now changes no count.
- **Arm C's baseline is clamped at zero.** Unrelated frees during the private loop made its drop negative (−225 in run 39516558534, with a shared drop of 0). Subtracting that negative baseline reported a leak the shared loop never had. A real per-cycle shared leak (≥ 256 pages) still fails.
- **`mprotect` cannot defeat `SHM_RDONLY`.** `PROT_WRITE` on any part of a read-only shm VMA returns `-EACCES`.
- **Agents are refused** every shm call (`-EPERM`). A segment is memory that `aether_mem_charge` does not see, so allowing agents to create segments would bypass their memory cap. This is the option safer for isolation (Order 4, item 2).

**NSI:** 112 `SYS_SHMGET`, 113 `SYS_SHMAT`, 114 `SYS_SHMDT`, 115 `SYS_SHMCTL` (110 and 111 are DDR-1181's).

### A2.3 Gate: `smoke-shm` arms G-L (same probe, `user/shmtest.c`)

| Arm | Sentinel | What it shows |
|---|---|---|
| G | `PRADYOS_SHM_G key=-13` | `shmget` with a non-private key returns `-EACCES` |
| H | `PRADYOS_SHM_H shared=0x66` | The parent creates and attaches a segment and writes `0x55`. A child created by `fork` **after** `shmget` attaches the inherited shmid (the capability crossed the fork), then writes `0x66` through the mapping it **inherited** and exits. The parent reads `0x66` through its own attachment. |
| I | `PRADYOS_SHM_I forged=0` | A child forked **before** `shmget` receives the shmid value through a `MAP_SHARED` page (P1) and tries `shmat` on it. It also tries all 64 slots with generations 1 to 4. `forged` counts the attempts that succeeded. |
| J | `PRADYOS_SHM_J eidrm=-43 freed_ok=1` | After `IPC_RMID` on an attached segment, a new `shmat` returns `-EIDRM`, while the existing mapping still reads its value. After the last `shmdt`, `SYS_MEMINFO` free pages rise by at least the segment's 16 pages minus 4 pages of noise. |
| K | `PRADYOS_SHM_K ro_faulted=1 ro_mprotect=-13` | A child attaches with `SHM_RDONLY` and writes, so it dies on the fault. The parent's `SHM_RDONLY` attach, then `mprotect(PROT_READ\|PROT_WRITE)`, returns `-EACCES`. |
| L | `PRADYOS_SHM_L nattch_fork=1` | Attach, `fork`, the child exits **without** `shmdt`, then `IPC_RMID` and a `shmdt` in the parent. The segment is destroyed only now, with free pages rising as in J. This is the exit-path decrement. |

**Vacuity:** on the tree before P2, `SYS_SHMGET` is `-ENOSYS`. The probe prints `PRADYOS_SHM FAIL shmget=-38` (forbidden), and arms G–L are absent, so the gate is red. Arms I and the K-`mprotect` half pass trivially on an unfixed tree. They are boundary arms, meaningful only together with H, and recorded as such.

**Mutants (each lands on a different arm and is shown red in CI before merge):**

| Mutant | Change | Arm |
|---|---|---|
| S2-1 | the key check removed (any key creates) | G |
| S2-2 | `fork` COWs shm pages (no `PTE_SW_SHMEM` on the shmat mapping) | H |
| S2-3 | `shmat` resolves the shmid in a global table, ignoring the caller's capabilities | I |
| S2-4 | `shmat` ignores `marked` | J (`eidrm`) |
| S2-5 | `SHM_RDONLY` ignored (mapped RW) | K (`ro_faulted`) |
| S2-6 | the exit path does not decrement `nattch` (the segment is never destroyed) | L |
| S2-7 | the last holder's exit does not drop `holders` (an unattached, never-RMID'd segment leaks its slot) | M: `PRADYOS_SHM_M reclaimed=1`. A child creates a segment, does not attach it, and exits. The parent then counts free pages and expects them back |

**Arm H vacuity, found before any S2 mutant ran (2026-10-26).** H first had the child write `0x66` through a fresh `shmat`. A fresh attach maps the frames writable whatever `fork` does to the inherited PTEs, so with S2-2 the child's write still reached the shared frame and H passed: the arm could not see the bug it names. The child now writes through the inherited mapping. With S2-2 the inherited PTE lacks `PTE_SW_SHMEM`, so `vmm_fork_address_space_cow` marks it COW (`kernel/mm/vmm_cow.c`, the `PTE_SW_SHMEM` test), the child's write is copied into a private frame, and the parent reads `0x55`. The child also detaches the inherited mapping, so J still does not depend on the exit path. This is reasoned from the code, **not verified** until S2-2 is shown red in CI.

**S2 mutant runs (2026-10-27).** Each mutant is the `e45ded4` tree plus one change; `e45ded4` itself is green ([40325560933](https://github.com/prady4the4bady/prady4os/actions/runs/40325560933)). Every run failed in the boot step at `smoke-shm`, not in the build, and only the designed arm differs from the sentinel:

| Mutant | Run | Red arm, as printed |
|---|---|---|
| S2-1 | [40456868078](https://github.com/prady4the4bady/prady4os/actions/runs/40456868078) | G `key=66` (want `-13`) |
| S2-2 | [40456870067](https://github.com/prady4the4bady/prady4os/actions/runs/40456870067) | H `shared=0x55` (want `0x66`); the inherited-mapping fix above is what makes this arm see the bug |
| S2-3 | [40456872506](https://github.com/prady4the4bady/prady4os/actions/runs/40456872506) | I `forged=1` (want `0`) |
| S2-4 | [40456874533](https://github.com/prady4the4bady/prady4os/actions/runs/40456874533) | J `eidrm=584115617792 freed_ok=0` (the attach of a marked segment succeeded and returned an address) |
| S2-5 | [40456877095](https://github.com/prady4the4bady/prady4os/actions/runs/40456877095) | K `ro_faulted=0` |
| S2-6 | [40456879719](https://github.com/prady4the4bady/prady4os/actions/runs/40456879719) | L `nattch_fork=0` (want `1`) |
| S2-7 | [40456881815](https://github.com/prady4the4bady/prady4os/actions/runs/40456881815) | M `reclaimed=0` |

**Re-run on the merged head `551b562`** (A2.4 plus `dev/phase1` through `9306963`; fixed tree green on [41405761144](https://github.com/prady4the4bady/Prady4OS/actions/runs/41405761144)). Each mutant was re-applied on `551b562`. Each is red in `smoke-shm` only, on its own arm, with the same values as above:

| Mutant | Run on `551b562` | Red on |
|---|---|---|
| S2-1 | [41406018550](https://github.com/prady4the4bady/Prady4OS/actions/runs/41406018550) | G `key=66` |
| S2-2 | [41413539496](https://github.com/prady4the4bady/Prady4OS/actions/runs/41413539496) | H `shared=0x55`; the probe then stops at `j_mapping_lost=85`, a consequence of the same COW |
| S2-3 | [41413543019](https://github.com/prady4the4bady/Prady4OS/actions/runs/41413543019) | I `forged=1` |
| S2-4 | [41419522047](https://github.com/prady4the4bady/Prady4OS/actions/runs/41419522047) | J `eidrm=584115617792 freed_ok=0` |
| S2-5 | [41419526150](https://github.com/prady4the4bady/Prady4OS/actions/runs/41419526150) | K `ro_faulted=0` |
| S2-6 | [41419530619](https://github.com/prady4the4bady/Prady4OS/actions/runs/41419530619) | L `nattch_fork=0` |
| S2-7 | [41426415211](https://github.com/prady4the4bady/Prady4OS/actions/runs/41426415211) | M `reclaimed=0` (`[shm] M drop=16`) |

### A2.4 A removed segment keeps its slot until no shmid names it (found by S3-5 surviving)

**Defect, from a real artefact.** S3-5 (`fd_clone` takes no hold) stayed green in run [40322396520](https://github.com/prady4the4bady/prady4os/actions/runs/40322396520). The diagnostic build `mut/shm3-S3-5-diag` (`83bb3e9`, run [40470702205](https://github.com/prady4the4bady/prady4os/actions/runs/40470702205), never merged) printed every hold and put. In arm R the object's `holders` went from 1 to 4 across the `fork`, although the mutant's `fd_clone` took no hold, so the parent's `close` left 3 and the child's `mmap` succeeded. The three extra holds are `cap_fork` (`kernel/cap.c`) copying three RES_SHM capabilities: the three shmids the probe removes with `IPC_RMID` earlier in the same process (`id` of arms H-J, `id2` of K, `id3` of L).

**Mechanism.** `put_and_reclaim` destroyed a marked, unmapped segment and set `used = 0` while capabilities still named it (`marked || holders == 0`). A capability stores only the slot index, so after `shm_obj_create` reused the slot (it takes the lowest free one), every old shmid resolved to the **new** object:

- `shmat(old_shmid)` mapped the new object, which can belong to another process. That breaks D1(c): a segment is reachable only through a capability its creator's table was given.
- The old capabilities' holds and unholds (`cap_fork`, table teardown) counted against the new object, so its `holders` drifted. In arm R that kept a P3 object alive after its last fd had closed, which is why S3-5 could not turn R red.

**Change.** A marked segment still gives its frames back at its last detach, so memory is reclaimed exactly when it was before. If capabilities still name it, the slot then stays **dead** (`used = 3`) instead of free: `shmat` and `IPC_RMID` refuse it (`-EINVAL`, as Linux does for a removed id), holds and unholds still count on it, and the last unhold frees the slot. `shm_hold` counts on any slot in use (1, 2 or 3), so a hold and its unhold always land on the same object. A process can pin at most its own capability slots this way (64), and they go when it exits.

**New arm T** (`user/shmtest.c`, after M): create a segment, remove it before any attach (it is reclaimed at once), create a second segment and write `0x99` into it, then `shmat` the first shmid. Want `PRADYOS_SHM_T stale=-22`. On the unfixed tree the second segment reuses the first one's slot, the attach succeeds, and the arm prints the byte it reads (`stale=153`). That is the vacuity check: the arm is red on the tree before this change.

**Mutant S2-8:** `put_and_reclaim` frees the slot of a marked segment whatever `holders` is (the pre-A2.4 rule). Want red on T only.

**S2-8 run:** [40537012413](https://github.com/prady4the4bady/prady4os/actions/runs/40537012413) (`3cbdf3f`), red on T only: `PRADYOS_SHM_T stale=153` (want `-22`), every other arm on the sentinel. The head with A2.4, `45af5b6`, is green ([40536969423](https://github.com/prady4the4bady/prady4os/actions/runs/40536969423)) with `stale=-22`. A2.4 is **gated by arm T**.

**S2-8 re-run** on `551b562`: [41426419602](https://github.com/prady4the4bady/Prady4OS/actions/runs/41426419602), red on T only, `PRADYOS_SHM_T stale=153` (want `-22`). The fixed tree passes T on [41405761144](https://github.com/prady4the4bady/Prady4OS/actions/runs/41405761144). A2.4 is gated by arm T, in QEMU TCG only, and is **not verified on real hardware**.

## Amendment 3 (2026-10-21 UTC, after Amendment 2): P3 design, POSIX `shm_open(SHM_ANON)` under D1(c)

**Security boundary: review before merge.** P3 lets processes share memory through a file descriptor.

### A3.1 The interface

- **`SYS_SHM_OPEN` = NSI 116** takes `(name, oflag, mode)` and returns an fd or `-errno`.
  - `name` must be exactly `SHM_ANON`, which is `(const char *)1` (the FreeBSD form). Any other value is `-EACCES`, including a real string and `NULL` (D1(c)). The kernel never dereferences `name`.
  - The access mode in `oflag` must be `O_RDWR`; any other mode is `-EINVAL`. Without a namespace, a read-only fd could only name an object nobody can fill. `O_CREAT` and `O_EXCL` are accepted and ignored: every call creates a new object. Any other `oflag` bit is `-EINVAL`. `mode` is ignored, because nothing can open the object by name.
  - Agents get `-EPERM`, as in A2.2.
- **No `shm_unlink` syscall.** With no names there is nothing to unlink; a libc returns `-ENOENT` itself. This is recorded, not hidden.
- **Reachability.** The fd reaches other processes only by `fork` (and by `SYS_THREAD_CREATE`, which copies the fd table). The kernel has no fd-passing operation today, so D1(c)'s "explicit fd passing" is not reachable either. A future fd-passing operation must take a hold on the object, as `fork` does.

### A3.2 Objects and lifetime (D3: reuses the A2.2 table)

- **New fd kind `FD_SHM`.** The entry stores the object index in `fd_entry.mnt`, which only `FD_VFS` uses otherwise. That keeps `struct fd_entry`, and with it `struct tcb`, the same size, so the pinned signal offsets (DDR-1181) do not move.
- **Created empty.** `shm_open` reserves an object with `npages == 0` and one holder (the fd). `ftruncate(fd, len)` sets the size **exactly once**: `0 < len <= 1 MiB`, rounded up to pages, frames allocated zeroed. A second `ftruncate`, or `ftruncate` with 0 or more than 1 MiB, is `-EINVAL`. Growing or shrinking a mapped object would need to remap every attachment; P3 does not do it. A concurrent second `ftruncate` is refused by a `filling` state taken under the table lock.
- **Holders are fd entries.** Each `FD_SHM` entry holds the object once:
  - `shm_open` takes the first hold;
  - `fork`/thread creation (`fd_clone`) and `dup2` take one more for each entry copied;
  - `close`/`fd_free` (which also runs at exit) drops one.
  An object is destroyed when `nattch == 0 && holders == 0`, the A2.2 rule with `marked` never set. A POSIX object is anonymous, so it is already "unlinked".
  - An object that was never sized is reclaimed the same way, so the A2.2 condition no longer requires `npages != 0`.
- **`mmap(addr, len, prot, MAP_SHARED, fd, off)`** on an `FD_SHM` fd maps pages `off/4096 .. off/4096 + npages(len) - 1` of the object:
  - `off` must be page-aligned, and `off + len` must not exceed the object's size. An unsized object is `-EINVAL`.
  - It is an attach (`shm_attach_begin`, `vm_area.shm`), so `munmap`, `exec`, the last `mm_put` and `fork` all use the A2.2 paths unchanged. Each frame gets `pmm_incref` and `PTE_SW_SHMEM`.
  - `PROT_WRITE` is allowed because the fd is always `O_RDWR`. `PROT_EXEC` is refused (W^X, unchanged).
  - **`MAP_PRIVATE` on an `FD_SHM` fd is `-EINVAL`.** A private copy of a shared object is a COW path that P3 does not need.
  - The address hint follows the `mmap` rules, not `shmat`'s: 0 means the arena bump pointer, and a non-zero hint must be page-aligned, inside the arena and not overlapping.
  - Shared pages are not charged to `aether_mem_charge` (agents cannot create them), as in A2.2.
- **Other fd operations.**
  - `fstat` reports `st_size = npages * 4096`.
  - `read`, `write`, `lseek`, the io_uring reads and writes, and `mmap` of a non-`FD_SHM` fd with `MAP_SHARED` keep their existing answers: every one already returns `-EBADF`, `-EINVAL` or `-ENODEV` for a kind it does not know. P3 adds no byte I/O on a shm fd.
  - `epoll` reports a shm fd as not pollable, as it does for every non-pipe, non-console kind.

### A3.3 Gate: `smoke-shm` arms N-S (same probe)

| Arm | Sentinel | What it shows |
|---|---|---|
| N | `PRADYOS_SHM_N fdshared=0x66` | The parent runs `shm_open`, sizes the object to 2 pages, maps both and writes `0x55` to page 1, then forks. The child maps **only page 1** through the inherited fd (`off = 4096`), checks that it reads `0x55`, writes `0x66` and exits. The parent reads `0x66` through its own mapping. This proves that the fd names the object and that `off` selects the page. |
| O | `PRADYOS_SHM_O rejects=4` | `shm_open` with a real name is `-EACCES`; with `NULL` it is `-EACCES`; with `O_RDONLY` it is `-EINVAL`; with an unknown `oflag` bit it is `-EINVAL`. |
| P | `PRADYOS_SHM_P rejects=7` | All of these are `-EINVAL`: `mmap` before `ftruncate`; a second `ftruncate`; `ftruncate(0)`; `ftruncate` over 1 MiB on a fresh object; `mmap` with `off + len` past the size; `MAP_PRIVATE` on the fd; `mmap` with `len = -1` (A3.4). |
| Q | `PRADYOS_SHM_Q cycles=64 opened=64 leaked_ok=1` | 64 cycles of: `shm_open`, `ftruncate(4 pages)`, `mmap`, write, `close` **while mapped**, read back, then `munmap`. The value must survive the `close` (the mapping keeps the object). The table has 32 slots, so a leak of one object per cycle fails `opened` at cycle 33. The free-page drop must stay under 128 pages; a frame leak would be 256 pages. |
| R | `PRADYOS_SHM_R child_hold=0x77` | The parent sizes and maps an object, writes `0x77` and **unmaps it before forking**, so the child inherits the fd but no mapping. The child waits on a P1 flag page. The parent **closes its fd**, then sets the flag. The child, whose fd is now the only holder, maps the object and reads `0x77`. This proves `fd_clone` takes a hold. An inherited mapping would keep the object alive by itself, which is why there is none. |
| S | `PRADYOS_SHM_S dup_hold=0x88` | `dup2(fd, 20)`, `close(fd)`, then `mmap` through fd 20 reads the `0x88` written earlier through the old mapping (unmapped before the `mmap`). This proves `dup2` takes a hold. |

**Vacuity:** before P3, `SYS_SHM_OPEN` is `-ENOSYS`. The probe prints `PRADYOS_SHM FAIL shm_open=-38` (forbidden), and arms N–S are absent, so the gate is red. Arms O and P are boundary arms. They pass trivially only if `shm_open` itself fails, and that case is caught by the `FAIL` line.

**Mutants (each lands on a different arm and is shown red in CI before merge):**

| Mutant | Change | Arm |
|---|---|---|
| S3-1 | `mmap` ignores `off` (always maps from page 0) | N |
| S3-2 | the name check accepts any pointer | O |
| S3-3 | `ftruncate` may resize an object that is already sized | P |
| S3-4 | `fd_free` does not drop the hold (every closed object leaks its slot) | Q (`opened`) |
| S3-5 | `fd_clone` does not take a hold | R |
| S3-6 | `dup2` does not take a hold | S |

**S3 mutant runs.** S3-1 [40322278330](https://github.com/prady4the4bady/prady4os/actions/runs/40322278330) red on N (`fdshared=0x00`); S3-2 [40322308133](https://github.com/prady4the4bady/prady4os/actions/runs/40322308133) red on O (`rejects=2`); S3-3 [40322337823](https://github.com/prady4the4bady/prady4os/actions/runs/40322337823) red on P (`rejects=4`); S3-4 [40322367227](https://github.com/prady4the4bady/prady4os/actions/runs/40322367227) red on Q (`opened=30`); S3-6 [40322426930](https://github.com/prady4the4bady/prady4os/actions/runs/40322426930) red on S (`dup_hold=0x00`). **S3-5 survived** ([40322396520](https://github.com/prady4the4bady/prady4os/actions/runs/40322396520), green). The cause was the P2 defect recorded in A2.4, not arm R: the probe's three removed SysV shmids held the P3 object through `cap_fork`, so the child's fd was not the only hold. With A2.4 those shmids pin their own dead slots. **S3-5 re-run** on the merged head `c5a26b8` (mutant `759dc46`, run [40545214807](https://github.com/prady4the4bady/prady4os/actions/runs/40545214807)): red on R only, `PRADYOS_SHM_R child_hold=0x00` (want `0x77`), every other arm on the sentinel. S3-1 to S3-6 are now all red on their designed arms.

**S3 re-run on the merged head `093bbe4`** (P3 plus #78 `551b562`, which carries `dev/phase1` through `9306963`; fixed tree green on [41434076815](https://github.com/prady4the4bady/Prady4OS/actions/runs/41434076815)). Each mutant was re-applied on `093bbe4`, builds with 0 warnings, and gives its own `kernel.bin`. Only `smoke-shm` (shard 0) failed in each run:

| Mutant | Run on `093bbe4` | Red on |
|---|---|---|
| S3-1 | [41434433917](https://github.com/prady4the4bady/Prady4OS/actions/runs/41434433917) | **N** `fdshared=0x00`. P is red too (`rejects=5`, want `6`). The mutated line sets `first = 0`, and `shm_fd_map` uses `first` in its size bound, so the `off + len` past-the-size `mmap` in P is accepted. That is the same defect, seen by a second arm. |
| S3-2 | [41434436902](https://github.com/prady4the4bady/Prady4OS/actions/runs/41434436902) | **O** only, `rejects=2` |
| S3-3 | [41441841656](https://github.com/prady4the4bady/Prady4OS/actions/runs/41441841656) | **P** only, `rejects=4` |
| S3-4 | [41443116765](https://github.com/prady4the4bady/Prady4OS/actions/runs/41443116765) | **Q** `opened=25` (want `64`). The probe then stops at `PRADYOS_SHM FAIL r_open=-28` (`-ENOSPC`): the leaked objects fill the table, so R cannot open one |
| S3-5 | [41445828110](https://github.com/prady4the4bady/Prady4OS/actions/runs/41445828110) | **R** only, `child_hold=0x00` |
| S3-6 | [41447493592](https://github.com/prady4the4bady/Prady4OS/actions/runs/41447493592) | **S** only, `dup_hold=0x00` |

Every arm N–S is shown red on this head, each by the mutant designed for it. QEMU TCG only; **not verified on real hardware**.

### A3.4 A length that wraps to zero pages (CodeRabbit 4443548860)

**Defect, found in review.**
- `sys_mmap_locked` refused only `len == 0`.
- For `len` within a page of 2^64, `(len + PAGE_SIZE - 1) / PAGE_SIZE` wraps to 0. `shm_fd_map` then:
  - takes an attach (`shm_attach_begin`);
  - passes the size check (`first + 0`);
  - stores an area with `npages = 0`.
- Every VMA walker treats `npages == 0` as a free slot: `munmap`, `shm_release_vmas` and `shm_fork_vmas`. So the attach is never detached, `nattch` never reaches 0, and the slot leaks.
- One unprivileged process can fill the 32-slot table this way.

**Fix.** `len > VMM_MMAP_TOP - VMM_MMAP_BASE` is `-EINVAL`, checked before any path rounds `len` to pages.
- No mapping can exceed the arena anyway; `mmap_pick_base` already refused larger ones with `-EINVAL`.
- This covers every `mmap` path: anonymous, private file, `FD_SHM` and the P4 file path.

**Gate.** Arm P gains `mmap(len = -1)` on the fd and now wants `rejects=7`.
- **Vacuity:** on the tree before the fix the call returns an address, so it is not counted, and P prints `rejects=6`: red.
- **Mutant S3-7:** remove the bound. It must be red on P.

**Runs on `6943d97`** (fixed tree green on [41464838400](https://github.com/prady4the4bady/Prady4OS/actions/runs/41464838400), P `rejects=7`):
- **S3-7** ([41464834933](https://github.com/prady4the4bady/Prady4OS/actions/runs/41464834933)): red on P only, `rejects=6`. Every other arm A–T is on its sentinel.
- **S3-3 re-run** ([41475898640](https://github.com/prady4the4bady/Prady4OS/actions/runs/41475898640)): re-applied on `6943d97` because arm P changed. Red on P only, `rejects=5`.
- **S3-1, S3-2, S3-4, S3-5 and S3-6 were not re-run.** They stand on their runs on `093bbe4`. The only kernel change since then is this length bound: none of them touches it, and none of arms N, O, Q, R or S passes a length above the arena. That is a reading of the code, stated as such.

**NSI:** 116 `SYS_SHM_OPEN`.

## Amendment 4 (2026-11-02 UTC, after Amendment 3): P4 design, shared file mappings with write-back

Status: **design**. The code follows in the same PR. P4 stacks on P3 (#80). It reuses the A2.2 object table and the A3.2 attach path.

**Security boundary: review before merge.** A writable shared file mapping writes the file later, with the capability captured at `mmap` (D4). The write can happen long after the fd is closed.

### A4.1 What exists today (read from `ead9246`)

- **The `mmap` path.** File-backed `MAP_SHARED` is refused (`sys_mmap.c`, the DDR-1178 P1 comment). File-backed `MAP_PRIVATE` is eager: DDR-1112 reads each page with `vfs_read` at `mmap` time.
- **No page cache.** `read(2)` and `write(2)` go to the filesystem driver directly (`sys_io.c` → `vfs_write`). Each `open` has its own heap `struct vfs_file`, holding its own copy of `size`.
- **Which filesystems can write.**
  - FAT32 and pdrive write in place.
  - SFS is append-only (D5).
  - ext4 has no `write` op.
- **There is no common inode number.** `struct vfs_file` has `cookie`, `dirent_clus` and `dirent_off`:
  - ext4 and SFS store the inode in `cookie`. SFS also stores a snapshot root in `dirent_clus`.
  - FAT32 stores the file's first cluster in `cookie`. That is 0 for an empty file and changes on the first write, so it does not identify the file. The file is identified by `dirent_clus` and `dirent_off`.
  - pdrive stores its table index in `cookie`.
- **Locks.**
  - `mm_lock` is a `kmutex` (sleeping).
  - `mnt_lock` is a sleeping mutex.
  - `g_shm_lock` is a spinlock taken with IRQs off.
  - The filesystem paths copy through kernel buffers and never take an `mm` lock. So the order `mm_lock` → `mnt_lock` is safe, and no I/O may happen under `g_shm_lock`.
- **Where an address space is released.** The last `mm_put` (and with it `shm_release_vmas`) runs in `wait4` (`sys_wait.c`), in `exec` (`elf.c`) and in the reaper thread (`sched.c`). All three are thread context with IRQs on, so they may block.
- **`fat32_write` always rewrites the file's directory entry** (`dirent_update` at `dirent_clus`/`dirent_off`, step 4), even for a write inside the file. So a write through a stale `struct vfs_file` corrupts the disk:
  - after `unlink`, the clusters and the dirent slot are free and may belong to another file;
  - after `rename`, the old dirent slot is free;
  - after a truncate, the clusters past the new end are free.
- **`vfs_write` charges the calling thread's write budget** (ADR-032: a 1 MiB bucket that refills at 256 KiB per tick). It returns `-EAGAIN` when the bucket is short. Write-back at reap time runs as the parent or the reaper thread, not as the mapper.
- **The fd access mode is stored but not enforced.** `fd_entry.flags` keeps the open flags, but `sys_file.c` notes that every `FD_VFS` fd is writable. Enforcement is DDR-1182 (#72, not merged).
- **A process reaches only its root mount** (`vfs_rename`: no path-to-mount resolution). `shmtest` runs on `vfs_default_mnt()`.

### A4.2 Interface

- **`mmap(addr, len, prot, MAP_SHARED, fd, off)` on an `FD_VFS` fd.**
  - **Agents** get `-EPERM`, as in A2.2.
  - **The filesystem must name the file.**
    - The driver must provide a new optional `vfs_fs_ops.ident(ctx, f) -> uint64_t`:
      - FAT32: `dirent_clus << 16 | dirent_off`;
      - ext4 and SFS: `cookie | (uint64_t)dirent_clus << 32`;
      - pdrive: `cookie`.
    - The key is `(mnt, ident)`.
    - A driver without `ident` gives `-ENODEV`.
  - **Bounds.**
    - `off` must be page-aligned.
    - `off + len` must not pass the file's size rounded up to a page; otherwise `-EINVAL`. POSIX would allow the mapping and raise `SIGBUS` on access; refusing is simpler and cannot corrupt the file.
    - A file larger than `SHM_MAX_PAGES` (1 MiB) is `-EFBIG`.
    - An empty file is `-EINVAL`.
  - **Read and write permission.**
    - Any mapping needs an fd open for reading: an `O_WRONLY` fd is `-EACCES`.
    - `PROT_WRITE` needs the fd open `O_RDWR` and a filesystem with in-place write. A new `vfs_fs_ops.shared_write` flag is set by FAT32 and pdrive only. Otherwise it is `-EACCES`. That covers an `O_RDONLY` fd, an SFS file (D5) and an ext4 file.
    - The check reads `fd_entry.flags`. Until DDR-1182 enforces the access mode for `write(2)` too, it is consistent with the open mode but is not, on its own, a boundary.
  - **Write budget (ADR-032).**
    - A `PROT_WRITE` mapping prepays its whole size, `npages * 4096` bytes, from the mapper's write bucket at `mmap`. If the bucket is short, `mmap` fails with `-EAGAIN`.
    - Write-back at `munmap`, `exec` and release is then not charged again. That write-back is at most the area's size, which was prepaid, so it cannot be used to write faster than the bucket allows.
    - `msync` charges its own writes in the caller's context, like `write(2)`.
    - `PROT_EXEC` is refused (W^X).
    - A mapping made without write permission is recorded as read-only (`vm_area.shm_ro = 1`). `mprotect` then cannot add `PROT_WRITE`; this is the A2.2 rule, gated by arm K.
- **New `SYS_MSYNC` = NSI 119**, `(addr, len, flags)`.
  - **Arguments.**
    - `addr` must be page-aligned (`-EINVAL`).
    - `[addr, addr + len)` must lie inside one mapping (`-ENOMEM`, as POSIX). `len` 0 checks the page at `addr`.
  - **Flags.**
    - Exactly one of `MS_ASYNC` (1) or `MS_SYNC` (4) must be set. Both write back synchronously before returning.
    - `MS_INVALIDATE` (2) is `-EINVAL`: not supported (§A4.6).
  - **Effect.**
    - It writes back the dirty pages of that range.
    - On an anonymous, SysV or P3 mapping there is nothing to write, and it returns 0.
    - A write-back error returns `-EIO`.
- **A mapped file is busy.** While a FILE object exists for a file, each of these is `-EBUSY`:
  - `ftruncate`;
  - `open` with `O_TRUNC` (which is unlink plus create, DDR-782);
  - `unlink`;
  - `rename`, of the file or onto it.

  Each would make a later write-back use freed clusters or a freed directory entry (§A4.1). POSIX allows all four; Linux keeps the pages alive and raises `SIGBUS` past the end. Until a page cache owns file identity, P4 refuses them.

### A4.3 Objects and frames (D5's frame table, on the A2.2 table)

- **One object per mapped file.**
  - A file object is an A2.2 slot with `kind = FILE`, holding:
    - the key;
    - a copy of the `struct vfs_file` taken at first `mmap`;
    - `npages = ceil(size / 4096)`, with the frames read eagerly from the file. The tail past `size` is zero.
  - A file object has no id and no fd hold. `holders` stays 0, so it lives while it is attached (`nattch > 0`). The last detach frees it, and the next `mmap` reads the file again.
  - It shares the 32-slot table with SysV and P3 objects.
- **Lookup.**
  - `mmap` finds a live object with the same key under `g_shm_lock` and attaches it. So two mappers of one file share frames: each sees the other's writes at once, with no `msync`. This is what D5 asks for.
  - If there is no such object, a new one is created. Its frames are read with `vfs_read` **outside** `g_shm_lock`. The slot is published only after the read.
  - Two concurrent first mappers can both read. The second to publish finds the first's object, drops its own frames, and attaches the first's object.
- **The attach is the A3.2 path.** It goes through `shm_attach_begin` and `vm_area.shm`, with `pmm_incref` and `PTE_SW_SHMEM` on each frame. So `fork`, `exec`, `munmap` and the last `mm_put` reuse the A2.2 detach paths. A4.4 adds a write-back step in front of them.
- **The capability (D4).**
  - Each attaching `vm_area` records the fd's `cap` in a new field `vm_area.wcap`. It is 0 for a read-only mapping.
  - `fork` copies it with the area.
  - Write-back of an area's dirty pages uses that area's `wcap` and the object's `vfs_file` copy.
  - `vm_area` lives in `struct mm`, not in `struct tcb`, so the pinned `tcb` offsets (DDR-1181) do not move.

### A4.4 Write-back

- **Dirty means the PTE's D bit.** Each mapper has its own PTE for a shared frame, so each area writes back the pages that *it* dirtied.
- **Steps, per page in the range:**
  1. Under `mm_lock`, test and clear D in that area's PTE.
  2. Invalidate the TLB entry on every CPU that may hold it: the existing shootdown for this `mm`.
     - Without this step the CPU keeps a writable TLB entry whose D bit is already set. A later write would not set D in the PTE again, and the next write-back would miss it.
  3. Write the page with `vfs_write(wcap, &obj->file, page_off, frame, n)`. `n` is clipped to the size recorded at first `mmap`, so write-back never extends the file.
- **Write-back runs at:**
  - `msync`;
  - `munmap`, before the unmap;
  - **`exec`**, from `shm_writeback_vmas` before the point of no return. After that point `mm->cr3` is already the new image's.
  - **The last `mm_put`**, from `shm_writeback_vmas` just before `shm_release_vmas` detaches.
  - At `munmap` and release only the D bit is tested: the mapping goes away right after, so nothing needs clearing.
  - It runs under `mm_lock` (sleeping), never under `g_shm_lock`. The lock order is `mm_lock` → `mnt_lock`.
- **Errors.**
  - `msync` returns `-EIO`.
  - `munmap` and exit cannot report one. They print `[shm] writeback failed rc=<n>` once per page and continue. That line is not forbidden; arms FD and FE detect lost data by reading the file.
- **When exit data lands.** Write-back at exit happens when the address space is released. For a child that is when its parent reaps it in `wait4`, or when the reaper thread frees an orphan. A reader that reads the file before the writer is reaped can see old data. A second mapper of the file sees the new data at once, because the frames are shared.

### A4.5 Gate: `smoke-shm` arms FA-FG (same probe, now FAT-rooted)

- `main.c` spawns `shmtest` with `root_mnt` set to the FAT volume, as it does for the FAT32 probes (`fat32mc`).
- Arms A–T touch no files, so the root change does not reach them.
- The probe creates `/SHMFILE.BIN` and, for FG, `/SHMFG.BIN`.

| Arm | Sentinel | What it shows |
|---|---|---|
| FA | `PRADYOS_SHM_FA msync=0xa1` | Write 2 pages of `0x00` with `write(2)`. Map them `MAP_SHARED` RW, store `0xa1` at page 1, `msync(MS_SYNC)`. `pread` page 1 through a **second fd** reads `0xa1`. |
| FB | `PRADYOS_SHM_FB redirty=0xb2` | After FA, store `0xb2` at the same byte, `msync` again, `pread` reads `0xb2`. This shows D is cleared **and** the stale TLB entry is dropped (§A4.4 step 2). |
| FC | `PRADYOS_SHM_FC shared=0xc3` | Open the file a second time and `mmap` it again in the same process. Store `0xc3` through the first mapping. The second mapping reads `0xc3` with no `msync`, so the two `mmap`s share one object. |
| FD | `PRADYOS_SHM_FD munmap=0xd4` | Store `0xd4`, `munmap` with no `msync`, then `pread` reads `0xd4`. |
| FE | `PRADYOS_SHM_FE exit=0xe5` | A child opens and maps the file, stores `0xe5`, and exits with no `msync` or `munmap`. The parent's `wait4` reaps it, then `pread` reads `0xe5`. |
| FF | `PRADYOS_SHM_FF rejects=8` | Each of these is refused: `PROT_WRITE` on an `O_RDONLY` fd (`-EACCES`); `off + len` past the file (`-EINVAL`); on a mapped file, `ftruncate`, `open(O_TRUNC)`, `unlink` and `rename` (each `-EBUSY`); `msync` with `MS_INVALIDATE` (`-EINVAL`); `msync` of an unmapped address (`-ENOMEM`). |
| FG | `PRADYOS_SHM_FG cycles=40 opened=40` | 40 cycles of `open`, `mmap`, store, `munmap`, `close`. The table has 32 slots, so a file object that outlives its last detach fails `opened` at cycle 33. |

**Vacuity, checked against the tree before P4 (`ead9246`):**
- File-backed `MAP_SHARED` is refused there. The probe prints `PRADYOS_SHM FAIL fa_mmap=-22`, which is forbidden, and arms FA-FG are absent. The gate is red.
- Each arm reads the file through a separate fd or after a reap, never through the mapping that wrote it, so it cannot pass on the frame alone.
- **FB depends on how QEMU caches TLB entries.** If a syscall entry or exit reloads CR3 (flushing the TLB) on this kernel, S4-2 may stay green. If S4-2 is not red in CI, FB will be reported as not shown red and redesigned. It will not be counted.

**Mutants (one per arm; each must be shown red in CI before merge):**

| Mutant | Change | Arm |
|---|---|---|
| S4-1 | `msync` returns 0 without writing | FA |
| S4-2 | write-back clears D with no TLB invalidation | FB |
| S4-3 | `mmap` skips the key lookup (a new object every time) | FC |
| S4-4 | `munmap` skips write-back | FD |
| S4-5 | the last `mm_put` skips `shm_writeback_vmas` | FE |
| S4-6 | `PROT_WRITE` allowed on an `O_RDONLY` fd | FF |
| S4-7 | the last detach of a file object leaves its slot dead (`used = 3`) instead of free. A slot that stays live would be found again by its key and reused, so it would not leak. | FG |

**S4 runs** on `dda8bf0` (P4 plus #80's `6943d97`; fixed tree green on [41475702132](https://github.com/prady4the4bady/Prady4OS/actions/runs/41475702132)). Each mutant builds with 0 warnings and gives its own `kernel.bin`. Only `smoke-shm` (shard 0) failed in each red run.

| Mutant | Run | Result |
|---|---|---|
| S4-1 | [41475905429](https://github.com/prady4the4bady/Prady4OS/actions/runs/41475905429) | **red on FA** `msync=0x00`. FB is red too (`redirty=0x00`), because FB also reads the file after an `msync`. Same defect. |
| S4-2 | [41482000288](https://github.com/prady4the4bady/Prady4OS/actions/runs/41482000288) | **green: not shown red.** See below. |
| S4-3 | [41482218024](https://github.com/prady4the4bady/Prady4OS/actions/runs/41482218024) | **red on FC** only, `shared=0x00` |
| S4-4 | [41501319306](https://github.com/prady4the4bady/Prady4OS/actions/runs/41501319306) | **red on FD** only, `munmap=0x00` |
| S4-5 | [41501322606](https://github.com/prady4the4bady/Prady4OS/actions/runs/41501322606) | **red on FE** only, `exit=0x00` |
| S4-6 | [41501328002](https://github.com/prady4the4bady/Prady4OS/actions/runs/41501328002) | **red on FF** only, `rejects=7` |
| S4-7 | [41526232483](https://github.com/prady4the4bady/Prady4OS/actions/runs/41526232483) | **red on FG** only, `opened=23` |

**S4-2 stayed green, so the TLB invalidation of §A4.4 step 2 is not gated.**
- FB does gate that a second `msync` writes the newer value, and S4-1 shows FB red. It does not gate the invalidation itself.
- **Likely reason, not verified:** `msync`'s write-back blocks on disk I/O, the scheduler switches address space, and the CR3 reload drops the stale writable TLB entry. A missing `invlpg` then has no visible effect on this platform.
- The invalidation stays in the code because it is what x86 requires when no context switch intervenes. It is labelled **not gated**.

**Why these runs still stand on the current head.** Since `dda8bf0` the only kernel change is #80's DDR-1196 arena bound in `mmap_pick_base`. Arms FA–FG pass `addr = 0` and lengths of a few pages, so they never reach the new branch. None of the S4 mutants touches it. That is a reading of the code, stated as such.

### A4.6 Not in P4, recorded here

- `MS_INVALIDATE`, so `write(2)` does not reach an existing mapping. Until a page cache exists (checklist "page cache with dirty-page write-back"), `read(2)` and `write(2)` are not coherent with a shared mapping:
  - `read(2)` sees mapped writes only after write-back;
  - a page dirtied both through the mapping and by `write(2)` ends as the mapping's copy at write-back.
- Lazy (demand) paging, mappings past end of file, files over 1 MiB, and resizing a mapped file.
- Write-back of SFS (append-only) and ext4 (no write), which are read-only shared mappings only.

**Not gated, and why:**
- **`PROT_WRITE` on SFS or ext4 is `-EACCES`** (D5). `shmtest` is FAT-rooted and a process reaches only its root mount, so it cannot open an SFS or ext4 file. The refusal is the same `shared_write` test that FF's FAT32 checks pass through, but the SFS and ext4 values of the flag are read from the code, not run.
- **The prepay `-EAGAIN`.** Reaching it needs the bucket drained first. The probe does not do that, so the path is reasoned, not run.
- **Two concurrent first mappers of one file** (§A4.3). `smoke-shm` runs on one CPU.

**NSI:** 119 `SYS_MSYNC`.

### A4.7 Cross-CPU write/msync/write: arm FH, to show S4-2 red (2026-11-03, operator 6594717280 item 4)

**Does a missing flush lose a later write? Yes, on more than one CPU.** This is reasoned from the code and the x86 rules, and not yet shown in CI:
1. Thread B, on CPU 2, stores to a mapped page. The page walk sets D in the PTE. CPU 2 caches a **writable TLB entry with D already set**.
2. Thread A, of the same `mm`, on CPU 1, calls `msync`. Write-back tests D, writes the page, and clears D in the PTE (§A4.4).
3. With the shootdown, CPU 2's entry is invalidated. B's next store walks the tables and sets D again, so the next write-back writes it.
4. **Without the shootdown (S4-2),** CPU 2 keeps the stale entry. B's next store hits it, and the CPU does not set D in the PTE again, because the entry already has D. The next `msync`, `munmap` or exit sees a clean PTE and skips the page. **The second store never reaches the file.**

On one CPU this cannot happen while `msync` blocks: the write-back's disk I/O makes the CPU switch to another address space. The CR3 write flushes the TLB, so FB stays green under S4-2 (§A4.5). FH puts the second writer on another CPU, which never switches address space during the round.

**Arm FH** (same probe, `user/shmtest.c`; `smoke-shm` now boots with `QEMU_SMP=4`):
- **Setup:** the main thread A pins itself to CPU 1 (`SYS_SCHED_CPU`). It maps one page of `/SHMFH.BIN` `MAP_SHARED` and `PROT_READ | PROT_WRITE`. It creates thread B, which pins itself to CPU 2. CPU 0 is left out because the BSP runs most kernel threads.
- **One round `r`, 16 rounds:**
  1. B stores `X_r` to the page and signals.
  2. A calls `msync(MS_SYNC)`. That writes `X_r` and clears D. A then signals B.
  3. B stores `Y_r` to the same byte and signals.
  4. A calls `msync(MS_SYNC)` again, then reads the byte back with `read(2)`. A round is **lost** if the file does not hold `Y_r`.
- **B never enters the scheduler in a round.** It waits by spinning on a flag on another page, with no syscall, so CPU 2 makes no CR3 write between its two stores. A never touches the mapped page.
- **Sentinel:** `PRADYOS_SHM_FH rounds=16 lost=0 placed=16`. `placed` counts rounds in which A read CPU 1 and B read CPU 2 (`SYS_SCHED_CPU` op 0) around the stores. Fewer than 16 placed rounds fails the arm as **not placed**. A misplaced run is never counted as a pass.
- **Vacuity, checked before writing it:** on the tree before P4 the probe has failed at FA already, so FH is never reached. On the P4 tree without the shootdown (S4-2), every round whose CPU 2 entry survives loses `Y_r`.
- **The sensitivity is probabilistic.** Any CR3 write on CPU 2 between B's two stores also flushes the entry, for example a timer preemption to another address space. So under S4-2 a round can still pass. FH is red if **any** of the 16 rounds is lost. If S4-2 stays green on FH in CI, the shootdown stays labelled **not gated** and FH is reported as not shown red. It will not be counted.
- **S4-2 mutant:** unchanged (no `tlb_shoot` in `shm_vma_writeback`, no local `invlpg` in `vmm_dirty_op`).
- **QEMU only.** QEMU's TCG soft-TLB caches a writable entry the same way. Whether real CPUs hit this window is **not verified on real hardware**.

## 7. Honesty labels

**Page counts in arms J, L, M and Q (DDR-1185).** All four read `SYS_SYSINFO.free_pages`, which is system-wide (DDR-1185 §1).
- **M moves onto `pm_measure`.** Its window spans a fork, the child's run and exit, and the reap, so the scheduler runs other processes inside it. A burst that frees pages there could hide a leaked segment (a false pass). `reclaimed=1` now needs a conclusive trial with drop < `RECLAIM_MIN`; an inconclusive trial is red. The meter lines are printed as `[shm] M drop=` and `[shm] M meter`.
- **Q (P3) moves onto `pm_measure` too.** Its `Q_CYCLES` loop of `shm_open`, `ftruncate`, `mmap`, `close` and `munmap` is a long multi-syscall window, the same exposure as arm C. `leaked_ok=1` now needs a conclusive trial (`q_run`, no tcbs) with drop < `LEAK_BOUND`; printed as `[shm] Q meter`.
- **J and L stay single-pair.** Each window is one syscall (`shmdt`, preceded in L by `shmctl(IPC_RMID)`), and smoke-shm runs on one CPU (`rqcpus=1`, no `QEMU_SMP`). Foreign code can enter that window only through a timer preemption at syscall return, and a false pass would need at least `RECLAIM_MIN` (12) pages freed by someone else in that instant. That path is judged far-fetched; it is recorded here, **not** measured.

P1 is **gated by `smoke-shm` arms A-F**. P2 is **gated by `smoke-shm` arms G-M and T**; S2-1 to S2-7 are shown red in CI (A2.3), first on the A2.3 head and again on `551b562`. S2-8 is shown red on T (A2.4). P3 is **gated by `smoke-shm` arms N-S**; S3-1 to S3-6 are shown red in CI (Amendment 3). P4 is **gated by `smoke-shm` arms FA-FG** (Amendment 4): S4-1 and S4-3..S4-7 are shown red in CI; S4-2 is not, so the TLB invalidation step is **not gated**. Everything is emulated (QEMU TCG) only, and **not verified on real hardware**. This record does not close OPEN-1/2/12/13 or U1-U3.
