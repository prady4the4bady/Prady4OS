# DDR-1142 — A UEFI GOP framebuffer, so the desktop has a display without virtio-gpu

**Status:** design committed before code (`0fe3334`, §NON-NEGOTIABLE 5); **BUILT
and gated** in the commit that adds §5 below. Operator go: PR #17 comment
**5827611413** (OWNER-verified), item (1).
**Operator instruction:** PR #17 comment 5822830053, item 3 (OWNER-verified):
*"UEFI GOP framebuffer path as a real driver change. Name the mechanism and test
it. If it cannot be verified without physical hardware, say so and name the
closest verifiable proxy."*

## 1. What is there today, measured

- **The only display path is virtio-gpu.** `sys_fb_info`, `sys_fb_map` and
  `sys_fb_flush` (`kernel/syscall/sys_fb.c`) all call `virtio_gpu_fb()`. With no
  virtio-gpu device all three return `-ENODEV`, and the compositor has nothing
  to draw on.
- **Real UEFI machines do not have virtio-gpu.** On a PC booted through the
  release ISO's UEFI arm, the firmware has already set a video mode and
  published it through the **Graphics Output Protocol (GOP)**: a linear
  framebuffer at a physical address, with a width, a height, a stride and a
  pixel format. The loader (`boot/uefi/loader.c`) never asks for it, so that
  mode is thrown away at `ExitBootServices`.
- `sys_fb_map` assumes the framebuffer's kernel pointer **is** its physical
  address (`phys = (uintptr_t)fb`). That holds for virtio-gpu, whose buffer is
  PMM memory inside the low identity map. It does **not** hold for a GOP
  framebuffer, which is device memory typically above 1 GiB (QEMU std-vga/OVMF
  puts it at `0x80000000`), outside the identity map.

## 2. The mechanism

### 2.1 Loader: query GOP before `ExitBootServices`

- `HandleProtocol(ST->ConsoleOutHandle, &EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID)`.
  The console-out handle is the one the firmware is drawing its own text on, so
  its GOP is the one attached to a real display. Falls back to no framebuffer if
  absent.
- Read `Mode->Info` and `Mode->FrameBufferBase`. **The mode is not changed** —
  no `SetMode`. The firmware already chose a mode the attached panel supports,
  and switching it is a second thing to get wrong on hardware this project
  cannot test. Stated as a limitation: the desktop runs at the firmware's
  resolution.
- **Accepted formats:** `PixelBlueGreenRedReserved8BitPerColor` only. Its byte
  order in memory (B, G, R, X) is exactly what the compositor already writes
  for virtio-gpu's `B8G8R8A8_UNORM`, so no conversion is needed.
  `PixelRedGreenBlueReserved8BitPerColor`, `PixelBitMask` and `PixelBltOnly`
  are **recorded and refused**: the handoff carries the format, and the kernel
  declines to use a framebuffer whose colours it would draw wrong. A panel
  showing nothing is honest; a panel showing swapped colours is a wrong answer
  that looks right.

### 2.2 Handoff: a separate block, so the pinned header does not move

`struct boot_info`'s 32-byte header is `_Static_assert`-pinned in both paths and
is followed by a flexible `e820[]`. Nothing can be appended to it.

- A new `struct boot_fb` (32 bytes) at fixed physical **`0x4FE0`**, the last 32
  bytes of the boot_info page:
  `magic 'GFB1' | format | base (u64) | width | height | stride_px | check`,
  where `check = magic ^ format ^ base_lo ^ base_hi ^ width ^ height ^ stride_px`.
- **The E820 cap drops from 169 to 167** so the entries can never reach it
  (`32 + 167 × 24 = 0xFC8 < 0xFE0`). The loader's own `die()` at the cap
  already refuses rather than truncates.
- **§INV.13 — both boot paths.** stage2.asm's E820 loop (`BIOS INT 15h`) is
  capped too, and stage2 **zeroes** the block, so the BIOS path's answer is "no
  framebuffer", stated in memory, not left to whatever the BIOS left behind.
  The kernel accepts the block only when magic **and** check both match.

### 2.3 Kernel: one display source, two backends

- New `kernel/drivers/gpu/display.c`:
  `display_fb(&w, &h, &stride_bytes, &phys)` returns virtio-gpu if it is up,
  otherwise the GOP framebuffer if the handoff was valid; `display_present()` is
  virtio-gpu's present, or `0` for GOP (a linear framebuffer **is** the
  scanout, so there is nothing to flush).
- The GOP framebuffer is mapped into the kernel at a fixed window
  **`0xFFFFD40000000000`** (next to the existing ECAM/BAR/NVMe windows, checked
  unoccupied), `VMM_RW | VMM_NX`.
- `sys_fb.c` and `virtio_input.c`'s geometry query switch to `display_*`.
  `sys_fb_map` maps the **physical** address the backend reports, not the
  kernel pointer, which removes the `phys == kvirt` assumption rather than
  papering over it.
- Boot prints one line either way:
  `[fb] gop <w>x<h> stride=<px> fmt=<n> base=<hex> used=<0|1>` or
  `[fb] gop none`.

## 3. Verification: what can be shown here, and what cannot

**Physical hardware cannot be tested in this environment.** The closest
verifiable proxy is **OVMF on QEMU's std-vga**. OVMF's `QemuVideoDxe` publishes a
real GOP over a real PCI BAR. The path from `HandleProtocol` to pixels in the
scanout is therefore the same code a physical board runs. What differs is the
firmware's GOP driver, and that cannot be covered here.

**The gate is `smoke-gop`: an OVMF boot with no virtio-gpu.**

- **Arm G — the handoff arrived.** The kernel prints
  `[fb] gop 1280x800 stride=1280 fmt=1 base=0x0000000080000000 used=1`, with
  the exact values measured on the proxy's first boot and pinned. A loader that
  never queried GOP prints `gop none`.
- **Arm S — the pixels reached the scanout.**
  - **The obvious arm is vacuous.** "The kernel wrote a pattern and read it
    back" passes on plain RAM: a mapping to the wrong address reads back
    whatever it was just given.
  - So a kernel self-test fills four quadrants with four distinct colours and
    prints `[fb] gop pattern drawn`.
  - The gate then takes a **QMP `screendump`**, which is what the emulated
    display is actually showing. It asserts each quadrant's centre pixel.
  - A write that did not reach the device, went to the wrong base, or used the
    wrong stride leaves OVMF's own screen in the dump.
- **Arm B — the BIOS path says `gop none`.** This is §INV.13's discipline: the
  path that has no GOP must state it, not inherit garbage.

**Mutants, each on a recorded hash:**
- **M1:** the loader skips the GOP query. Fails G and S.
- **M2:** the stride is ignored and `width` is used instead. Fails S when the
  mode's stride differs from its width. If the proxy's mode has
  `stride == width`, M2 is **undetectable here**, and that is recorded rather
  than claimed covered.
- **M3:** the kernel maps the wrong physical base (`+ 1 MiB`). Fails S.

## 4. Not claimed

- **No physical UEFI machine has been tested.** OVMF's GOP driver is not a
  vendor's.
- There is no `SetMode`: the firmware's resolution is used.
- Formats other than BGRX are refused, not converted.
- There is no BIOS-path framebuffer (VBE). The BIOS arm has no display without
  virtio-gpu, exactly as before.
- The mapping uses the default cache type, not write-combining. It is slower on
  real hardware, and correct.

## 5. Results, measured on the proxy

### 5.1 The proxy's mode

The first boot printed the values arm G now pins:

```
[fb] gop 1280x800 stride=1280 fmt=1 base=0x0000000080000000 used=1
```

`fmt=1` is `PixelBlueGreenRedReserved8BitPerColor`, the only accepted format.
**The stride equals the width on this proxy**, which decides M2 below.

### 5.2 Arms, as shipped

`smoke-gop` (shard 5, strict, 140 s budget; one run measured 137 s) is
`tools/ci/gop_gate.sh`. It runs QEMU directly, because arm S needs a QMP
socket that `boot_test.sh` does not open.

- **G:** the exact line above.
- **S:** after `[fb] gop pattern drawn`, a QMP `screendump` of the scanout. The
  centres of the four quadrants must read red, green, blue and yellow.
  Measured: `TL (320,200)=(255,0,0) TR (960,200)=(0,255,0) BL (320,600)=(0,0,255)
  BR (960,600)=(255,255,0)`.
- **C, added during the build: a design gap in §3.** Arms G, S and B cover the
  kernel's own mapping, and **none of them reaches `sys_fb_map`**. That is the
  path the ring-3 compositor draws through, and the one line of this DDR that
  changed existing behaviour (`phys` from the backend instead of `phys == kvirt`).
  - So the gate attaches the FAT and SFS disks, the compositor comes up on the
    GOP framebuffer, and after `PRADYOS_AMBIANCE_OK` a second screendump must
    show **none** of the four kernel pattern colours at the quadrant centres.
  - Measured: `(71,24,0)` on the top row and `(49,17,0)` on the bottom row,
    which is the ambiance gradient and not the pattern.
- **B:** the BIOS image boots through `boot_test.sh` and must print
  `[fb] gop none`. `[fb] gop rejected` is forbidden.

### 5.3 Mutants

The pre-mutation tree is loader `5f97545fc4b8bfe5` and kernel `9ff230a9dc3395ec`
(1,352,074 B). Each mutant changes one line.

| Mutant | Change | Hash | Result |
|---|---|---|---|
| M1 | loader returns before the GOP query | loader `84b10dd2b6a9675a` (kernel unchanged) | **Fails G and S**: `<no [fb] gop line>`, no screendump. B passes. |
| M2 | kernel uses `width` as the stride | kernel `c3610a000b1a908e` | **PASSES every arm.** It is undetectable on this proxy because stride == width (§5.1). Recorded as uncovered, as §3 said it might be. |
| M3 | kernel maps `base + 1 MiB` | kernel `f4e62f929b1728c9` | **Fails S alone**: all four centres wrong (`(0,0,0)`, `(0,0,0)`, `(255,0,0)`, `(0,255,0)`). G passes, so G and S are independent. |
| M4 | `sys_fb_map` maps `phys + 1 MiB` | kernel `835ce75161add71b` | **Fails C alone**: two centres still show the kernel pattern (`STILL THE KERNEL PATTERN`). G and S pass. **This is the mutant that proves arm C was needed.** |

After M2 and M4 the revert rebuilds to `9ff230a9dc3395ec` **bit-for-bit**.

**The loader's hash is not reproducible, and that is not a defect of this
change.** `lld-link` stamps the PE header's `TimeDateStamp` with the build time
(measured `0x6ab6ae15` in the current `BOOTX64.EFI`). Restoring `loader.c`
**byte-identical** to its pre-M1 source (checked with `cmp`) rebuilt to
`3a916d25f6cae0ff`, not `5f97545fc4b8bfe5`. So a loader mutant is attributed
by its **source diff and its behaviour**, never by its hash. The kernel links
with `ld.lld` and does reproduce. `/Brepro` would fix the loader. It is
**recorded, not applied**, because it changes the ISO's bytes for no
functional gain days before a tag.

### 5.4 Regression: UEFI boots now have a display

This is a behaviour change, and it is the risk the regression run was aimed
at. Before this DDR, a UEFI boot with no virtio-gpu had no framebuffer, so the
compositor exited at `SYS_FB_INFO`. **Now it runs.** A retained `smoke-uefi`
capture (471 lines) reads:

```
[fb] gop 1280x800 stride=1280 fmt=1 base=0x0000000080000000 used=1
PRADYOS_COMPOSITOR_OK 1280x800
PRADYOS_AMBIANCE DAWN … DAY … DUSK … NIGHT
PRADYOS_AMBIANCE_OK
```

- **UEFI:** `smoke-uefi`, `smoke-iso-x86` (both arms) and `smoke-iso-userspace`
  all rc=0 on the shipped kernel.
- **virtio-gpu is untouched:** `smoke-shell` 5/5, `smoke-blkmq`,
  `smoke-rqstress-liveness`, `smoke-blk-integrity` and `smoke-selftest` were
  run before the push. They are recorded in `SESSION_HANDOFF.md` with this
  commit.
- `kernel.bin` is 1,352,074 B, **size unchanged**, so the size/headroom pair
  and `ci-docstate-check` are unaffected. Only the hash moved.

### 5.5 Not claimed, restated after measurement

- **No physical UEFI machine was tested.** Everything above is OVMF's
  `QemuVideoDxe` on q35 std-vga. A vendor's GOP driver may pick a mode whose
  stride differs from its width. That is exactly the case M2 shows this gate
  cannot see.
- There is no `SetMode`, no format other than BGRX, no BIOS framebuffer and no
  write-combining, as §4 said.
- No new sentinel. GLOBAL_FORBIDDEN stays 77. The gate count goes 181 → 182.
