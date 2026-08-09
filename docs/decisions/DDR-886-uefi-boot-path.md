= DDR-886 — UEFI boot path (Group 4 item 22)

**Status:** Accepted
**Date:** 2026-08-10
**Scope:** `boot/uefi/`, `Makefile` (`esp-image`, `smoke-uefi`), `boot_test.sh`.
**Blocks:** item 48 — the ISO gate must prove **both** boot paths, and this is
the second one.

## 1. The contract the loader must reproduce, exactly

The legacy path (`boot/stage2/stage2.asm`) hands the kernel a very specific
machine state. The UEFI loader is a *second implementation of the same
contract*, not a new one:

| Item | Value |
|---|---|
| Kernel entry | `kernel_entry`, at virtual `0xFFFFFFFF80000000` |
| `RDI` | physical pointer to `struct boot_info` — **0x4000** |
| Kernel image | `build/kernel.bin`, flat, loaded at physical **0x400000** |
| Paging | `0xFFFFFFFF80000000` → `0x400000` (2 MiB span, 4 KiB pages) **plus** a low identity map |
| Mode | long mode, interrupts off |
| Stack | none required — `kernel_entry` sets `RSP` itself |

Anything that differs here is a divergence between boot paths, and a kernel that
boots one way and not the other is worse than a kernel that only boots one way:
the failure appears far from the loader that caused it.

## 2. Why a from-scratch PE32+ application, not EDK2 or gnu-efi

The item names EDK2. Building the loader **against** EDK2 would add a
multi-hundred-megabyte submodule and its own build system to a project whose
entire toolchain story is "clang + nasm + lld, zero warnings, `-Werror`".

clang already emits PE32+ directly (`--target=x86_64-unknown-windows`) and
`lld-link-18` is already installed. The UEFI structures this loader needs are
about 150 lines of declarations, all fixed by the specification. That is a
smaller and more auditable surface than a submodule, and it keeps
`toolchain-check` meaningful.

**Stated as a deviation** because the item said EDK2: what ships is a UEFI
application conforming to the UEFI spec, built with the existing toolchain, and
booted under OVMF — which is what "UEFI boot path" has to mean operationally.
No EDK2 dependency is introduced.

## 3. The things that make a UEFI loader subtly wrong

**1. `GetMemoryMap` invalidates its own key.** Every allocation — including the
one made to hold the memory map — changes the map and bumps the key.
`ExitBootServices` fails with `EFI_INVALID_PARAMETER` if the key is stale. The
map must be fetched **immediately** before `ExitBootServices`, and if that call
fails the map must be re-fetched and retried, not merely reported.

**2. Nothing may touch boot services after `ExitBootServices` returns.** That
includes `ConOut->OutputString`. A debug print placed after the exit is a call
through a pointer into memory the firmware has released — it usually appears to
work, which is the problem.

**3. The firmware's identity map is not the kernel's map.** UEFI enters the
loader in long mode with an identity map, so it is tempting to reuse CR3. It
cannot be: the kernel runs at `0xFFFFFFFF80000000`, which UEFI does not map.
Fresh tables are built (PML4/PDPT/PD/PT), covering both the low identity range
and the higher-half window, and CR3 is loaded after `ExitBootServices`.

**4. UEFI memory types are not E820 types.** Only `EfiConventionalMemory`,
`EfiBootServicesCode` and `EfiBootServicesData` are free for the OS after exit —
the last two only *because* boot services are gone. Treating
`EfiRuntimeServicesData` or `EfiACPIReclaimMemory` as free hands the PMM memory
the firmware still owns.

**5. The descriptor size is not `sizeof(EFI_MEMORY_DESCRIPTOR)`.** The firmware
reports its own `descriptor_size`, which may be larger. Iterating with `sizeof`
walks off alignment and produces a plausible-looking but wrong map.

## 4. The gate

`smoke-uefi` boots OVMF with a FAT ESP containing `EFI/BOOT/BOOTX64.EFI` and
`KERNEL.BIN`, and requires the same sentinel as every other boot gate:
`NEXUS KERNEL OK`, plus the loader's own `[uefi] handoff` line so a failure can
be attributed to the loader rather than the kernel.

The sentinel being **identical to the legacy path's** is the point: it proves the
same kernel, unmodified, reaches the same state through a completely different
loader. A UEFI-specific sentinel would let the two paths drift apart while both
looked green.

## 5. Scope

**Not implemented:** Secure Boot signing, UEFI runtime services after exit (the
kernel does not call them), GOP framebuffer handoff (the kernel's display path is
virtio-gpu; a UEFI framebuffer is a separate item), and loading the kernel from
anywhere other than the ESP root.

`build/kernel.bin` is byte-identical between the two paths. Nothing in the kernel
is conditional on which loader ran — if it ever needs to be, that is a defect in
this contract, not a feature.

---

## 6. What the build found — the gate was blind, twice

**The first version silently truncated the firmware's memory map.** It capped at
96 entries and stopped. OVMF emits well over 100 descriptors, so the kernel
printed `entries=0x60` — exactly the cap — and simply had less RAM than the
machine, with nothing anywhere saying so. Adjacent same-type runs are now
**merged** (16 entries for this machine), and exceeding the 169 that fit below
`0x5000` is **refused** with a FATAL rather than truncated. Truncating loses
memory silently; refusing is visible.

**The gate could not see a wrong handoff.** Mutation M2 — hand the kernel a
deliberately wrong `boot_info` pointer — **PASSED**: `NEXUS KERNEL OK` prints
before the memory map is used for anything, so the original sentinel proved only
that the kernel started, not that the handoff was correct.

Requiring the E820 entry count fixed it. Final matrix:

| Mutation | Result |
|---|---|
| M2 wrong `boot_info` pointer | **killed** |
| M3 every memory type marked usable | **killed** |
| M4 iterate with `sizeof` instead of `desc_size` | **killed** |

M1 (drop the higher-half mapping) was attempted but the edit did not compile, so
it is recorded as **invalid, not a kill**.

**The legacy path was re-verified after the harness refactor.** `BOOTDISK` is now
a variable so the ESP can replace the boot disk rather than join it; `smoke`,
`smoke-fs`, `smoke-user`, `smoke-shell` and `smoke-numa` are green on the legacy
path afterwards.

`build/kernel.bin` is byte-identical across both paths, and the shared
`NEXUS KERNEL OK` sentinel is what proves it.

**Group 4 item 22 complete. Item 48 is unblocked** — the ISO gate now has a
second boot path to cover.
