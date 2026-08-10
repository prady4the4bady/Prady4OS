= DDR-897 — ld-pradyos.so: Phase-0 design (Group 6 item 35)

**Status:** Phase-0 DESIGN ONLY. **No loader code is written.** This is the
checkpoint artefact; implementation begins only after sign-off.
**Date:** 2026-08-10
**Scope:** design. Touches nothing.

## 0. Where PRADYOS actually starts from

Every user binary today is **static, no-PIE**: the Makefile links with
`-static -no-pie`, `elf.c` loads `PT_LOAD` at fixed addresses, and there is no
interpreter path. `SYS_MMAP` is anonymous-only and refuses `fd != -1`
(DDR-877) — so **file-backed mapping does not exist**, and a dynamic loader
cannot map a shared object the way a normal linker would.

That single fact reshapes the plan: the first sub-phase is not the linker.

## 1. ELF class and ABI

- **ELFCLASS64 / ELFDATA2LSB / EM_X86_64 only.** No 32-bit, no compat.
- **System V AMD64 psABI.** The kernel's syscall path is already SysV
  (DDR-877), and PRADYOS has no legacy ABI stratum to preserve.
- `PT_INTERP` = `/lib/ld-pradyos.so.1`. Absolute; no search path in v1.

## 2. Relocation types — the minimum that is sufficient, not the full table

| Type | Needed for |
|---|---|
| `R_X86_64_RELATIVE` | PIE self-relocation; by far the most common |
| `R_X86_64_GLOB_DAT` | GOT entries for data symbols |
| `R_X86_64_JUMP_SLOT` | PLT entries |
| `R_X86_64_64` | absolute 64-bit references |
| `R_X86_64_COPY` | copy relocations for data in the executable |
| `R_X86_64_IRELATIVE` | **only if** ifuncs appear; musl uses them for some string functions |

**Anything else is refused with a diagnostic naming the type**, not skipped. A
skipped relocation is a wrong pointer that faults arbitrarily later — the
"absorb bad input" defect this project keeps cataloguing, in its most expensive
form.

`R_X86_64_IRELATIVE` is listed conditionally because whether musl emits it here
is a **measurement**, not an assumption. Sub-phase 1 answers it.

## 3. GOT/PLT and binding

- **Eager (BIND_NOW) binding for v1.** Lazy binding needs `_dl_runtime_resolve`
  — an assembly trampoline that preserves the full argument register set
  (including XMM for varargs), plus a writable GOT at runtime.
- **W^X (ADR-021) is binding and eager binding is what keeps it satisfiable.**
  With `BIND_NOW` + `RELRO`, the GOT is written during relocation and then made
  **read-only before control reaches user code**. Lazy binding requires the GOT
  writable for the process lifetime, which is a permanent W^X-shaped hole in
  every dynamic process.
- Link executables with `-Wl,-z,now -Wl,-z,relro`.

Lazy binding is therefore **not deferred for effort reasons** — it is rejected
for v1 on W^X grounds, and re-opening it needs an ADR amending ADR-021.

## 4. Symbol lookup, visibility, interposition

- Breadth-first over the load order: executable, then `DT_NEEDED` in order,
  transitively, each object once.
- **Global scope only.** No `RTLD_LOCAL`, no `dlopen` in v1.
- First definition wins — the standard rule that makes interposition work.
- `STB_WEAK` binds only if no strong definition exists anywhere in scope.
- `STV_HIDDEN`/`STV_INTERNAL` are not exported.
- **No symbol versioning** (`DT_VERNEED`/`DT_VERDEF`) in v1. If a versioned
  object is loaded, **refuse and say so** rather than binding the wrong symbol
  silently.

## 5. Memory, protections, TLS

- Segments mapped per `PT_LOAD` with exact `p_flags`. **No RWX mapping, ever.**
- `PT_GNU_RELRO` re-protected read-only after relocation, before entry.
- **TLS: `PT_TLS` for the initial-exec model only.** `SYS_SET_TLS` already
  exists and sets `fs_base`; the linker allocates the static TLS block and the
  main thread's TCB. **No dynamic TLS, no `__tls_get_addr`.** A `DTPMOD`/`DTPOFF`
  relocation is refused, not guessed at.

## 6. Failure behaviour

Every failure is **loud and fatal before user code runs**: missing dependency,
unsupported relocation type, unresolved symbol, unsupported TLS model, versioned
object, or a mapping that would violate W^X. The process exits with a diagnostic
naming the object and the cause.

There is no partial-success path. A dynamic loader that starts a process with an
unresolved GOT entry has produced a program that fails at an unpredictable later
instruction, and the report will name the wrong subsystem.

## 7. Bootstrap sequence

1. Kernel `elf.c` sees `PT_INTERP`, loads the interpreter, and enters **it**
   with the application's `AT_*` auxv.
2. `ld-pradyos.so` self-relocates using only `R_X86_64_RELATIVE` — it may not
   call through its own GOT before this completes.
3. Map dependencies; build the scope list.
4. Apply relocations, eagerly.
5. Apply RELRO protections.
6. Jump to the application entry.

Step 2 is the classic trap: the loader must be built `-fPIC` with **no external
calls** on that path.

## 8. Toolchain and kernel changes required

- Kernel: `PT_INTERP` handling in `elf.c`; a full `AT_*` auxv (`AT_PHDR`,
  `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_ENTRY`, `AT_PAGESZ`).
- **Kernel: file-backed `mmap`.** This does not exist — `sys_mmap` refuses
  `fd != -1` today. It is a prerequisite, not a detail.
- Build: a `-fPIC -shared` link for the loader, `-pie -z now -z relro` for
  dynamic executables, and musl built as a shared object.

## 9. Test strategy

Each sub-phase gates independently; none is "done" on a boot sentinel:

- Relocation coverage: a fixture object exercising each supported type, with the
  **refusal** arm for an unsupported one.
- Symbol resolution: interposition order proven by which definition wins.
- W^X: assert no writable-executable mapping exists after entry, and that the
  GOT is read-only.
- Failure arms: missing dependency, unresolved symbol, versioned object — each
  must exit with its named diagnostic.

## 10. Phasing and estimate

| Sub-phase | Content | Gate |
|---|---|---|
| **35.0** | this DDR | — |
| **35.1** | **file-backed `mmap`** (`fd`, `offset`, `MAP_PRIVATE`) | read-back of a mapped file region; refusal arms preserved |
| **35.2** | `PT_INTERP` + full auxv in `elf.c` | a stub interpreter that prints auxv and exits |
| **35.3** | loader self-relocation + `PT_LOAD` mapping + RELRO | loader reaches its own `main` with W^X intact |
| **35.4** | relocations + symbol resolution, eager binding | per-type fixture gate + refusal arms |
| **35.5** | musl as a shared object; a dynamic `hello` | dynamic binary runs; static path unchanged |
| **35.6** | TLS initial-exec | `SYS_SET_TLS` fixture under the loader |

**Estimate: 6 sub-phases, each its own session.** 35.1 and 35.2 are kernel work
with no linker in sight; the loader proper starts at 35.3.

**The honest headline: item 35 cannot begin with the linker.** Its first two
sub-phases are kernel prerequisites, and file-backed `mmap` (35.1) is the real
gate on everything after it.

## Stop point

Per instruction, work stops here for sign-off before 35.1.
