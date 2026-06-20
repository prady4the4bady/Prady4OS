# ADR-021: ELF64 loader + W^X/NX + per-process address spaces (BINDING SECURITY)

- **Status:** Accepted 2026-06-19 — **BINDING security policy**
- **Phase:** 5 (slice 5a)
- **Note on numbering:** the Layer-5 brief referenced "ADR-020" for the ELF
  loader; ADR-020 was already assigned to the RTC + FAT32 LFN work in slice 4j,
  so the ELF loader / W^X policy is recorded here as **ADR-021**. This ADR is the
  binding W^X security policy and may not be amended except by a new superseding
  ADR.

## Context

Layer 5 runs userspace. The first requirement is a safe ring-3 execution
substrate: each process in its own address space, loaded from an ELF, with
write-xor-execute enforced so no page is ever both writable and executable, and
guard pages so a stack/heap overflow faults cleanly instead of corrupting
kernel data. The current VMM is a single shared address space reached through a
low identity map; ring-3 entry exists (`enter_user_mode`, IRETQ) but without NX,
per-process CR3, or an ELF loader.

## Decision — W^X memory protection policy (BINDING)

Exact page permissions, derived from ELF `p_flags` (PF_R/W/X). **EFER.NXE is set
before the first ring-3 IRETQ**; `vmm_map` honors `VMM_NX`.

| Region              | Flags                              |
|---------------------|------------------------------------|
| user text (PF_X)    | present, USER, **RX** (no W, no NX)|
| user rodata (PF_R)  | present, USER, R, **NX**           |
| user data/BSS (PF_W)| present, USER, **RW + NX**         |
| user heap           | present, USER, **RW + NX** (mmap)  |
| user stack (8 MiB)  | present, USER, **RW + NX**         |
| guard page          | **not present** (PTE = 0)          |
| kernel text         | RX (verify VMM holds this)         |
| kernel data         | RW + NX                            |

- **No RWX page anywhere** in Layer 5 — not for the shell, not for JIT. JIT, if
  ever, gets its own later layer gated by a dedicated `CAP_JIT`.
- A segment that is both W and X in the ELF is rejected (W^X violation → load
  fails) rather than mapped RWX.
- **Guard pages** are unmapped PTEs (present = 0) placed immediately **below the
  stack bottom** and below every heap region. A user access there faults; the
  page-fault handler converts a user fault to a clean process kill
  (`sched_exit`, i.e. `sys_exit(-1)` semantics, until signals arrive in 5b).

## Decision — per-process address space

- **Per-process CR3.** Each process gets its own PML4 (`vmm_new_address_space`).
  The kernel is shared by **copying the kernel's top-level PML4 entries**: entry
  0 (the low identity map — required because the VMM reaches page tables through
  it, plus the kernel image and PMM live there) and entry 511 (the higher-half
  kernel). The remaining PML4 slots are private per process.
- **User virtual range = PML4 slot 1**, i.e. `[512 GiB, 1 TiB)`. This sits above
  the low-1 GiB identity map (slot 0) so user addresses never collide with it,
  and is private per process (slot 0/511 are the only shared ones). The user ELF
  is linked into this range; the stack sits near the top of it.
- The active CR3 is switched to the process AS before loading its segments
  (the kernel stays mapped, so kernel execution continues safely), and on every
  context switch into a user thread (`tcb.cr3`). Kernel threads use the master
  kernel CR3.
- **Address-space teardown** frees the private user page-table subtree + the
  PML4 on process exit (kernel entries are not freed — they are shared).

## Decision — ELF loading

Parse the ELF64 header (`\x7fELF`, class 64, x86-64, ET_EXEC). For each
`PT_LOAD`: allocate physical pages, map at `p_vaddr` with the W^X flags above,
copy `p_filesz` bytes from the file, zero-fill the rest (`p_memsz - p_filesz` =
BSS). Build the user stack top-down per the System V AMD64 ABI: strings, then the
auxv (`AT_PAGESZ`, `AT_NULL`), `envp` (NULL), `argv` (+ NULL), `argc`; the entry
SP points at `argc`. Enter ring 3 at `e_entry`.

## Consequences / deferred (tracked, with DDR/ADR before building)

- **COW fork** — interim is copy-all-pages (5b); COW deferred.
- **Dynamic linking** (`ld-pradyos.so`) — static only for now.
- **SMP user threads** — needs APIC (Layer-2 deferred list).
- The 5a user binary uses the existing cap-gated `sys_putc` with the console
  capability delivered in `RDI` at entry (a 5a convention); POSIX `write` and the
  ~50-call expansion land in 5b.
- Bootstrapping the binary onto SFS: the kernel embeds the freestanding test ELF,
  writes it to the (formatted) SFS volume, then **loads it back from SFS** — so
  the load path genuinely reads the ELF from the filesystem.

## Verification (gate: `smoke-user`)

QEMU q35: the kernel formats SFS, writes the embedded static ELF to it, loads it
**from SFS** into a fresh per-process address space with W^X enforced, enters
ring 3, the program prints `HELLO FROM RING-3` via `sys_putc`, and exits cleanly
via `sys_exit`. A negative check (a deliberate write to the guard page or to a
text page faults → clean kill) is added as the W^X regression. `-Werror` clean.
