# DDR-765 — NVMe controller bring-up + Identify (M2 driver 1/2)

**Status:** implemented — gate `smoke-nvme` PASS (`QEMU NVMe Ctrl ns1 32768 LBAs
x 512 B`); image builds `-Werror`-clean. 100 gates.
**Layer:** 3 (drivers). M2 storage completeness. First of two NVMe slices.

## Problem

PRADYOS has virtio-blk as its only block driver. Real hardware (and QEMU's
`-device nvme`) uses NVMe. M2 calls for an NVMe driver that registers with the
generic block layer (`blk_register`, like virtio-blk). NVMe is large, so it is
split: **this slice = controller bring-up + Identify** (prove the hardware path);
**DDR-766 = I/O queue + read/write + `blk_register`** (the block device).

## Decision — `kernel/drivers/nvme/nvme.c`

Detect and initialize an NVMe controller; no block I/O yet.

1. **Detect** (kmain PCIe loop): a device with `class_code == 0x01 &&
   subclass == 0x08` (mass storage / NVM). Vendor-agnostic (QEMU's is 8086:5845).
2. **PCI setup:** enable memory space + bus master in the command register
   (config 0x04), like `virtio_pci_attach`.
3. **BAR0 → MMIO:** read BAR0 (config 0x10, handle 64-bit), map it uncached
   (`vmm_map … VMM_RW | VMM_PCD`) — the register-mapping pattern from
   `virtio_pci.c:map_bar`.
4. **Registers** (NVMe 1.x, BAR0-relative): `CAP`@0x00 (u64: MQES, DSTRD, TO),
   `VS`@0x08, `CC`@0x14, `CSTS`@0x1C, `AQA`@0x24, `ASQ`@0x28 (u64), `ACQ`@0x30
   (u64), doorbells @ `0x1000 + (2*qid + is_cq) * (4 << CAP.DSTRD)`.
5. **Reset + configure:** `CC.EN=0`; poll `CSTS.RDY==0` (bounded). Allocate a
   PMM page each for the admin SQ (64-byte entries) and admin CQ (16-byte
   entries); zero them. `AQA` = (asqs-1)<<16 | (acqs-1) (use 63/63 → one page
   each). `ASQ`/`ACQ` = the phys addresses. `CC` = IOCQES(4)<<20 | IOSQES(6)<<16
   | MPS(0)<<7 | CSS(0)<<4 | EN(1). Poll `CSTS.RDY==1` (bounded; if it never
   readies → `[nvme] controller not ready`, abort cleanly).
6. **Identify** (admin opcode 0x06): a 4 KiB PRP data page. Submit **Identify
   Controller** (CNS=1) — 64-byte command in the admin SQ (opcode, CID, PRP1 =
   data-page phys), bump the SQ tail doorbell, poll the admin CQ for the phase-bit
   flip + status==0. Read the model number (bytes 24..63, 40 chars). Then
   **Identify Namespace** (CNS=0, NSID=1) → NSZE (total LBAs, offset 0) and the
   active LBA format's data size (LBADS in `lbaf[FLBAS&0xf]`, `2^LBADS` bytes).
7. Print `[nvme] <model> ns1 <NSZE> LBAs x <lbasize> B` and set a "ready" flag
   for DDR-766.

Polling completion (no NVMe IRQ this slice) is fine for bring-up + Identify; the
CQ phase bit is the standard poll. All waits are `g_ticks`-bounded so a missing
or wedged controller can never hang the boot.

## Gate — `smoke-nvme` (new; 99 → 100)

`boot_test.sh` gains a `QEMU_NVME` knob (like `QEMU_GPU`/`QEMU_SMP`) that appends
`-drive if=none,id=nvm,file=build/nvme.img,format=raw -device nvme,serial=PRADYOSNV,drive=nvm`.
The gate builds a small `build/nvme.img` and asserts `[nvme] ` + a model/LBA line
(the Identify round-trip). Every other gate omits `QEMU_NVME`, so the driver is a
no-op there (no NVMe device present → detection finds nothing).

## Non-goals (this slice)

- No block read/write, no `blk_register` (that's DDR-766).
- No NVMe interrupts (polling for bring-up).
- No multiple namespaces / multiple controllers (NSID 1, first controller).
- No power-management / SMART / admin commands beyond Identify.
