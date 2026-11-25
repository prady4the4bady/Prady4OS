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

## Amendment 2 (2026-10-21 UTC): step 3, the applier and slide-aware users, still slot 0

**Security boundary: review before merge** (K4). The kernel still boots at slot 0, so the slide is 0. This step makes every slide-dependent path compute from the slide instead of a constant, and runs the applier on every boot.

- **Applier** (`arch/x86_64/boot.asm`, before `.bss` is zeroed and before `kmain`):
  - It computes `slide = lea [rel __kimg_lo] − 0xFFFFFFFF80000000`. The constant is an immediate, so it carries no relocation of its own.
  - It decodes `.kreloc` and adds `slide` to each recorded field: a qword for `64`, the low dword for `32S`.
  - It runs at slide 0 too. Every boot then executes the decoder over all 3,562 entries, which is how a decoder fault would show up today.
  - A bad table magic with `slide == 0` is skipped, because nothing would move. With `slide != 0` it halts: a kernel that cannot relocate itself must not run.
  - The slide survives the `.bss` clear in `r14` and is stored in `kaslr_slide`.
- **Self-check** (`kreloc_selfcheck`, early in `kmain`): it decodes the table again in C and checks that every recorded field now holds an address inside the **running** image `[__kimg_lo, __bss_end]`. It prints `[kaslr] table ok entries=N` or `[kaslr] table FAIL …`, and never prints the slide (K1).
  - This catches a table whose offsets do not land on the fields, a failure that the build's `--verify` cannot see, because it shares the encoder's view of the link.
- **Slide-aware users:**
  - `vmm_protect_kernel` and its audit walk `PD_HI[slide >> 21]` and take VAs from `__kimg_lo`;
  - `kimg.c` computes `image_len = __data_end − __kimg_lo`;
  - the kernel-RIP printers pass values through `kaslr_unslide()`, which subtracts the slide only from addresses inside the running image. Those printers are the panic `RIP=`, the backtrace return addresses, the `[apfreeze]` `rip=` and `loser_rip=`. User RIPs and data pointers print raw (§2.5).
- **Build-time slide check.** Every build links the same objects a second time at `KERNEL_VBASE + 2 MiB`. `kreloc.py --slide-check` then replays `boot.asm`'s loop, step for step, over `kernel.bin` with a 2 MiB slide, and requires the result to be **byte-identical** to the higher link.
  - This proves on every build that the table is complete and that the loop decodes it. Measured on this head: 0 differing bytes; the two links differ in exactly 3,577 bytes before the replay.
  - It replays the loop in Python. It does not run the asm. The asm runs in CI on every boot (at slide 0), and with a non-zero slide only in step 4.
- **Gate:** `smoke-wxkernel` also requires `[kaslr] table ok` and forbids `[kaslr] table FAIL`. `GLOBAL_FORBIDDEN` is unchanged.
- **Mutants (each must be shown red in CI before merge):**

  | Mutant | Change | What turns red |
  |---|---|---|
  | K3-1 | `kreloc.py` records every offset +2 (the encoder and `--verify` agree with each other) | the build: `slide-check: relocated image differs` |
  | K3-2 | the applier adds a forced 2 MiB instead of `slide` | every boot gate (fields point 2 MiB off, into unmapped memory). This proves the applier writes the recorded fields. |
  | K3-3 | the W^X walk indexes `PD_HI[(slide >> 21) + 1]` | `smoke-wxkernel`: `[wx] kernel W^X FAIL` |
  | K3-4 | `kreloc_selfcheck` reads each field 1 byte late | `smoke-wxkernel`: `[kaslr] table FAIL` (the in-kernel check can go red) |

- **Not verified by this step:** `kaslr_unslide` and the slot-dependent walk are exercised with a zero slide only. They are proven under a non-zero slide by step 4's arms D and E.

## Amendment 3 (2026-10-21 UTC): step 4, slot choice in both loaders and `smoke-kaslr`

**Security boundary: review before merge** (K4). Step 3's applier is green on every boot gate at slot 0 (#84, run 39521945274). This step makes the slot non-zero.

- **Slot choice (both loaders, same commit).**
  - **Entropy, in order:**
    - RDSEED, when CPUID leaf 7 EBX bit 18 is set;
    - else RDRAND, when CPUID leaf 1 ECX bit 30 is set;
    - each is retried up to 10 times on CF=0;
    - else the TSC (K2, `weak`).
  - **The slot** is `s = (r mod 511) + 1`, in `[1, 511]` (K3).
  - **`stage2.asm`** sets `PD_HI[s] = PT_HI` instead of `PD_HI[0]`, and jumps to `KERNEL_VIRT + s·2 MiB`.
  - **`loader.c`** does the same, with `pdi = s`.
  - **Both pass the entropy source in RSI:** 1 = tsc, 2 = rdrand, 3 = rdseed, 0 = none.
- **Kernel.**
  - `boot.asm` keeps RSI in `r13` across the applier and the `.bss` clear, and stores it in `kaslr_src`.
  - The slot itself is printed by `kputs_serial_only`, which writes the UART and bypasses the `SYS_DMESG` ring (K1): `[kaslr] slot=N`.
  - The ring-visible line names no slot: `PRADYOS_KASLR_B applied=1 src=rdseed|rdrand|tsc`.
- **Gate `smoke-kaslr`.** It boots 3 times with `QEMU_CPU=qemu64,+rdrand,+rdseed`, and every boot must also pass the ordinary boot sentinels.

  | Arm | Pass condition |
  |---|---|
  | A | `PRADYOS_KASLR_A reloc_ok=1`. A function pointer stored in `.rodata` (an `R_X86_64_64` field) and a `mov $sym` (`R_X86_64_32S`) both equal the same function's RIP-relative address. |
  | B | `PRADYOS_KASLR_B applied=1 src=rdseed` or `src=rdrand`. |
  | C | At least 2 distinct `[kaslr] slot=` values across the 3 boots. |
  | D | `probe_kaslr_fault` takes a deliberate #PF under the fault-expect latch, which now records the faulting RIP. It prints `[kaslr] D fault_rip=` with the RIP passed through `kaslr_unslide`. The gate resolves it with `tools/ci/sym_at.sh` against `build/kernel.elf` and requires `probe_kaslr_fault`. |
  | E | `[wx] kernel W^X OK`, `PRADYOS_WX_ALIAS present=1 rw=0 nx=1`, and the text-write fault, all with `s ≠ 0`. |

- **Mutants (each must be shown red in CI before merge).**

  | Mutant | Change | Arm |
  |---|---|---|
  | M1 | the applier skips `R_X86_64_64` entries | A |
  | M2 | the slot is forced to 0 | B |
  | M3 | the slot is a constant (7) | C |
  | M4 | the D printer prints the raw, slid RIP | D |
  | M5 | the W^X walk uses `PD[0]` | E |

- **Every other gate** now boots slid. Under TCG's default `qemu64` that is a weak TSC slot. A missed relocation would fail those gates too, and step 3's build-time slide check already proves the table complete.

## 8. Honesty labels

When built, KASLR will be **gated by `smoke-kaslr` arms A–E**, emulated (QEMU TCG) only, and **not verified on real hardware**.
- Entropy is about 9 bits (K3: 10). That is a probabilistic hurdle, not a secret. Any kernel-address leak defeats it, and raw data pointers in dumps are such a leak.
- A `tsc`-sourced slot is not counted as protection.
- Nothing here is labelled ASI.
- This record does not close OPEN-1, OPEN-2, OPEN-12, OPEN-13 or U1–U3. §2.5 exists to keep their diagnosis intact.

### Amendment 3 note: arm D's first CI run (head `004bd45`, run 39552929205)

Arms A, B and E passed on the first boot (`slot=396`, `src=rdseed`). Arm D printed `fault_hit=0 fault_rip=0x0`, because the probe load never faulted. `movq 0xFFFFFF0000000000, %rax`, written as an absolute memory operand, was encoded as `48 8b 04 25 00 00 00 00`: a disp32 of 0, i.e. `mov 0x0,%rax`. The assembler truncated the 64-bit address without a warning, and address 0 is mapped.

Fix:
- The address is loaded with `movabs` into a register, and the probe is `mov (%reg),%rax`.
- Before arming, `vmm_resolve` checks that the address is unmapped. If it is mapped, `[kaslr] D arm FAIL: probe address is mapped` is printed, which the gate forbids.

The mutant branches are merged forward onto the fix, so each one isolates its own arm.

### Amendment 3: mutant runs (CI evidence, mutants merged forward onto `00fa6b1`; head green, run 40125051487)

| Mutant | Run | What turned red |
|---|---|---|
| M1 the applier skips `R_X86_64_64` | [40158014109](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158014109) | **A.** The boot-time table check `[kaslr] table FAIL entries=3592 bad=336` fires first. The image then never reaches `NEXUS KERNEL OK`, so all ten shards are red. |
| M2 slot forced to 0 | [40158015373](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158015373) | **B:** `PRADYOS_KASLR_B applied=0 src=rdseed` on all three boots (and C: 1 distinct slot). |
| M3 constant slot 7 | [40158016603](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158016603) | **C:** `3 boots, 1 distinct slot(s)`. A and B pass. |
| M4 D printer prints the raw RIP | [40158017791](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158017791) | **D:** each raw RIP resolves to `__bss_end`, not `probe_kaslr_fault`. All three boots and arms A–C pass. |
| M5 W^X walk uses `PD[0]` | [40158019116](https://github.com/prady4the4bady/Prady4OS/actions/runs/40158019116) | **E:** `[wx] kernel W^X FAIL: no PD[slot]`, which is forbidden by every gate, so all ten shards are red. |

All five are red. M2–M5 land on their designed arms. M1 lands on the arm-A condition (the relocations are wrong), but the in-kernel table check catches it before the arm-A line can print.

M5's runs show the slot varying across shards: 116, 330, 416, 191, 98, 88, 234, 340, 321 and 185, which is 10 distinct slots in 10 boots. They also show that only shard 8 boots with RDSEED (`src=rdseed`); the other shards use the TSC fallback (`src=tsc`, K2). Under QEMU TCG that is emulated only, and not verified on real hardware.

## Amendment 4 (2026-11-25 UTC): the recheck before the rebuild, and step 2 again

**Security boundary: review before merge** (K4).

### A4.1 Why the steps are rebuilt

Steps 2, 3 and 4 were #82, #84 and #85. All three were closed unmerged. Operator comment #45/7062649586 item 3 orders them rebuilt as fresh PRs off current `dev/phase1`, in the same order (table, applier, slot choice), after this design is rechecked against three changes made since October:
- the boot loaders' 4 MiB kernel window (DDR-1183 A2);
- user ASLR and the user ELF/`PT_INTERP` loader (DDR-1189 A1–A5);
- the kernel's size.

This file is carried onto `dev/phase1` from `kaslr-4-slot` (`266bb8d`). Everything above this amendment is unchanged. Its measured numbers (§1, A1–A3) describe the October tree; where A4 differs, A4 wins.

### A4.2 The boot loaders: the window is two PT pages, so the slot range shrinks by one

Since DDR-1183 A2, both loaders map the kernel through **two** PD entries:
- `stage2.asm:602-605` sets `PD_HI[0] -> PT_HI` and `PD_HI[1] -> PT_HI2`;
- `loader.c:128-129` sets `pd_hi[pdi]` and `pd_hi[pdi + 1]`.

§2.1 and A3 map one entry, `PD_HI[s]`. Built as written, that would leave the second 2 MiB of the image unmapped. At `s = 511` it would also need a PD entry 512, which does not exist. So:
- **The slot is `s` in `[1, 510]`, `s = (r mod 510) + 1`.** Both loaders write `PD_HI[s] -> PT_HI` and `PD_HI[s+1] -> PT_HI2`, and jump to `KERNEL_VIRT + s·2 MiB`. That is still one commit for both files.
- **K3 becomes 510 slots**, about 8.99 bits. Arm C's chance that three boots of correct code all draw the same slot becomes 1/510², about 3.8·10⁻⁶.
- **`vmm_protect_kernel` and its audit walk `PD_HI[s]` and `PD_HI[s+1]`.** Mutant M5 becomes "the walk uses `PD[0]` and `PD[1]`".
- **The `32S` range still holds.** At `s = 510` the image ends 1,024 MiB above `0xFFFFFFFF80000000`, inside the top 2 GiB.
- **The physical side is unchanged.** The physical load address stays `0x400000`, so the `__bss_end <= 0x800000` ceiling check (physical, DDR-1183 A2.5) is unaffected.

**The user ELF/`PT_INTERP` loader** (DDR-1189 A3, A5) maps lower-half user addresses only, and reads no kernel-image address. It is unaffected.

### A4.3 Fixed-base users: one new one since A2

A sweep of `kernel/`, `arch/` and `boot/` for `0xFFFFFFFF8` in non-comment code, on `dev/phase1` at `f868ca0`, finds the two loaders, `kernel.ld`, `kimg.c:8,15`, and `vmm.c:84` (`KERNEL_VBASE_WX`, used at `vmm.c:110,187,214,215`). Step 3 converts every one of these to slide-aware code:
- **`vmm.c:96-99,110,187`, the W^X walk and audit.** As before (A2), but over two PD entries (A4.2).
- **`kimg.c:15`, `image_len()`.** As before.
- **`vmm.c:214-215`, `vmm_ptspan_probe`. This one is new: DDR-1183 A2, gate `smoke-ptspan`.**
  - It computes the image's physical end as `__bss_end − KERNEL_VBASE_WX + 0x400000`. Under a slide, `__bss_end` is relocated, so that sum is the physical end **plus the slide**.
  - Its `hi=` test (`end > 0x600000`) would then hold for any slot, whatever the image size. The arm would pass vacuously.
  - Step 3 subtracts `__kimg_lo` instead.

### A4.4 User ASLR

§6's "No user-space ASLR" is superseded.
- **User ASLR exists:** DDR-1189 A1, in `kernel/crypto/aslr.c`. Its sources are `rng_bytes`, then RDRAND, then the TSC, which is labelled `aslr: weak entropy`.
- **It is independent of KASLR.** The slot is drawn by the loaders before the kernel runs, so `aslr.c` cannot serve it, and the two share no state. Both label a TSC-only source as weak; neither counts it as protection.
- **No kernel address leaks through `fstat`.** DDR-1189 A5 made `fstat` report `st_dev = mnt + 1`. `mnt` is an `int` mount id (`kernel/fs/vfs/vfs.h:27`), not a pointer.
- **§2.5 and K1 stand.** Kernel RIP printers unslide, and the slot goes to the serial console only.
- **§8's stated leak stands.** Raw data pointers in dumps are a leak. Step 3 lists, in its PR description, every printer of an image address that it finds.

### A4.5 Size, measured on `dev/phase1` at `f868ca0`

- **Relocations.** An `ld.lld --emit-relocs` re-link of the current objects gives:
  - 3,475 `R_X86_64_32S` and 389 `R_X86_64_64`, so **3,864** fields (October: 3,562);
  - alongside 6,058 `PLT32` and 1,979 `PC32`, which need no patching;
  - the same four types, in `.rela.text`, `.rela.data` and `.rela.rodata`, and 0 absolute fields.
- **The encoded table is 4,994 B** (1.29 B per entry, the same rate as A1).
  - A1's 6,144 B reservation would leave 19% margin, so the reservation is raised to **8,192 B**, which leaves 64%.
  - K2-1 (a reservation of 1,024 B) still trips.
- **`kernel.bin` with step 2: 1,724,824 B,** measured locally. That is +8,198 B over `dev/phase1`'s 1,716,626 B, and leaves **44,648 B of headroom** under the 1,769,472 B gate.
  - The `RCU_TORTURE=1`/`LOCKDEP=1` build, which binds, grows by the same 8,198 B, to about 1,741,208 B, about 28,264 B of headroom. That figure is expected, not measured. The step 2 PR states the sizes CI prints.
- **Step 3's build-time slide check** (A2) links a second, unshipped image at `+2 MiB`. That adds to build time, not to `kernel.bin`.

### A4.6 Order and who merges

1. **Step 2 is this PR:** this record, `.kreloc`, `kreloc.py` and the build checks. The kernel still runs at slot 0 and nothing reads the table. Its mutants are K2-1 to K2-4 (A1), each shown red in CI before merge.
2. **Step 3:** the applier, the in-kernel self-check, the build-time slide check, and the slide-aware users of A4.3. Still slot 0.
3. **Step 4:** slot choice in both loaders, with the A4.2 range, and `smoke-kaslr` arms A–E.

**Paths.** `AGENTS.md` allows no new flat files in `kernel/`, so the reservation lives in `kernel/mm/kreloc.c` and `kernel/mm/kreloc.h` (A1 names them `kernel/kreloc.*`). `kernel.bin` depends on `tools/kreloc/kreloc.py`, so a changed generator rebuilds the table.

Under K4, the operator reviews and merges each step; 7062649586 item 3 names step 4, the #85 replacement, explicitly. I do not merge any of them.

QEMU (TCG) only, not verified on real hardware. Nothing here closes OPEN-1, OPEN-2, OPEN-12, OPEN-13 or U1–U3.

### A4.7 Step 3 as built (the applier, still slot 0)

Ported from #84 (`3ddb03d`) onto step 2. It adds the applier in `boot.asm`, `kreloc_selfcheck`, `kaslr_unslide` and the build-time slide check as in A2. On top of A2:
- **The W^X walk** reads `PD_HI[slot]` and `PD_HI[slot+1]` (A4.2). A missing entry prints `[wx] kernel W^X FAIL: no PD[slot]` or `no PD[slot+1]`.
- **`vmm_ptspan_probe`** takes its base from `__kimg_lo` (A4.3).
- **Five code-address printers added since #84 now unslide** (A4.4):
  - the `#BP` self-test RIP (`idt.c`);
  - the double-free caller in `pmm.c`;
  - the three `caller=` return addresses in `sfs.c`.

  The saved return addresses in `[apfreeze]`'s `bt=` list unslide too, like its `rip=` (Codex on #169). The percpu pointers in `syscall.c`'s `gs FAIL` line are data and stay raw (§2.5).
- **Measured at slot 0:** 3,879 entries (3,490 `32S`, 389 `64`), a 5,015 B table, and `kreloc: slide-check OK` (the +2 MiB relocation equals the +2 MiB relink).

**Mutants** (A2's K3-1 to K3-4, each to be shown red in CI before merge):
- **K3-3 under two PT pages:** the walk indexes `PD_HI[slot+1]` and `PD_HI[slot+2]`. `PD_HI[2]` is empty, so it must print `no PD[slot+1]`.
- **The A4.3 and A4.4 conversions cannot be shown red at slot 0,** because there the slide is 0 and old and new code agree. They are proven under a non-zero slide in step 4: arm D for the printers, and `smoke-ptspan` booted slid for the probe.

### A4.8 Step 4 as built (slot choice in both loaders)

Ported from #85 (`266bb8d`) onto step 3. It is A3 with the A4.2 range.
- **The slot is `s = (r mod 510) + 1`, in `[1, 510]`, in both loaders.**
  - `stage2.asm` maps `PD_HI[s] -> PT_HI` and `PD_HI[s+1] -> PT_HI2`, then jumps to `KERNEL_VIRT + s·2 MiB`.
  - `loader.c` maps `pd_hi[s]` and `pd_hi[s+1]`.
- **The port found the A4.2 hazard in code, not only on paper.** `loader.c` merged without a conflict, and the merged code mapped `pd_hi[pdi]` and `pd_hi[pdi + 1]` while still drawing `r % 511 + 1`. At `s = 511` that writes entry 512, one past the end of the PD page, silently. Both loaders are fixed to 510 in the same commit.
- **The kernel side is A3 unchanged:**
  - `kaslr_src` comes from RSI;
  - the slot is printed by `kputs_serial_only` (K1);
  - `PRADYOS_KASLR_B` names only the source;
  - arm D's probe is `probe_kaslr_fault` under the fault-expect latch.
- **`smoke-kaslr`** boots 3 times with `QEMU_CPU=qemu64,+rdrand,+rdseed` and checks arms A to E. `tools/ci/kaslr_check.sh` judges B, C and D across the boots.
- **Every other gate now boots slid.** Under TCG's default `qemu64` that is a weak TSC slot (K2). That is how the A4.3 probe change (`smoke-ptspan`) and the A4.4 printer changes are first exercised under a non-zero slide.
- **Mutants M1 to M5** as in A3, with:
  - M2 and M3 setting the slot in both loaders;
  - M5 walking `PD[0]` and `PD[1]`.
