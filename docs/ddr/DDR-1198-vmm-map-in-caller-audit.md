# DDR-1198: which `vmm_map_in` callers can reach a kernel-shared page-table entry

Status: **audit, read from the code at `dev/phase1` `9105001`. No code change.**
Asked by: operator, #45 comment 6594717280, step 5 ("audit every `vmm_map_in` caller for `descend`'s user-promotion; report which can reach a kernel-half entry").
Platform: nothing here is run. It is a reading of the code. Nothing is verified on real hardware.

## 1. The question

`descend` (`kernel/mm/vmm.c`) walks one level of the page tables for `map_core`. When the leaf it is building is a user mapping, it also promotes every **existing** entry it walks through to `VMM_USER`:

```c
if (user && !(e & VMM_USER))
    table[i] = e | VMM_USER;  /* promote existing entry to user-walkable */
```

Each address space's PML4 starts as a copy of the kernel master's top-level entries (`vmm_new_address_space`). Below the top level, those entries point at **shared** kernel tables:
- slot 0 is the low identity map;
- slot 511 is the higher-half kernel;
- slots 256 and up hold the kernel MMIO windows: ECAM `0xFFFFC0…`, NVMe `0xFFFFD2…`, GOP `0xFFFFD4…`, xHCI `0xFFFFD6…`, and the self-test slot 256.

A user mapping whose walk passes through one of those shared entries has three effects:
- it marks shared kernel tables user-walkable in every address space;
- it changes this address space's top-level copy, which then no longer equals the master's;
- `vmm_destroy_address_space` frees the subtree of any slot that differs from the master, so this process's teardown frees a shared kernel table (DDR-1196 §1).

The user range is PML4 slot 1 (`0x80_0000_0000`…`0xFF_FFFF_FFFF`), plus slot 255 for two fixed pages. Neither slot is populated in the kernel master.

## 2. Every caller

| Caller | Where the VA comes from | PML4 slot | Can it reach a shared kernel entry? |
|---|---|---|---|
| `sys_mmap.c` `sys_mmap_locked` → anonymous loop (line 305) | `mmap_pick_base`: the hint is bounded by DDR-1196 (`base > VMM_MMAP_TOP`, `npages > (TOP - base) / PAGE_SIZE`); `len` by DDR-1178 A3.4 | 1 | **No, since DDR-1196** (`a70bfd4`). Before it, yes: the hint wrapped into slot 511 and slot 0. |
| `sys_mmap.c` `map_obj_pages` (line 139), from `shm_fd_map` | `mmap_pick_base`, as above | 1 | No |
| `sys_mmap.c` `map_obj_pages`, from `shmat` | `mmap_next`, bounded by `base + npages * PAGE_SIZE > VMM_MMAP_TOP` with `npages ≤ 256`. `base ≤ VMM_MMAP_TOP`, so the sum cannot wrap. | 1 | No |
| `sys_io_uring.c:67` | `mmap_next`; `va < VMM_MMAP_BASE \|\| va + PAGE_SIZE > VMM_MMAP_TOP` | 1 | No |
| `elf.c:111` `load_page` | `p_vaddr`, page-rounded. Refused unless `vstart >= USER_MIN && vend <= USER_STACK_BOT - PAGE_SIZE && vend > vstart`. A wrapped `vend` fails `vend > vstart`. The loop stays in `[vstart, vend)`. | 1 | No |
| `elf.c:205` initial stack | `USER_STACK_TOP - (i + 1) * PAGE_SIZE`, constant | 1 | No |
| `vmm_cow.c:174` `vmm_stack_fault` | The fault VA. Range-tested `[USER_STACK_BOT, USER_STACK_TOP)` before anything is mapped. | 1 | No |
| `vmm_cow.c:79`, `:102` `vmm_fork_address_space_cow` | Rebuilt from the parent's PML4 slots that **differ from the master** (`e4 != kpml4[i4]`), leaves with `VMM_USER` | whatever slots differ | **Only after a prior promotion.** Normally only slots 1 and 255 differ. If some other path had already promoted a shared slot, fork would copy its user leaves into the child through the child's shared entry, spreading the damage. It is an amplifier, not a source. |
| `sys_fb.c:55` | `FB_USER_VA` (`0x87_0000_0000`) + `i * PAGE_SIZE`. `npages` comes from the display backend (`stride * h`), not from the caller. | 1 | No |
| `sys_surface.c:183`, `:202` | `SURFACE_VA_BASE` (`0x86_0000_0000`) + `id * 4 MiB`, `id < SURFACE_MAX` (16); bytes ≤ `SURFACE_BYTES_MAX` | 1 | No |
| `metric_page.c:41` `metric_page_map_user` | `METRIC_USER_VA`, constant `0x7FFF_FFEF_F000` | 255 | No: slot 255 is empty in the master |
| `vdso_page.c:29` `vdso_map_user` | `VDSO_USER_VA`, constant `0x7FFF_FFF0_0000` | 255 | No, as above |
| `main.c:4115`, `:4116`, `:4238`, `:4316`, `:4606`, `:4757` | Kernel self-tests on address spaces they build; constants in slot 1 | 1 | Not reachable from ring 3 |

**Answer: after DDR-1196, no caller can make `descend` promote a shared kernel entry.** `mmap` with a wrapping hint was the only path that could. Fork can spread a promotion but cannot start one.

## 3. Found on the way, outside the question (not verified)

- **Identity-mapped device memory.** `ahci.c:335` (`bar5`), `e1000e.c:207` (`bar0`), `ioapic.c:126` and `lapic.c:150` call `vmm_map(bar, bar, …)`. That maps the physical address at the same virtual address, through the **current** CR3.
  - If firmware or QEMU placed such a BAR in `[0x80_0000_0000, 0x100_0000_0000)`, the kernel mapping would land in PML4 slot 1, the user range.
  - If it ran while a process's CR3 was loaded, it would land in that process's private tables.
  - These are kernel mappings without `VMM_USER`, so `descend` does not promote anything, but they would sit inside user VA space. Whether QEMU ever places a 64-bit BAR at 512 GiB or above in the configurations CI boots is **not verified**.
- **`descend`'s promotion rule is unchanged.** Any future caller that maps a user page at a VA it does not bound will reintroduce the DDR-1196 class. A guard in `map_core` would close it for every caller at once: refuse a `VMM_USER` leaf whose PML4 index is a master-populated slot. That is a code change with its own gate and mutant, so it is **proposed here, not done**.

## 4. Honesty labels

- This record is a reading of the code at `9105001`. Nothing in it is run, and nothing is verified on real hardware.
- It does not change any gate and closes no open defect (OPEN-1/2/12/13, U1–U3).
