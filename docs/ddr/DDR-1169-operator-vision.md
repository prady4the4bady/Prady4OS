# DDR-1169 — Operator vision page, drift annotation, assembly inventory

**Status:** docs-only. No code change, `kernel.bin` unchanged. Operator
instruction: PR #44 comment 6177173413 (OWNER).

## 1. `docs/VISION.md`

A single page stating the operator's standing vision: agents operate the
machine through capability-checked syscalls only (no ambient authority, no
self-granting); project starts and code rewrites stay forced `PENDING` and a
rejection stops and rolls back the work; inference is local Ollama first with
cloud deferred until authorized; no "ASI" labels; quantum stays withdrawn.
`CLAUDE.md` and `SESSION_HANDOFF.md` now point to it as the first file to read.

Where the vision describes something not built (rollback, the computer-use
surfaces), the page says so and points to DDR-1168.

## 2. "ASI" labels removed

Headings in `docs/AETHER_MASTER_FEATURES.md`, `docs/PRADYOS_MASTER_PLAN.md`,
`docs/design/LAYER7_UI_UX_BRIEF.md` and `PRADYOS_Blueprint.md`, plus two Python
comments and one `.gitignore` comment, are relabelled; each heading keeps a
"formerly …" note so old references still resolve. Dated entries in
`SESSION_HANDOFF.md` are a historical record and are left as written. No UI
string contained the label (`git grep -w ASI` over `kernel/` and `user/`
returns nothing).

## 3. `PRADYOS_Claude_Instructions.md` drift

A banner at the top, and an inline **SUPERSEDED (DDR-1169)** note at each
drifted section: chipset variants (only x86_64 ships; aarch64/riscv64 are
boot-only stubs, ADR-034), Rust (none; the only `.rs` file is a toolchain
smoke test), the aarch64 assembly mirror, the Rust init (`user/init.c` is C),
the Wayland/wlroots/Rust compositor (`user/compositor.c` is a custom C
framebuffer compositor), the KWin/wlroots blur, the variant table, and the
repository-structure tree (`arch/aarch64` and `arch/riscv64` hold only
`.gitkeep`). The quantum section already carried a WITHDRAWN note. The
original text is kept so the record shows what was planned.

## 4. Assembly inventory (operator item 4)

`docs/FEATURE_STATUS.md` mentions assembly in **one** row: "CPU model matrix"
(runtime CPUID dispatch for ERMS memcpy/memset). No row tracks the assembly
routines themselves.

| Routine | C reference + differential test | Benchmark |
|---|---|---|
| `fast_memset.asm` (DDR-1076) | **Yes** — `memset_selftest` (`kernel/main.c`) compares both forced paths against an explicit byte loop, 28 cases, gated on `smoke-bench` | no |
| `fast_memcpy.asm` (DDR-871) | **No** direct differential test; exercised only indirectly by every `memcpy` caller | no |
| `ipc_copy.asm` (DDR-873) | **No** C reference; end-to-end content readback on `smoke-sendipc` (`back=0xA71C0001`) | no |
| `context.asm` (context switch) | n/a (not algorithmic) | **Yes** — `user/benchtest.c` `SYS_YIELD` round trip, `smoke-bench`, TCG-emulated ticks only |
| `syscall_entry.asm` | n/a | **Yes** — `benchtest.c` `SYS_GETPID` round trip, TCG-emulated only |
| `isr.asm`, `cpu.asm`, `boot.asm`, `ap_boot.asm`, `usermode.asm` | n/a (entry/bring-up glue) | no |
| `user_image.asm`, `install_blobs.asm` | n/a (`incbin` data only) | no |

The benchmark numbers are QEMU TCG emulated ticks and are valid for
regression only, never as hardware cycles (`benchtest.c` header, DDR-870).

**Gap recorded, not fixed here:** `fast_memcpy` and `ipc_copy32` have no
differential test against a C reference. A `memcpy` differential arm on the
`memset_selftest` pattern (both forced ERMS paths, odd alignments, lengths
0/1/7/8/4095/4096, compared to a byte loop) is the obvious next item.

## 5. Not claimed

No code changed; no gate added; no capability, action type or behaviour
changed. DDR-1168 is scope only.
