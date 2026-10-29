# DDR-1179 — KASLR (C1.7): virtual slide, relocation table, unslid diagnostics

**Status:** ACCEPTED (decisions K1-K4 recorded below; design only, no code in this PR). Supersedes the deferral in
DDR-1051 because `docs/V1_MASTER_CHECKLIST.md` C1.7 puts KASLR back in scope
("was deferred, now in scope"). DDR-1051's reasons are not dismissed. Each one is
answered below by a design rule or an owner decision (§7).
**Needs:** C0.1 (rollback item ticked in #65). It does not depend on C1.3 or C1.4.

## 1. Current state, measured on the tree at `dev/phase1` = `9bf5815`

- **Kernel build.** It is built `-fno-pic -fno-pie -mcmodel=kernel` (`Makefile:259-262`) and linked at `KERNEL_VBASE = 0xFFFFFFFF80000000`, `KERNEL_LMA = 0x400000` (`kernel/kernel.ld:19-24`). The link carries no relocations (`Makefile:792`, no `--emit-relocs`).
- **Relocation count.** I re-linked the current objects with `ld.lld --emit-relocs` into a scratch ELF (not committed). The measured relocations are:

  | Type | Count | Needs patching under a slide? |
  |---|---|---|
  | `R_X86_64_PLT32` | 5,586 | no (PC-relative) |
  | `R_X86_64_PC32` | 1,866 | no (PC-relative) |
  | `R_X86_64_32S` | 3,226 | **yes** (all in `.text`) |
  | `R_X86_64_64` | 336 | **yes** (335 in `.rodata`, 1 in `.data`) |

  So **3,562** fields need patching. At 4 bytes per entry the raw table is **14,248 B**. That fits the current headroom (40,566 B under the 1,572,864 B gate), but it is a third of it; see §3.3.
- **Two boot paths build PT_HI** and jump to the fixed `KERNEL_VIRT`:
  - `boot/stage2/stage2.asm:552-618` (`PML4[511]->PDPT_HI[510]->PD_HI[0]->PT_HI`, then `mov rax, KERNEL_VIRT; jmp rax`);
  - `boot/uefi/loader.c:102-130,396-405`.
- **Nothing else maps the top 2 GiB.** I grepped `kernel/`, `arch/` and `boot/` for the `[510]`/`[511]` walks and for other `0xFFFFFFFF…` bases; the only users are the two boot paths and `vmm.c`'s W^X walk.
- **Fixed-base assumptions in the kernel:**
  - `kernel/mm/vmm.c:83-113,176-183`: the W^X walk hardcodes `PD[0]` and computes VAs from `KERNEL_VBASE_WX`.
  - `kernel/kimg.c:8,14-16`: `image_len() = __data_end - KERNEL_VBASE`. Under a slide this becomes `len + slide`, which is wrong.
  - `Makefile:795-797`: the link-time PT_HI span check. It uses physical addresses, so it is unaffected.
- **Not affected:**
  - the AP trampoline: `smp.c:384` stores `(uint64_t)smp_ap_entry` at run time, which is relocated like any other pointer;
  - the boot entry stub: `arch/x86_64/boot.asm:29-42`, all RIP-relative;
  - the pristine-image hash: `kimg.c:83-87` hashes the boot-time *pristine copy*, which is never relocated.
- **Entropy at boot.**
  - RDRAND is unused. RDSEED is only used driver-time (`kernel/drivers/rng/virtio_rng.c:37-52`).
  - CI's default CPU model (qemu64, TCG; no `-cpu` unless `QEMU_CPU` is set, `boot_test.sh:649`) has neither, so most gates print `[rng] source=NONE`.
  - The TSC is available.
- **Who can read the log.** Every `kputs` line also goes into the kernel log ring, which user processes read with `SYS_DMESG` (`kernel/console.c:89-97`). Printing the slide there would hand it to every process.

## 2. Decision: what is built

**A virtual slide in 2 MiB steps. The physical load address stays at `0x400000`.**

- Keeping the physical address fixed leaves alone the DDR-1046 identity-alias rule (kernel at low PD entry 2), the `KIMG_PHYS` window and the pristine copy.
- DDR-1051's physical-only variant is rejected. It defends against a physical-write primitive that DDR-1046 already removed.

1. **Slot choice (loaders).** Both boot paths draw a slot `s` in `[1, 511]`, map `PD_HI[s] -> PT_HI` instead of `PD_HI[0]`, and jump to `KERNEL_VIRT + s·2 MiB`. Slot 0 is excluded so a working slide can be proved (§4, arm B). Both files change in the **same commit** (CLAUDE.md, Hard-won facts: PT_HI is implemented twice).
2. **Self-relocation (kernel).** `boot.asm` computes `slide = (lea [rel kernel_entry]) − KERNEL_VBASE`. Before `.bss` is cleared and before `kmain`, it walks the relocation table and adds `slide` to each recorded field: 4 bytes for `32S`, 8 bytes for `64`. The loaders pass nothing new; `BOOT_INFO` is unchanged.
   - The `32S` fields stay in range for every slot in `[1, 511]`, since the whole image stays inside the top 2 GiB.
   - PT_HI maps the image RW at this point. W^X is applied later in `vmm_protect_kernel`, so patching `.text` is possible; after W^X, `.text` cannot be patched.
3. **The table.**
   - `kernel.ld` reserves a fixed-size `.kreloc` section after `.data`.
   - The kernel is linked once with `--emit-relocs`.
   - A host tool, `tools/kreloc/kreloc.py`, extracts the `32S` and `64` field offsets into a sorted, delta-encoded table and writes it into `.kreloc` with `objcopy --update-section` before `-O binary`.
   - The build **fails** if the table exceeds the reservation, or if any relocation type other than the four in §1 appears. An unknown type is a design change, not something to skip.
4. **Fixed-base users made slide-aware.** `vmm.c`'s W^X walk uses `PD[s]` and the actual base; `kimg.c` computes `image_len()` from two symbols (`__data_end − kernel_entry`), never from a constant.
5. **Diagnostics stay resolvable without publishing the slide.** Every RIP or code-address printer subtracts the slide and prints *link-time* (unslid) addresses. That covers the panic dump and backtrace (`kernel/idt.c:1035-1075`) and `[apfreeze]` (`idt.c:230-250`). So:
   - `tools/ci/sym_at.sh`, `sym_bt.sh` and the §INV.18 rule (resolve every RIP against its own `kernel.elf`) keep working unchanged;
   - the OPEN-1/2/12/13 instrumentation keeps its meaning.

   Data pointers in dumps (e.g. `cr2`) stay raw. Printing them is how the leak is bounded; it is not removed.
6. **Entropy.** The order is RDSEED, then RDRAND, then TSC, each behind a CPUID check and a bounded retry. The source is reported as `src=rdseed|rdrand|tsc`.
   - A `tsc` slot is labelled **weak** and does not count as KASLR protection (§8).
   - The slide is still applied with a TSC slot, so all ~196 gates exercise the relocation path, not only the KASLR gate. Whether this fallback is acceptable is owner decision **K2**.

## 3. Risks

1. **A missed relocation.** A missed field is an absolute pointer to the unslid address, which is unmapped. It faults deterministically on first use, and arm A plus the full gate set catch it. The worse case is a field that is only read on a rare path. Mitigation: the build rejects unknown relocation types, and the table is generated from the link, never hand-kept.
2. **Something compares a code address against a constant.** Found so far: `vmm.c` W^X and `kimg.c`. The implementation PR greps for every `0xFFFFFFFF8` literal in non-comment code and lists each one in its description.
3. **Size.** 14,248 B raw (measured) out of 40,566 B headroom. Delta encoding should roughly halve it; that is **not verified**. The implementation PR reports the encoded size and the new headroom, and updates CLAUDE.md's size line through the operator.
4. **`make image` staleness.** Flag-only changes do not rebuild (CLAUDE.md, Hard-won facts). The implementation verifies by hash.
5. **UEFI.** `loader.c` allocates the image at the fixed `KERNEL_LMA`. That is unchanged, because only the virtual mapping slides. `smoke-iso-x86` (BIOS and UEFI) must stay green.

## 4. Gate `smoke-kaslr` (shard to be chosen by load, strict)

The gate boots with `QEMU_CPU=qemu64,+rdrand,+rdseed`, three times. All three boots must also pass the ordinary boot sentinels.

| Arm | Sentinel | Pass condition | Vacuity check on the unfixed tree |
|---|---|---|---|
| A | `PRADYOS_KASLR_A reloc_ok=1` | A function pointer stored in `.rodata` (an `R_X86_64_64` field) equals the same function's RIP-relative address, **and** an absolute `32S`-materialised address (`mov $sym`) equals its RIP-relative address. | The sentinel does not exist today. Red. |
| B | `PRADYOS_KASLR_B applied=1 src=rdseed` (or `rdrand`) | The slide is non-zero and came from a hardware source. | Red (absent). |
| C | 3 boots, at least 2 distinct slots | The serial-only slot line (K1) differs across boots. With 511 slots, the chance that all three are equal on correct code is 1/511² ≈ 3.8·10⁻⁶. | Red (absent). |
| D | Resolved RIP | A probe takes a deliberate, caught fault in a named function, `probe_kaslr_fault`. The gate script resolves the printed `rip=` with `tools/ci/sym_at.sh` against `build/kernel.elf` and requires that symbol. | Red (absent). |
| E | `smoke-wxkernel` lines under a slide | The existing W^X sentinels (`PRADYOS_WX_ALIAS present=1 rw=0 nx=1` and the text-write fault) hold with `s ≠ 0`. | It passes today only with `s = 0`. That is a regression arm, recorded as such. |

**Mutants. Each lands on a different arm and must be shown red in CI before merge:**

| Mutant | Change | Arm it must turn red |
|---|---|---|
| M1 | The applier skips `R_X86_64_64` entries | A |
| M2 | Slot forced to 0 | B |
| M3 | Slot constant (e.g. 7) | C |
| M4 | Panic printer prints the raw, slid RIP | D (`sym_at` resolves to the wrong symbol or none) |
| M5 | W^X walk uses `PD[0]` | E (`[wx] kernel W^X FAIL`) |

## 5. Scope split (one PR per concern, in this order)

1. This DDR.
2. The build-side table: `.kreloc`, `kreloc.py` and the checks, still with slot 0. This adds no behaviour; the gate is the size and type check in the build.
3. The applier plus the slide-aware `vmm.c`/`kimg.c`/printers, still with slot 0. Green on every existing gate.
4. Slot choice in **both** loaders, plus the `smoke-kaslr` gate, the mutants and the evidence.

## 6. What this does not do

- No user-space ASLR. The mmap base and stack are fixed (`vmm.h:36,73`); that belongs to C1.6.
- No physical randomisation.
- No change to the 1,572,864 B size gate.

## 7. Owner decisions required (stop condition: not covered by an owner decision)

- **K1, slot visibility.** May the slot be printed on the **serial console only**, bypassing the `SYS_DMESG` ring? The serial line is operator/CI access, not readable by local processes. Without K1, arm C cannot be measured, and the gate drops to arms A, B, D and E. A salted hash of a 9-bit value is brute-forceable, so it is not offered as a middle option.
- **K2, fallback.** With no RDSEED/RDRAND, apply a TSC slot labelled `weak` (recommended: the relocation path is then exercised by every gate), **or** boot at slot 0 with `[kaslr] DISABLED no-entropy`?
- **K3, range.** 511 slots under `PDPT_HI[510]` (~9 bits, one PD; recommended) **or** 1,023 slots across `PDPT_HI[510..511]` (~10 bits; both loaders build a second PD)?
- **K4, security review.** KASLR is a hardening feature, and it touches both boot paths and the W^X setup. Under #45 it is the operator's to merge; I do not self-merge.

## Decisions (Order 4, #45/6301435736 item 3)

These are the §7 recommendations, now decided:
- **K1: serial only.** The slot is printed on the serial console and bypasses the `SYS_DMESG` ring, so local processes cannot read it. Arm C stays in the gate.
- **K2: weak TSC fallback.** Without RDSEED or RDRAND, the slot comes from the TSC and is labelled `src=tsc` (weak). Every gate therefore runs the relocation path. A `tsc` slot is never counted as protection (§8).
- **K3: 9 bits.** Slots 1-511 under `PDPT_HI[510]`, one PD.
- **K4: built in PRs flagged "security boundary: review before merge".** The operator reviews and merges; I do not self-merge.

The §5 scope split proceeds in order: build-side table, then the applier and slide-aware users at slot 0, then slot choice and `smoke-kaslr`.

## Amendment 1 (2026-10-21 UTC): step 2, the relocation table (no behaviour change)

**Security boundary: review before merge** (K4). This step adds data and build checks only. No code reads the table yet, and the kernel still runs at slot 0.

**Measured on this tree** (`ld.lld --emit-relocs` re-link of the current objects): 3,226 `R_X86_64_32S` + 336 `R_X86_64_64` = **3,562** fields, all in `.rela.text`, `.rela.rodata` and `.rela.data`. This is the same count as §1. No field holds an absolute constant (0 skipped).

- **Format** (`tools/kreloc/kreloc.py`), little-endian:
  - `"KRL1"`, then `u32 count`, then `u32 payload bytes`;
  - then `count` LEB128 varints of `(offset − previous) << 1 | is64`, where `offset` is from the link base and strictly increasing.
  - Measured size: **4,584 B** (1.29 B per entry), against the §3.3 raw estimate of 14,248 B.
- **The reservation.** `.kreloc` is a fixed **6,144 B**: `kreloc_table[KRELOC_RESERVE]` in `kernel/kreloc.c`, with `KRELOC_RESERVE` in `kernel/kreloc.h`. That leaves 34% margin.
  - It sits after `.data` and before `__data_end`, so the pristine image copy and `SYS_INSTALL` (`kimg.c`, `[KERNEL_VBASE, __data_end)`) carry it.
  - It is data for W^X (RW+NX), like `.data`.
- **Build steps:**
  1. Link with `--emit-relocs`.
  2. `kreloc.py` writes the padded table.
  3. `llvm-objcopy --update-section .kreloc=…`.
  4. `kreloc.py --verify` re-reads the table **from the section in `kernel.elf`** and decodes it against the link.
  5. `-O binary`.
- **Each of these fails the build:**
  - an unknown relocation type;
  - a table larger than the reservation;
  - a `.kreloc` section whose size differs from `KRELOC_RESERVE`;
  - `entries + absolute_skipped` differing from an **independent** `llvm-readelf -r` count of `R_X86_64_64|32S`;
  - an embedded table that does not decode to the link's fields.
- **Which fields are recorded.** A `64`/`32S` field is recorded when the value the linker wrote into it lies inside `[__kimg_lo, __bss_end]`. A field outside that range holds an absolute constant and must not slide; such fields are counted and reported.
- **Mutants (each must be shown red in CI before merge; a red build is the gate):**

  | Mutant | Change | Check it must trip |
  |---|---|---|
  | K2-1 | `KRELOC_RESERVE` = 1,024 | table over the reservation |
  | K2-2 | a `@GOTPCREL` reference added to `kreloc.c` (`R_X86_64_REX_GOTPCRELX`) | unknown relocation type |
  | K2-3 | `kreloc.py` drops `32S` entries | the `llvm-readelf` cross-count |
  | K2-4 | the `--update-section` step removed | `--verify` reads an all-zero section (magic) |

**Vacuity:** on the tree before this step, none of these checks exists, so no relocation error can fail the build. Each mutant shows that its check trips.

**Size:** `kernel.bin` grows by 6,144 B plus alignment. The new size and headroom are in the PR description, measured.

## 8. Honesty labels

When built, KASLR will be **gated by `smoke-kaslr` arms A–E**, emulated (QEMU TCG) only, and **not verified on real hardware**.
- Entropy is about 9 bits (K3: 10). That is a probabilistic hurdle, not a secret. Any kernel-address leak defeats it, and raw data pointers in dumps are such a leak.
- A `tsc`-sourced slot is not counted as protection.
- Nothing here is labelled ASI.
- This record does not close OPEN-1, OPEN-2, OPEN-12, OPEN-13 or U1–U3. §2.5 exists to keep their diagnosis intact.
