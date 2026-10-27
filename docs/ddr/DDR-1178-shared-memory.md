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
| P3 | POSIX `shm_open`/`shm_unlink` | Decided in Amendment 1 (D1, D3) |
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
- **Not covered by P1:** cross-process sharing between unrelated processes (P2/P3), file coherence (P4), `MAP_FIXED` replace, partial `munmap` of a shared range (refused today for every mapping; unchanged).

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

The shm code is unchanged between `e45ded4` and the current head (the later commits are merges of `dev/phase1` and of the C1.4 P1c fixes), so these runs still apply to it.

### A2.4 A removed segment keeps its slot until no shmid names it (found by S3-5 surviving)

**Defect, from a real artefact.** S3-5 (`fd_clone` takes no hold) stayed green in run [40322396520](https://github.com/prady4the4bady/prady4os/actions/runs/40322396520). The diagnostic build `mut/shm3-S3-5-diag` (`83bb3e9`, run [40470702205](https://github.com/prady4the4bady/prady4os/actions/runs/40470702205), never merged) printed every hold and put. In arm R the object's `holders` went from 1 to 4 across the `fork`, although the mutant's `fd_clone` took no hold, so the parent's `close` left 3 and the child's `mmap` succeeded. The three extra holds are `cap_fork` (`kernel/cap.c`) copying three RES_SHM capabilities: the three shmids the probe removes with `IPC_RMID` earlier in the same process (`id` of arms H-J, `id2` of K, `id3` of L).

**Mechanism.** `put_and_reclaim` destroyed a marked, unmapped segment and set `used = 0` while capabilities still named it (`marked || holders == 0`). A capability stores only the slot index, so after `shm_obj_create` reused the slot (it takes the lowest free one), every old shmid resolved to the **new** object:

- `shmat(old_shmid)` mapped the new object, which can belong to another process. That breaks D1(c): a segment is reachable only through a capability its creator's table was given.
- The old capabilities' holds and unholds (`cap_fork`, table teardown) counted against the new object, so its `holders` drifted. In arm R that kept a P3 object alive after its last fd had closed, which is why S3-5 could not turn R red.

**Change.** A marked segment still gives its frames back at its last detach, so memory is reclaimed exactly when it was before. If capabilities still name it, the slot then stays **dead** (`used = 3`) instead of free: `shmat` and `IPC_RMID` refuse it (`-EINVAL`, as Linux does for a removed id), holds and unholds still count on it, and the last unhold frees the slot. `shm_hold` counts on any slot in use (1, 2 or 3), so a hold and its unhold always land on the same object. A process can pin at most its own capability slots this way (64), and they go when it exits.

**New arm T** (`user/shmtest.c`, after M): create a segment, remove it before any attach (it is reclaimed at once), create a second segment and write `0x99` into it, then `shmat` the first shmid. Want `PRADYOS_SHM_T stale=-22`. On the unfixed tree the second segment reuses the first one's slot, the attach succeeds, and the arm prints the byte it reads (`stale=153`). That is the vacuity check: the arm is red on the tree before this change.

**Mutant S2-8:** `put_and_reclaim` frees the slot of a marked segment whatever `holders` is (the pre-A2.4 rule). Want red on T only.

This is checked by the arm in CI once it runs; until S2-8 is shown red, A2.4 is **not verified**.

## 7. Honesty labels

**Page counts in arms J, L and M (DDR-1185).** All three read `SYS_SYSINFO.free_pages`, which is system-wide (DDR-1185 §1).
- **M moves onto `pm_measure`.** Its window spans a fork, the child's run and exit, and the reap, so the scheduler runs other processes inside it. A burst that frees pages there could hide a leaked segment (a false pass). `reclaimed=1` now needs a conclusive trial with drop < `RECLAIM_MIN`; an inconclusive trial is red. The meter lines are printed as `[shm] M drop=` and `[shm] M meter`.
- **J and L stay single-pair.** Each window is one syscall (`shmdt`, preceded in L by `shmctl(IPC_RMID)`), and smoke-shm runs on one CPU (`rqcpus=1`, no `QEMU_SMP`). Foreign code can enter that window only through a timer preemption at syscall return, and a false pass would need at least `RECLAIM_MIN` (12) pages freed by someone else in that instant. That path is judged far-fetched; it is recorded here, **not** measured.

P1 is **gated by `smoke-shm` arms A-F**. P2 is **gated by `smoke-shm` arms G-M and T**; S2-1 to S2-7 are shown red in CI (A2.3). A2.4 (arm T) is **not verified** until S2-8 is shown red. P3 and P4 are **not started**. Everything is emulated (QEMU TCG) only, and **not verified on real hardware**. This record does not close OPEN-1/2/12/13 or U1-U3.
