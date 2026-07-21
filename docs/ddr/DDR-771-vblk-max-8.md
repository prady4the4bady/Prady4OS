# DDR-771 — lift `VBLK_MAX` 4 → 8 (MSI-X vector remap)

**Status:** implemented — `smoke-aether-sfsroot` boots 5 virtio-blk disks; PASS
(`blk4 ready` + AETHER daemon roots at the provisioned image at blk4 alongside
ext4). Root-caused a #GP the driver-only change first triggered (see below).
Unblocks DDR-770's "provisioned root as the DEFAULT, alongside ext4".

## Problem

The virtio-blk driver caps at `VBLK_MAX = 4` instances with four hardcoded MSI-X
handlers at `VBLK_MSIX_BASE = 50` (vectors 50–53). virtio-net sits at vector 54
and virtio-input at 55 — packed immediately after — so a 5th disk (unit 4) would
claim vector 54 and collide with net. Today that means a boot with
boot+fat+sfs+ext4 (4 disks) has no room for a 5th, so DDR-770's provisioned root
disk only works with ext4 suppressed (`QEMU_NO_EXT4`).

## Vector map (confirmed from the tree)

`48` LAPIC timer · `49` SMP wake IPI · `50–53` virtio-blk · `54` virtio-net ·
`55` virtio-input · `0xFF` spurious. virtio-gpu uses no MSI-X. **56–63 are free**
(nothing in the tree uses ≥56), and the IDT has 256 vectors.

## Decision

The MSI-X window is infrastructure shared by the IDT and the ISR stubs, so this
touches three files (a driver-only change #GP'd — see Gotcha):

1. `kernel/drivers/blk/virtio_blk.c`: `VBLK_MSIX_BASE` 50 → **56**, `VBLK_MAX`
   4 → **8**; add per-device handlers `vblk_msix4..7` (each `complete(&g_inst[i])`)
   and extend `vblk_msix_fn[]` to 8. (`BLK_MAX = 8` already.)
2. `arch/x86_64/isr.asm`: add `ISR_NOERR 56..63` stubs and grow the
   `isr_stub_table` `%rep` 56 → **64**.
3. `kernel/idt.c`: install IDT gates for `i < 64` (was 56) and widen
   `MSIX_VEC_COUNT` 6 → **14** (window 50..63) so `msix_register` accepts 56..63
   and `isr_dispatch` routes them. net@54 / input@55 keep their handler slots.

### Gotcha (root-caused)

A driver-only base change first produced a **#GP, error 0x1C2** on the first blk
completion: `0x1C2 >> 3 = 56`, IDT bit set — i.e. IDT vector 56 had no gate. The
IDT loop stopped at 55 and `msix_register` rejected ≥56, so the relocated
interrupt fired into an uninstalled vector. Fixed by extending the ISR stubs +
IDT gates + `MSIX_VEC_COUNT` above.

## Gate — extend `smoke-aether-sfsroot` (DDR-770)

Drop `QEMU_NO_EXT4` and add the ext4 disk, so the gate boots **five** virtio-blk
disks: boot(0)/fat(1)/sfs(2)/ext4(3)/sfsroot(4). Assert `blk4 ready` (the 5th
disk registered — impossible under the old cap) **and** that the AETHER daemon
still roots at the provisioned image (now blk4) → `PRADYOS_AETHER_CFG_OK
mode=sovereign`. This proves >4 disks work AND the provisioned root coexists with
ext4 — the DDR-770 default-path goal.

## Non-goals

- Making the provisioned root the literal default in the normal (non-gate) boot
  and retiring blk2's dual role — a follow-on now that the disk cap is lifted.
- MSI-X multi-vector per device / per-queue vectors (still one vector per disk).
