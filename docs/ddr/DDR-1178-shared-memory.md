# DDR-1178: Shared memory (checklist C1.3)

Status: **DESIGN ONLY.** This record contains no kernel code. Every statement about the finished kernel is **not verified** until a `pradyos-ci` run link is attached. Platform: QEMU TCG only. **Not verified on real hardware.**

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
| P2 | SysV shared memory (`shmget`/`shmat`/`shmdt`/`shmctl`) | **Yes**, §6 D1 and D2 |
| P3 | POSIX `shm_open`/`shm_unlink` | **Yes**, §6 D1 and D3 |
| P4 | Shared file mappings with write-back (`msync`, plus write-back at `munmap`/exit) | **Yes**, §6 D4 and D5 |

P1 creates no new way for unrelated processes to reach each other's memory. Only the process tree that `fork`ed from the mapper shares the pages, which is what POSIX anonymous shared memory means. P2-P4 do create new channels, so they wait for the owner.

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

**M3 (no `pmm_incref` on the shared fork path)** is run too, but it lands on **arm A**. When the child exits, the frame's only reference is dropped, and `pmm_free_pages` stamps it with `PMM_POISON` (`kernel/mm/pmm.c:227`). The parent then reads poison instead of `0x22`. So it is extra coverage on A, not a separate arm. Each mutant runs on its own `mut/ddr1178-*` branch, its tip is tagged `evidence/ddr1178-mN`, and every run is listed here with its link.

## 5. Risks

- **Bit 52 and NX.** NX is bit 63, so they don't collide. Every path that masks PTE flags with `0xFFF` (for example `vmm_cow.c` childflags) **drops bit 52**. Three such sites were found:
  - `vmm.c:245` (`map_core`: `flags & 0xFFF`, so **every** `vmm_map_in` would drop bit 52);
  - `vmm.c` `vmm_protect_range` pass 2 (preserves only `COW`, `SHARED`, `PWT` and `PCD`; found by review on #62);
  - `vmm_cow.c:80,95` (fork childflags).

  All three must carry `PTE_SW_SHMEM`, and arm E gates the `mprotect` one. The fork walk must carry `PTE_SW_SHMEM` explicitly, or the child's copy loses it and the next fork COWs it, which is exactly M1. The implementation PR lists every `0xFFF` mask site and how it is handled.
- **Refcount width.** It is 16 bits. A shared page inherited by more than 65,535 processes would wrap. Fork depth is bounded by the process table today; the bound is recorded in the implementation PR.
- **Huge pages.** None are used for user memory. `PG_PS` entries are skipped by the walk, unchanged.
- **Not covered by P1:** cross-process sharing between unrelated processes (P2/P3), file coherence (P4), `MAP_FIXED` replace, partial `munmap` of a shared range (refused today for every mapping; unchanged).

## 6. Owner decisions required before P2-P4 (stop condition, #45/6256077528)

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

## 7. Honesty labels

P1 will be **gated by `smoke-shm` arms A-D**, emulated (QEMU TCG) only, and **not verified on real hardware**. P2-P4 are **not started**. This record does not close OPEN-1/2/12/13 or U1-U3.
