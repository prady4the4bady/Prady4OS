# DDR-1189: Dynamic linking, shared libraries and user-space ASLR (checklist C1.6)

Status: design ahead (#45 comment 6436706603, item 2). No code until C1.3 has
merged (`Needs: C1.3`).

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
- **L2. Entropy.** The pool mixes virtio-rng when present, `RDRAND` when CPUID
  reports it, and TSC jitter. With neither hardware source, boot prints
  `aslr: weak entropy` and ASLR still runs. That case is not called secure.
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
| F | P3 | W^X: `PROT_WRITE` + `PROT_EXEC` together returns `-EACCES`; `PROT_EXEC` on an anonymous mapping or an `O_RDWR` fd returns `-EACCES`; `mprotect(PROT_EXEC)` on anonymous memory returns `-EACCES` | D6, D7 | the fd-mode check skipped; the `mprotect` check skipped |
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
