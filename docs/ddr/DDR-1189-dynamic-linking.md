# DDR-1189: Dynamic linking, shared libraries and user-space ASLR (checklist C1.6)

Status: designed ahead (#45 comment 6436706603, item 2). C1.3 has merged. P1 is
built under Amendment 1 (#154), and P2 under Amendment 2 (#156). P3 is designed
under Amendment 3, as P3a and P3b, and is not built. P4 is not built.

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
  - Every `PT_LOAD` must end below the lowest stack bottom any draw can give, and its guard page (`USER_STACK_LOWEST_BOT`), not only below the fixed one. An image therefore loads, or is refused, the same way whatever the draw (Codex on #154).
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
  - `rng_bytes()` stays fail-closed for key material. Its virtio path now holds a lock across the draw: the queue, its descriptor and the bounce page are shared, and exec now draws from every CPU (Codex on #154).
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

## Amendment 2 (2026-11-20): Part P2, static PIE

Authorised by #45 6993979920 (C1.6, part by part, after P1). §3 L1 and L3
stand; this amendment fixes how P2 meets them.

### A2.1 Where the tree is (`c16-p1-aslr` = `da6ce27`, P1)

- **Only `ET_EXEC` loads.** `elf_build_image` refuses any other `e_type`
  (`ELF_E_MACHINE`). It maps each `PT_LOAD` at its link address, which must lie
  in `[USER_MIN, USER_STACK_LOWEST_BOT - 4 KiB)`.
- **That bound is too loose (CodeRabbit on #154, carried here).** A `PT_LOAD`
  may reach into:
  - the surface window, `SURFACE_VA_BASE` = `0x86_0000_0000`;
  - the framebuffer window, `FB_USER_VA` = `0x87_0000_0000`;
  - the mmap arena, `0x88_0000_0000`.
  A later `mmap`, surface or framebuffer mapping would then collide with the
  image.
- **Every user ELF in the tree is `ET_EXEC`**, linked at `0x80_0000_0000`
  (`user/user.ld`, `user/user_c.ld`). Measured over the 91 built images: the
  highest `PT_LOAD` end is `0x80_0001_60a8` (`compositor.elf`).
- **The auxiliary vector** in the initial frame is `AT_PAGESZ` and `AT_NULL`
  only.
- **musl's static start-up uses `AT_PHDR`.** `src/env/__init_tls.c` line 90
  walks the program headers at `AT_PHDR` whenever `AT_PHNUM` is non-zero.
  Today it finds none.
- **The toolchain.** binutils 2.42 is both the local and the CI `ld`
  (`ubuntu-24.04`, DDR-1191). On a trial freestanding object, `ld -pie
  --no-dynamic-linker` gives an `ET_DYN` with no `PT_INTERP`, a `PT_DYNAMIC`,
  and only `R_X86_64_RELATIVE` entries in `.rela.dyn`. Linking it with
  `--dynamic-linker=` adds a `PT_INTERP`. Linking with `-z notext` and a
  pointer in `.text` puts a relocation in the read-execute segment and sets
  `DT_TEXTREL`.

### A2.2 Decisions

- **P2-1. Where images go.**
  - **One bound for every image:** `USER_IMAGE_TOP` = `0x86_0000_0000`, the
    surface window's base. Every `PT_LOAD`, `ET_EXEC` or `ET_DYN` after its
    bias, must lie in `[USER_MIN, USER_IMAGE_TOP)`, with no overflow in
    `p_vaddr + p_memsz`.
    - It replaces P1's stack-window bound and implies it; a static assertion
      holds that.
    - It closes the arena case above. Today's images end 24 GiB below it.
  - **`ET_DYN` base:** `USER_PIE_BASE + r18 * 4 KiB`, with
    `USER_PIE_BASE` = `0x81_0000_0000` and `r18 < 2^18`: 18 bits within 1 GiB
    (L3). The bias is that base minus the lowest page of any `PT_LOAD`.
  - **`PT_LOAD`s ascend and share no page** (`ELF_E_SEGMENT`; CodeRabbit on
    #156), `ET_EXEC` and `ET_DYN` alike.
    - Each `PT_LOAD`'s page-rounded range must start at or above the previous
      one's page-rounded end. The gABI already requires ascending `p_vaddr`.
    - `map_core` replaces a present leaf without a check. A shared page would
      leak the earlier frame, drop its bytes, and take the later segment's
      permissions, after the relocation-target and entry checks had read the
      earlier segment's flags. That predates P2 for `ET_EXEC`.
    - Every image built in the tree meets it (checked locally over all 94).
  - **`ET_DYN` extent:** the whole image, after its bias, must end at or below
    `USER_PIE_TOP` = `0x84_0000_0000`. An image can therefore span up to
    11 GiB.
  - `[0x84_0000_0000, 0x86_0000_0000)` is left for P3's interpreter.
    `ET_EXEC` images stay at their link addresses.
- **P2-2. What `ET_DYN` P2 accepts.**
  - **No `PT_INTERP`** (`ELF_E_INTERP`). P3 adds the interpreter.
  - **At most one `PT_DYNAMIC`**, inside the file. Its 16-byte entries are read
    up to `DT_NULL` or the segment's end.
  - **The relocation table is `DT_RELA` with `DT_RELASZ`.**
    - `DT_RELAENT`, if present, must be 24, and `DT_RELASZ` a multiple of 24.
    - The table must lie inside the file part of one `PT_LOAD`. It is read
      from the file, through that segment's offset.
  - **Refused (`ELF_E_DYN`):**
    - `DT_NEEDED`: libraries are P3's.
    - `DT_REL` and `DT_RELR`: x86-64 uses `RELA`, and a table the loader does
      not read must not be left unapplied.
    - A non-zero `DT_JMPREL` or `DT_PLTRELSZ`: PLT relocations need symbols. An empty one (0) is accepted (CodeRabbit on #156).
  - **Relocation types:** `R_X86_64_RELATIVE` with symbol 0 is applied, and
    `R_X86_64_NONE` is skipped. Anything else is `ELF_E_RELOC`.
  - **The entry must lie in an executable `PT_LOAD`** (`ELF_E_ENTRY`; Codex on
    #156). A shared object, or a PIE whose `e_entry` points elsewhere, is
    refused with `ENOEXEC` before `execve` commits, instead of faulting after
    it. `ET_EXEC` images are not checked, as before.
- **P2-3. Every target is checked before any is written.**
  - A target must be 8-byte aligned. It must lie inside a writable (`PF_W`)
    `PT_LOAD`, in `[p_vaddr, p_vaddr + p_memsz - 8]` before the bias.
    Anything else is `ELF_E_RELOC`, and nothing has been written.
  - This is L1 in the kernel: the loader never writes to text. A text
    relocation is refused because it lands in a non-writable segment, whether
    or not the image sets `DT_TEXTREL`.
  - **The write.** The value is `bias + r_addend`. The kernel resolves the
    target's page in the new address space (`vmm_resolve`) and writes through
    the frame's identity view, the way `load_page` copies file bytes.
    - §5 said "through the uaccess helpers". Those act on the current address
      space, and the new one is not current while it is built.
    - The frames are new and private, and nothing else maps them yet.
  - **Applying `RELATIVE` twice gives the same value**: it reads `r_addend`,
    not the word in memory. A start-up that relocates itself again, such as
    musl's `rcrt1`, is harmless.
- **P2-4. The auxiliary vector, `ET_DYN` only.**
  - `AT_PHDR`, `AT_PHENT`, `AT_PHNUM` and `AT_ENTRY`, all with the bias
    applied, ahead of `AT_PAGESZ`.
  - `AT_PHDR` is the program headers' address when the file part of a
    `PT_LOAD` holds them. Otherwise `AT_PHDR` and `AT_PHNUM` are 0.
  - **`ET_EXEC` frames are unchanged, byte for byte.** Otherwise musl's static
    start-up would begin walking `AT_PHDR` in every existing binary. That
    change is not P2's to make.
  - The 8 extra slots keep the parity that the 16-byte alignment rule reads.
- **P2-5. Gate `smoke-dynlink`, arm C as five sub-arms.**
  - **`user/dynpie.c`**, a static PIE: clang `-fPIE`, `ld -pie
    --no-dynamic-linker -z text`, raw syscalls.
    - It holds a data pointer and a function pointer, each needing a
      `RELATIVE` relocation.
    - It prints `PRADYOS_DYN_P base=0x... rel=R aux=X`.
    - `base` is the run-time address of its own ELF header (`__ehdr_start`).
    - `rel=1` when both pointers equal the addresses the code computes
      PC-relative.
    - `aux=1` when four things hold:
      - `AT_ENTRY` equals `_start`;
      - `AT_PHNUM` and `AT_PHENT` match its ELF header;
      - `AT_PHDR` equals `base + e_phoff`;
      - the base musl's rule computes from `AT_PHDR` (the address of
        `_DYNAMIC` minus `PT_DYNAMIC`'s `p_vaddr`) equals `base`.
  - **`user/dynpbad.c`**, built twice into two images that must be refused:
    - `/DYNPINT.ELF`, linked with a `PT_INTERP`;
    - `/DYNPTXT.ELF`, with an aligned 8-byte relocation in its text
      (`-z notext`).
    - Either one prints `PRADYOS_DYN_X` if it runs.
  - **The parent is `dyntest`'s boot instance.** After arms A and B, it forks
    four children that `execve` `/DYNPIE.ELF`.
    - It then forks two more, which `execve` the refused images and print
      `PRADYOS_DYN_R interp rc=` or `textrel rc=` when `execve` returns.
    - Last, it prints `PRADYOS_DYN_PIE_DONE n=4`.
  - The three new images go on the FAT image only, not into `kernel.bin`.
  - **C1:** four `P` lines, all `rel=1`.
  - **C2:** the four bases lie in `[USER_PIE_BASE, USER_PIE_BASE + 1 GiB)`,
    and at least 2 are distinct. A false red needs four equal draws: 2^-54.
  - **C3:** all `aux=1`.
  - **C4:** `interp rc=-8` (`-ENOEXEC`).
  - **C5:** `textrel rc=-8`.
  - `FORBIDDEN_SENTINEL` adds `PRADYOS_DYN_X`.
- **P2-6. Mutants, each red on its own sub-arm.**
  - **D3:** the relocations are validated but not written. Expected: C1 red
    (`rel=0`), the rest green.
  - **D3b:** `r18 = 0`, a fixed base. Expected: C2 red (`distinct=1`).
  - **D3c:** `AT_PHDR` and `AT_ENTRY` without the bias. Expected: C3 red
    (`aux=0`).
  - **D3d:** the `PT_INTERP` refusal skipped. Expected: C4 red. The image runs
    and prints `PRADYOS_DYN_X`.
  - **D3e:** the target check skipped. Expected: C5 red. The text relocation
    is written and the image runs.
  - **Vacuity, read before writing:**
    - P1's tree refuses every `ET_DYN`, so C1, C2 and C3 are red there: no
      `P` line is printed.
    - C4 and C5 are refusals, and they pass on P1's tree for the same reason.
      They mean something only beside C1, which shows that `ET_DYN` is
      accepted.
    - D3d and D3e show that each refusal comes from its own check. Stated.

### A2.3 Not covered by P2 (stated)

- **`PT_GNU_RELRO` is not enforced.** The RELRO pages stay writable. P3's
  `ld.so` or a later part may make them read-only.
- **A musl-linked static PIE** (`rcrt1.o`, `-static-pie`) is neither built nor
  tested. Only freestanding probes are, and `PT_TLS` in a PIE is untested.
- **Not gated:** the refusals in P2-2 other than `PT_INTERP` (the entry check
  among them), the `USER_IMAGE_TOP` bound, and the ascending, no-shared-page
  rule for `PT_LOAD`s. They are code paths with no probe. The entry check and
  the shared-page rule were checked locally on patched copies of `DYNPIE.ELF`.
- **Strength** is bounded by L3's 18 bits and, without hardware entropy, by
  L2's TSC path.
- QEMU (TCG) only, not verified on real hardware.

## Amendment 3 (2026-11-21): Part P3 designed, and split into P3a and P3b

Authorised by #45 6993979920 (C1.6, part by part). Design only: no code is
built under this amendment yet.

### A3.1 Where the tree is (`dev/phase1` = `d397412`, after P2)

- **The loader refuses `PT_INTERP`** for `ET_DYN` (P2-2). It relocates `ET_DYN`
  images itself, `R_X86_64_RELATIVE` only.
- **`execve` reads the whole image into one kernel buffer.** `EXEC_MAX` is 256 KiB
  (`kernel/syscall/sys_exec.c`). A full musl `libc.so` is expected to exceed it;
  its size is not measured yet (P3b measures it).
- **`mmap`** (`kernel/syscall/sys_mmap.c`):
  - `PROT_EXEC` is refused outright (`-EINVAL`).
  - `MAP_FIXED` is not supported.
  - File-backed `MAP_PRIVATE` is eager: frames are filled before they are mapped (DDR-1112).
- **`mprotect` refuses W+X together (`-EACCES`)**, but it adds `PROT_EXEC` alone
  to any mapping, including anonymous memory written earlier.
  - W^X therefore holds at every instant, but "write, then execute" works today.
  - L1 (§3) forbids that, and P3a enforces it. Recorded, not hidden.
- **`struct vm_area`** (`kernel/proc/sched.h`) records base, length, a shm
  object, `shm_ro`, a page offset and a write capability. It has no
  executable or ever-writable state.
- **No VFS implements symbolic links**; FAT32 has long names (`/LongFileName.txt`).
- **The musl build** (`tools/build_musl.sh`) is a static subset.
  - It maps `read`, `write`, `writev`, `mmap`, `munmap` and `exit` to NSI numbers.
  - Every other Linux number is offset by 4096 and so returns `-ENOSYS`.
  - NSI has `open`, `close`, `fstat`, `lseek`, `mprotect` and `SET_TLS`. It has no
    `pread`, and its `struct stat` is not checked against Linux's layout.
- **musl's `ld.so` does not refuse `DT_TEXTREL` itself.**
  - It calls `mprotect(map, len, PROT_READ|PROT_WRITE|PROT_EXEC)` and fails the
    load on any error but `ENOSYS` (`ldso/dynlink.c`, line 853 in the pinned tree).
  - L1's kernel refusal is therefore what refuses a text-relocation library. The
    refusal must be `-EACCES`, never `-ENOSYS`.
- **musl's `ld.so` maps a library** by mapping its whole span, then mapping each
  segment over it with `MAP_FIXED`, and `MAP_FIXED|MAP_ANONYMOUS` for `.bss`.
  It needs `MAP_FIXED` over the caller's own earlier mapping.

### A3.2 The split

P3 is two parts, each with its own PR, gate arms and mutants:

- **P3a, kernel only.** No musl is involved, so every arm is decided by kernel
  code and freestanding probes.
- **P3b, musl.** It builds `ld.so`/`libc.so` and extends the syscall shim, on
  top of P3a.

### A3.3 P3a decisions

- **P3a-1. L1 in `mmap` and `mprotect`.**
  - `mmap(PROT_EXEC)` is allowed only for a file-backed `MAP_PRIVATE` mapping of
    an fd opened read-only, and never together with `PROT_WRITE`. Everything
    else is `-EACCES` (today `-EINVAL`).
  - A `vm_area` gains two bits:
    - `x_ok`: the mapping came from a read-only file and was never writable;
    - `ever_w`: it has been writable at some time.
  - `mprotect` may add `PROT_EXEC` only when `x_ok` is set and `ever_w` is clear;
    anything else is `-EACCES`.
  - `mprotect` adding `PROT_WRITE` sets `ever_w`, so the mapping can never
    become executable again.
  - **`mprotect` covers pages that no `vm_area` describes.**
    - It walks page tables (`vmm_protect_range`), so ELF image pages, the
      stack and the heap are reachable too. Today any of them can be made
      R+X after being written.
    - The rule is therefore per page: `PROT_EXEC` is allowed on a page only
      if the page is already executable (its NX bit clear), or if it lies in
      a `vm_area` with `x_ok` set and `ever_w` clear.
    - An image text page that was made writable once is NX from then on, so
      it can never be made executable again.
  - **This changes behaviour today's tree allows**: adding `PROT_EXEC` to
    anonymous memory, the stack, the heap or an image data page.
    - In-tree callers of `PROT_EXEC` (read 2026-11-21):
      - `mprotecttest` arm D (W+X together, `-EACCES`, unchanged);
      - `shmtest` arm D (`MAP_SHARED|MAP_ANONYMOUS` with `PROT_RWX`, expects
        `-EINVAL`);
      - `systest` (anonymous `PROT_READ|PROT_EXEC` must be refused).
      No probe adds `PROT_EXEC` with `mprotect`.
    - **errno precedence:** `mmap` keeps `-EINVAL` for an executable mapping
      that is anonymous or `MAP_SHARED`, as today and as `shmtest` D and
      `systest` pin. `-EACCES` is new, and only for a file-backed
      `MAP_PRIVATE` request that L1 refuses (a writable fd, or `PROT_WRITE`
      with `PROT_EXEC`).
    - §4 arm F's "`-EACCES`" for anonymous `PROT_EXEC` and for
      `MAP_SHARED`+`PROT_EXEC` is restated to match: the existing gates
      keep their codes, and arm F checks `-EINVAL` there.
- **P3a-2. `MAP_FIXED` over the caller's own mapping.**
  - The fixed range must lie inside `MAP_PRIVATE` areas this address space
    mapped with `mmap`, all in the mmap arena.
  - The pages it covers are replaced and their frames freed. The areas keep
    their extent, so `munmap` still takes each one whole.
  - A range that touches anything else is `-EINVAL`, so `MAP_FIXED` never
    clobbers what it did not map:
    - an image `PT_LOAD`, the stack or the interpreter (all outside the arena);
    - a shm or file `MAP_SHARED` area, or an anonymous `MAP_SHARED` page;
    - a range that is partly mapped and partly free.
  - **L1 holds through a replacement.** Every area the range touches loses
    `x_ok` unless the new mapping is itself a non-writable mapping of a
    read-only fd. A page written through a replacement can therefore never be
    made executable.
  - **Two known conservative limits (Codex on #158).** Both refuse a request
    that would have been safe; neither ever permits write-then-execute.
    - A replacement only ever clears `x_ok`. A read-only file mapping that
      replaces a whole anonymous area therefore cannot later gain X through
      `mprotect`. musl's `ld.so` maps each text segment with `PROT_EXEC` in the
      `MAP_FIXED` call itself, so it never needs that path.
    - `x_ok` and `ever_w` are per `vm_area`, not per page.
      `mprotect(PROT_WRITE)` on one page refuses a later `PROT_EXEC` on every
      page of that area. musl's RELRO `mprotect` only removes write, and its
      `DT_TEXTREL` path is refused by design.
    - Lifting either needs its own state (whole-area coverage, or a per-page
      bitmap), its own arm and its own mutant. It is left until a caller needs
      it.
  - On `-ENOMEM` or `-EIO` part-way, the pages not yet mapped stay absent inside
    their area. POSIX leaves the range's contents unspecified after a failed
    `MAP_FIXED`; `munmap` still frees the area.
  - **Two changes from the first text of this amendment, made when it was built
    (2026-11-21):**
    - **The interpreter window is excluded.** The kernel maps the interpreter as
      image pages with no `vm_area` (P3a-3), so nothing there is the caller's own
      `mmap`. musl's `ld.so` maps libraries in the arena and never maps over its
      own image (it reuses the gaps in it for `malloc`).
    - **Wholly unmapped arena space is not `-EINVAL`.** Before P3a-2 the flag was
      ignored and the address taken as a hint, which maps exactly there when the
      space is free. That clobbers nothing, so it stays (arm M5). Only a range
      that is partly mapped and partly free is refused.
- **P3a-3. `PT_INTERP`.**
  - The kernel reads the path from the file: absolute, NUL-terminated inside
    `PT_INTERP`'s file range, at most `PATH_MAX`. It opens the path through the
    VFS with the caller's capabilities.
  - The interpreter must be an x86-64 `ET_DYN` with no `PT_INTERP` of its own.
    The P2-1 and P2-2 header rules apply to it too.
  - **It is mapped page by page from the file** (`vfs_read` per page into a
    fresh frame), not through the 256 KiB buffer. Its size is bounded only by
    its window.
  - **Its window** is `[USER_PIE_TOP, USER_IMAGE_TOP)` = `[0x84_0000_0000,
    0x86_0000_0000)`, which A2 left for it. Its base is drawn in the window's
    first 1 GiB: 18 bits (L3).
  - **The kernel applies no relocations** to the interpreter or to a main
    program that has a `PT_INTERP`. The interpreter relocates itself
    (`_dlstart_c`) and then the program. Applying `RELATIVE` twice would be
    harmless, but a half-relocated program is not.
  - **auxv:**
    - `AT_BASE` = the interpreter's bias;
    - `AT_PHDR`, `AT_PHENT`, `AT_PHNUM` and `AT_ENTRY` describe the main
      program, for an `ET_EXEC` with `PT_INTERP` too;
    - the entry is the interpreter's.
    - An `ET_EXEC` without `PT_INTERP` keeps its frame byte for byte (P2-4).
  - **As built (2026-11-21):**
    - `execve` passes the caller's filesystem capability and root mount
      (`struct elf_fs`). A kernel spawn (`elf_load`) passes none, so an image
      with `PT_INTERP` is refused there. No image the kernel spawns has one:
      `readelf -l` over `build/*.elf` finds `PT_INTERP` only in `dynpint.elf`
      and, after P3a, the arm D0 images, all reached through `execve`.
    - The interpreter's ELF header and program headers must lie in its first
      page (every linker puts them at offset 64). Its `PT_LOAD` file ranges are
      checked against its size, with the same rules as an image (one helper,
      `elf_scan`, serves both).
    - A program with an interpreter must end at or below `USER_PIE_TOP`. An
      `ET_DYN` already does; an `ET_EXEC` may otherwise reach into the
      interpreter window, and `map_core` replaces a present leaf without a
      check.
    - Before A3, an `ET_EXEC` with `PT_INTERP` was loaded with the interpreter
      ignored. It now gets its interpreter. No in-tree `ET_EXEC` has one.
    - Arm C4's image names `/lib/ld-musl-x86_64.so.1`. That file does not exist
      in P3a, so C4 is still refused with `-ENOEXEC`, now because the
      interpreter cannot be opened. P3b installs that path and must re-point C4.
- **P3a-4. Gate `smoke-dynlink`, arms D0 and F.**
  - **Arm D0, a freestanding interpreter.**
    - The probe `user/ldtest.c` is a static PIE installed as `/LIB/LDTEST.SO`.
    - It checks that `AT_BASE` is its own base, applies the main program's
      `RELATIVE` relocations, and jumps to `AT_ENTRY`.
    - The main probe, linked with `--dynamic-linker=/LIB/LDTEST.SO`, prints its
      relocated pointer and both bases.
    - This tests the kernel's half of `PT_INTERP` without musl.
    - As built, the main program is `dynpie.c` built `-DDYN_D0` (`/DYND0.ELF`).
      Its `rel=1` now means the interpreter relocated it, because the kernel
      relocates nothing when an interpreter is present. The gate checks:
      - D0a: `ldtest` saw `AT_BASE` equal to its own base and applied at least
        two `RELATIVE` relocations;
      - D0b: the program's relocated pointers and its auxv (`rel=1 aux=1`);
      - D0c: the interpreter base, page-aligned in the first 1 GiB of
        `[USER_PIE_TOP, USER_IMAGE_TOP)`, and the program base in the PIE window.
  - **Arm D0r.** `/DYNPII.ELF` names `/DYNPINT.ELF` as its interpreter, which
    has a `PT_INTERP` of its own, so `execve` returns `-ENOEXEC`. Were it
    accepted, `dynpint` would run and print the forbidden `PRADYOS_DYN_X` line.
  - **Arm F, the W^X rules of §4.** Each refusal returns `-EACCES`:
    - `PROT_WRITE|PROT_EXEC` together;
    - `PROT_EXEC` on anonymous memory;
    - `PROT_EXEC` from an `O_RDWR` fd;
    - `MAP_SHARED` with `PROT_EXEC`;
    - `mprotect(PROT_EXEC)` on anonymous memory;
    - `mprotect(PROT_EXEC)` on a file mapping that was ever writable.
  - **Arm M, `MAP_FIXED` (P3a-2).** `dyntest` maps a two-page span of its own
    file read-only, as `ld.so` maps a library first, and prints one line:
    - M0: an anonymous writable page replaces the first page and reads 0, where
      the file (read before the replacement) has `0x7f`; then the file replaces
      it again, executable from the read-only fd, and reads `0x7f`;
    - M1: a range half in the span and half in unmapped space is `-EINVAL`;
    - M2: an anonymous `MAP_SHARED` page and a file `MAP_SHARED` area are each
      `-EINVAL`;
    - M3: the stack and the image are `-EINVAL`;
    - M4: after a writable replacement is written, `mprotect(PROT_EXEC)` on it is
      `-EACCES`;
    - M5: wholly unmapped arena space is mapped at the address asked;
    - M6: one page replaced in place 256 times leaves the free page count
      (`SYS_MEMINFO`) within 64 of where it was. Measured 0 locally.
    - M1, M2, M3 and M5 also pass on the tree before P3a-2, which refused every
      overlap and ignored the flag. They pin refusals and behaviour the old tree
      already had, so their teeth are the mutants below, not the before/after.
      M0 and M4 are red on the old tree.
  - **Mutants:**
    - D4': the kernel ignores `PT_INTERP`, so the program runs without its
      interpreter; D0 is red;
    - D4'': a wrong `AT_BASE`; D0 is red;
    - D4c: an interpreter with a `PT_INTERP` of its own is accepted; D0r is red;
    - D6, D7, D7b and D7c of §4, each red on its own F assertion;
    - D6b: the `PROT_WRITE|PROT_EXEC` refusal in `mmap` is dropped; F1 is red
      (§4 names no mutant for that assertion);
    - D9: a gap inside the range is allowed; M1 is red;
    - D9b: the shared-page check is dropped; M2 is red (both halves). Every
      shared page, anonymous, SysV, `shm_open` or file, is mapped
      `PTE_SW_SHMEM`, so this one per-page test refuses them all; there is no
      second, per-area test that a mutant could not reach.
    - D9d: a replacement leaves `x_ok` as it was; M4 is red;
    - D9e: the old pages are not unmapped before the new ones are mapped (no
      shootdown, old frames leaked). It was planned against M0's stale
      translation, but M0 stayed green under TCG, so arm M6 was added to measure
      the leaked frames instead; D9e is red on M6.
    - **Not gated (P3a-3), recorded rather than hidden:** a relative or
      unterminated interpreter path, an interpreter that is not `ET_DYN`, a
      second `PT_INTERP`, and an `ET_EXEC` with an interpreter ending above
      `USER_PIE_TOP`. Each is a refusal with no probe that builds the image.
      The kernel relocating the program as well as the interpreter has no
      mutant either: `RELATIVE` stores `bias + addend`, so applying it twice
      leaves the same word.
    - M3 has no mutant: `mmap_fixed_range` and `mmap_pick_base` apply the same
      arena bound, so removing either one alone leaves M3 green. That is
      recorded, not hidden.

### A3.4 P3b decisions

- **P3b-1. Build.**
  - musl's full `src/` and `ldso/` from the pinned submodule, `-fPIC`, linked
    as one `libc.so`.
  - It installs as `/LIB/LD-MUSL-X86_64.SO.1` (long name `ld-musl-x86_64.so.1`).
  - With no symbolic links, musl resolves a `DT_NEEDED` of `libc.so` to itself,
    so one file serves both.
  - The test library `libtest.so` installs beside it.
- **P3b-2. The syscall shim** grows to what `ld.so` and the arms call:
  - `open` with `O_RDONLY|O_CLOEXEC`, and `close`;
  - `read`, and `pread`: a new NSI call, or a documented `lseek`+`read`
    emulation;
  - `fstat` with Linux's `struct stat` layout, checked field by field;
  - `mmap` with `MAP_FIXED`, `mprotect`, `munmap`, `writev`, `exit`;
  - `arch_prctl(ARCH_SET_FS)` to `SET_TLS`.
  - Each mapping is listed in the DDR with its NSI call, and the rest stay
    `-ENOSYS`.
- **P3b-3. Gate arms D, E and G of §4.**
  - G becomes: a library with `DT_TEXTREL` fails to load because the kernel
    refuses musl's W+X `mprotect` with `-EACCES`.
  - Its mutant D8 becomes "the kernel allows W+X in `mprotect`", and the library
    then loads.

### A3.5 Not decided here (stated)

- Whether P3b's `pread` is a new NSI number (124 is free) or an emulation.
- How `PT_GNU_RELRO` is enforced; musl's `ld.so` will `mprotect` it read-only
  once P3a's `mprotect` rules land.
- P4 (shared text) still needs C2.1.
- QEMU (TCG) only, not verified on real hardware.
