# DDR-1189: Dynamic linking, shared libraries and user-space ASLR (checklist C1.6)

Status: designed ahead (#45 comment 6436706603, item 2). C1.3 has merged; P1 is
being built under Amendment 1. P2 to P4 are not built.

## 1. Where the tree is (read from `dev/phase1` = `7981cd4`)

- **Loader.** `kernel/exec/elf.c` accepts only `ET_EXEC` for x86-64 and maps
  only `PT_LOAD` segments. `PT_INTERP` and `PT_DYNAMIC` are ignored, and an
  `ET_DYN` file is refused. The auxiliary vector is minimal (`AT_PAGESZ` and a
  few more).
- **Layout is fixed.** The anonymous mmap arena is
  `VMM_MMAP_BASE`-`VMM_MMAP_TOP` (`0x88_0000_0000`-`0x8F_0000_0000`), and the
  stack top is `USER_STACK_TOP` (`0x90_0000_0000`), 8 MiB deep
  (`kernel/mm/vmm.h`). Every process gets the same addresses.
- **W^X.** `mmap` refuses `PROT_EXEC` outright (`kernel/syscall/sys_mmap.c`).
  Executable code arrives only through the ELF loader.
- **libc.** A static musl subset (`build/musl/lib/libc.a`, PROC-D) built from
  the pinned submodule plus an overlay. There is no `libc.so` and no
  `ld-musl-x86_64.so.1`.
- **Entropy.** `kernel/drivers/rng/virtio_rng.c` exists; nothing reads
  `RDRAND`.

## 2. Scope split

| Part | Content | Needs |
|---|---|---|
| P1 | User ASLR for what exists now: random stack top, random mmap base, from one kernel entropy pool | C1.3 |
| P2 | Static PIE: the loader accepts `ET_DYN` without `PT_INTERP`, picks a random base, and applies `R_X86_64_RELATIVE` relocations | P1 |
| P3 | `PT_INTERP`: the kernel maps the interpreter as well; musl's `ld.so` loads `libc.so` and other libraries through executable file mappings | P2 |
| P4 | Shared text pages: two processes mapping the same library share its read-only frames | P3 and C2.1 (page cache) |

P4 needs the page cache (C2.1). Until then each process gets private copies of
library text, which is correct but uses more memory. That is recorded, not
hidden.

## 3. Decisions (delegated; my recommendations, taken)

- **L1. W^X stays absolute.** An executable mapping comes only from a file:
  `mmap(PROT_EXEC)` is allowed only with a file descriptor opened read-only,
  `MAP_PRIVATE`, and never together with `PROT_WRITE`. `mprotect` never adds
  `PROT_EXEC` to anonymous memory, nor to a mapping that was ever writable.
  Libraries with text relocations (`DT_TEXTREL`) are refused, so the loader
  never has to write to text.
- **L2. Entropy.** ASLR uses its own function, `aslr_random(nbytes)`, and
  does not change `rng_bytes()` (`kernel/crypto/rng.h`), which stays
  fail-closed for key material. `aslr_random` first calls `rng_bytes()`
  (virtio-rng or `RDSEED`); if that fails and CPUID reports `RDRAND`, it
  tries `RDRAND`, checking CF on every call and retrying at most 10 times per
  64-bit word (Intel's recommended bound); if the retries run out or `RDRAND`
  is absent, it mixes TSC jitter into a per-boot state
  and sets a flag. The first time the flag is set, the kernel prints
  `aslr: weak entropy` once on the console. ASLR still runs in that case, and
  that case is not called secure.
- **L3. Entropy bits.** The mmap arena is 28 GiB (`0x88_0000_0000` to
  `0x8F_0000_0000`), so a page-aligned base anywhere in it would give about 22.8
  bits. The design **caps** the mmap-base offset at 1 GiB, which gives 18 bits,
  so that a process always keeps at least 27 GiB of arena. The stack top moves
  within 256 MiB (16 bits) and a PIE base within 1 GiB (18 bits). All three are
  fewer than Linux's 28+ bits, and the record says so. Widening the mmap cap
  trades arena space for entropy; that trade is left for after P1 is measured.
- **L4. Where `ld.so` and `libc.so` come from.** They are built from the same
  pinned musl submodule (`ldso/dynlink.c` and the full `src/`), not from the
  static subset. They install under `/lib` on the SFS image. They are built
  without text relocations (L1).
- **L5. Agents.** An agent process may run dynamically linked binaries. Its
  executable mappings count against its memory cap like any other mapping.

## 4. Gate `smoke-dynlink` (probes `user/dyntest*.c`, arms by part)

| Arm | Part | Assertion | Mutant | What the mutant breaks |
|---|---|---|---|---|
| A | P1 | over 4 execs of the same probe, at least 2 distinct stack tops | D1 | stack top not randomised |
| B | P1 | over 4 execs, at least 2 distinct mmap bases | D2 | mmap base not randomised |
| C | P2 | a static-PIE probe runs, reads a relocated pointer correctly, and over 4 execs shows at least 2 distinct load bases | D3 | relocations skipped (the pointer is wrong) |
| D | P3 | a dynamically linked probe calls `lib_answer()` from `libtest.so` and prints its value | D4 | the interpreter is not mapped (exec fails) |
| E | P3 | GOT and PLT relocations in `libtest.so` resolve (a global variable and a function pointer both hold the expected value) | D5 | `R_X86_64_GLOB_DAT` ignored |
| F | P3 | W^X: `PROT_WRITE` + `PROT_EXEC` together returns `-EACCES`; `PROT_EXEC` on an anonymous mapping or an `O_RDWR` fd returns `-EACCES`; `MAP_SHARED` + `PROT_EXEC` on a read-only fd returns `-EACCES`; `mprotect(PROT_EXEC)` on anonymous memory, and on a file mapping that was mapped writable earlier, returns `-EACCES` | D6, D7, D7b, D7c | the fd-mode check skipped; the `mprotect` check skipped; the `MAP_PRIVATE` requirement skipped; the ever-writable record not kept |
| G | P3 | a library with `DT_TEXTREL` is refused | D8 | the `DT_TEXTREL` check skipped |
| H | P4 | a second process mapping `libc.so` uses fewer than 8 new frames for its text | D9 | text pages copied per process |

Arms A-C go falsely red only if all four draws are equal: (2^-16)^3 = 2^-48 for
the stack top, less for the other two. Each arm
is checked for vacuity before it is written: A, B and C fail on today's fixed
layout and `ET_EXEC`-only loader.

## 5. Risks

- **The loader is a security boundary.** It parses untrusted headers. Every
  offset and size from `PT_DYNAMIC` is bounds-checked against the file before
  use, as `PT_LOAD` already is.
- **Relocation of a PIE runs in the kernel (P2)**, on user memory, through the
  uaccess helpers (DDR-1181 §3.7), never through raw pointers.
- **Musl's dynamic linker** brings a large body of code into the TCB of every
  dynamically linked process. It is pinned to the submodule version, and
  upgrades follow the dependency triage rule (DDR-1172).

## 6. Honesty labels

- Nothing here is built. Each part is **not verified** until its arms are
  green and its mutants are shown red in CI.
- Emulated (QEMU TCG) only, and **not verified on real hardware**.
- ASLR strength is bounded by L3's bit counts, and by L2 when no hardware
  entropy is present.
- This record does not close OPEN-1/2/12/13 or U1-U3.

## Amendment 1 (2026-11-20): Part P1, user ASLR for what exists now

Authorised by #45 6993979920 (C1.6 follows C1.4's lock-order checking). This
amendment fixes P1's implementation; §3 L2 and L3 stand as written.

### A1.1 Where the tree is (`dev/phase1` = `01d0704`)

- **One loader core.** `elf_build_image` (`kernel/exec/elf.c`) builds every
  user image: the boot-time probe spawns through `elf_load`/`elf_load_args`,
  and `execve` (`kernel/syscall/sys_exec.c`). It maps the top stack pages at
  the fixed `USER_STACK_TOP` and writes the SysV initial frame there.
- **The mmap bump pointer** lives in `struct mm` (`mmap_next`). `mm_create` and
  `execve`'s commit set it to the fixed `VMM_MMAP_BASE`; fork copies it.
- **Stack growth** (`vmm_stack_fault`, ADR-038) maps not-present pages in the
  fixed `[USER_STACK_BOT, USER_STACK_TOP)`. The `[stkdepth]` diagnostic in
  `vmm_user_range_ok` measures from `USER_STACK_TOP`. No gate reads it.
- **Entropy.** `rng_bytes()` uses virtio-rng or `RDSEED` and fails closed.
  Gates attach virtio-rng only under `QEMU_RNG`, and the default TCG CPU has
  neither `RDSEED` nor `RDRAND`, so most boots have no hardware source
  (`[rng] source=NONE`). Nothing reads `RDRAND`.
- **No user probe hard-codes the stack** (`stackdemand` recurses relative to
  its own frame). `shmtest` and `sigsemtest` use the fixed address
  `0x8E00000000`, 24 GiB above any P1 mmap base.

### A1.2 Decisions

- **P1-1. The layout belongs to the address space.** `struct mm` gains
  `stack_top` (the stack is `[stack_top - USER_STACK_SIZE, stack_top)`, the
  guard page below it) and `mmap_base` (where `mmap_next` starts). `mm_create`
  sets the fixed values. Fork copies the parent's, because the child's stack
  contents are copies at the same addresses. `CLONE_VM` threads share the mm,
  and with it the layout.
- **P1-2. `elf_build_image` draws the layout and returns it.**
  - `stack_top = USER_STACK_TOP - r16 * 4 KiB`, with `r16 < 2^16`: 16 bits
    within 256 MiB (L3).
  - `mmap_base = VMM_MMAP_BASE + r18 * 4 KiB`, with `r18 < 2^18`: 18 bits
    within 1 GiB (L3).
  - It builds the stack and the initial frame at `stack_top`.
  - `elf_load_args` stores the layout in the new mm. `execve` stores it at its
    point of no return, where it used to reset `mmap_next` to the fixed base.
  - Nothing else moves in P1: the ELF image (`ET_EXEC`, link addresses, until
    P2), the vDSO, and the framebuffer window (`FB_USER_VA`).
- **P1-3. Every reader of the stack range uses the mm's.**
  `vmm_stack_fault` takes the stack top; the `#PF` path passes the faulting
  thread's. The `[stkdepth]` diagnostic measures from the current thread's.
- **P1-4. `aslr_random()` (`kernel/crypto/aslr.c`) implements L2 as written.**
  1. `rng_bytes()`.
  2. Failing that, `RDRAND` when CPUID.1:ECX[30] is set, checking CF on every
     call, at most 10 tries per 64-bit word.
  3. Failing that, TSC readings mixed into a per-boot state, with a flag set
     and `aslr: weak entropy` printed once.
  - The source of the first draw is printed once, `[aslr] source=rng`,
    `rdrand` or `tsc`.
  - `rng_bytes()` is not changed, and stays fail-closed for key material.
  - The draw happens before `execve`'s point of no return, so a failure there
    costs nothing; the TSC path cannot fail.
- **P1-5. Gate `smoke-dynlink`, arms A and B (§4).**
  - Probe `user/dyntest.c`, raw syscalls, no libc. It is embedded for
    `QEMU_PROBES=dynlink` and copied to the FAT image as `/DYNTEST.ELF`.
  - The boot instance forks four children. Each `execve`s `/DYNTEST.ELF` with
    `argv[1] = "child"`, and the parent waits for each in turn.
  - Each child prints `PRADYOS_DYN_L stack=0x... mmap=0x...`:
    - the address of a local in its entry function;
    - the address returned by a fresh one-page anonymous `mmap`.
  - The parent prints `PRADYOS_DYN_DONE n=4`.
  - All four samples come from `execve`, so the commit-side store is tested,
    not only the boot path.
  - The gate runs with `QEMU_RNG=1` and requires `[aslr] source=rng`, so it
    tests the primary source. Every other gate boots without virtio-rng and
    exercises the TSC path, which is not asserted (stated).
  - **Arm A:** the four `stack=` values are within
    `[USER_STACK_TOP - 256 MiB - 8 MiB, USER_STACK_TOP)`, and at least 2 of
    them are distinct.
  - **Arm B:** the four `mmap=` values are within
    `[VMM_MMAP_BASE, VMM_MMAP_BASE + 1 GiB + 1 MiB)`, and at least 2 of them
    are distinct.
  - A false red needs four equal draws: 2^-48 for the stack, 2^-54 for mmap.
- **P1-6. Mutants (each red on its own arm).**
  - **D1:** `elf_build_image` keeps `stack_top = USER_STACK_TOP`. Expected: A
    red (`distinct=1`), B green.
  - **D2:** it keeps `mmap_base = VMM_MMAP_BASE`. Expected: B red, A green.
  - **D2b:** `execve`'s commit resets `mmap_next` to `VMM_MMAP_BASE`, the
    fixed value, instead of storing the drawn base. Expected: B red, A green;
    this is what makes every sample come from `execve`.
  - **Vacuity, read before writing:** today's tree has the fixed layout, so A
    and B are both red on it.

### A1.3 Not covered by P1 (stated)

- The program image, the vDSO and the framebuffer window stay at fixed
  addresses until P2 and later.
- Strength is bounded by L3's bit counts. On the TSC path ASLR runs, but it is
  weak and not called secure.
- Fork shares the parent's layout by design. Only `execve` re-randomises.
- QEMU (TCG) only, not verified on real hardware.
