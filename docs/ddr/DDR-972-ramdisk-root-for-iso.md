# DDR-972 — a PMM-backed ramdisk root, so the ISO boots an OS instead of a kernel

Status: ACCEPTED. Written before the code it governs (§NON-NEGOTIABLES 5).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (§INV.4).
Fixes the defect recorded in **DDR-971**.

## 1. The defect being fixed

DDR-971 measured it: `make iso` produces an image whose kernel reaches
`NEXUS KERNEL OK` and then idles at `rqdepth=1 curpid=0` forever, because

```text
[blk] no block device
[fs] no mountable filesystem found
```

The ISO carries no root filesystem, and after handoff the kernel cannot read the
ATAPI CD it booted from. A control arm (same `kernel.bin`, normal 3-disk boot →
PRISM_READY, aetherd, 26 ELF loads) proved the kernel itself is fine.

## 2. Why this is much smaller than DDR-971 §8 assumed

DDR-971 sized three options against `kernel.bin`'s 519,810 B of headroom,
assuming a root image had to be **embedded**. Re-reading the build removes that
assumption entirely:

```make
sfs-image:
	dd if=/dev/zero of=$(SFS_IMG) bs=1M count=16 status=none
	@echo "sfs: ... (16 MiB blank — kernel formats in place)"
```

**The SFS root is a blank device the kernel formats at boot.** Nothing needs to
be embedded, so none of the three constraints DDR-971 worried about apply: no
`kernel.bin` growth, no stage-2 window raise (§INV.18), no PT_HI change
(§INV.13), no new loader contract, no build tooling.

The fix is a block device backed by memory rather than a disk.

## 3. Decision

`kernel/drivers/blk/ramdisk.c` — a `struct blk_device` whose backing store is a
physically contiguous PMM allocation.

> **Read this first: the SHIPPED topology is three of these, not one.** The
> single-device design described in this section mounted fine but did not start
> PRISM, because the userspace bring-up block is gated on `blk_count() > 2` and
> uses `blk_get(2)`. What ships is:
>
> | device | role |
> |---|---|
> | `blk0` | small blank stand-in for the (unmountable) boot disk |
> | `blk1` | 4 MiB SFS root — the filesystem userspace actually runs on |
> | `blk2` | 4 MiB blank scratch — **this is the one `sfs_format()` formats**, and the one `blk_get(2)` returns |
>
> So the "finds it with no change at all" claim below is about `blk2`. See
> *One design correction found by measurement* for how that was discovered.

The per-device properties are the same in either topology:

- `pmm_alloc_pages(order)` for **order 10 = 1024 frames = 4 MiB**. `pmmfree` at
  this point is 28,171 frames (~110 MiB), so the cost is 3.6% of free memory.
- Zeroed on allocation. `vfs_mount` probes for FAT32/SFS/ext4 signatures, and
  unzeroed PMM pages could present garbage that probes as a corrupt filesystem
  rather than a blank one.
- `read`/`write` are `memcpy` against `base + lba*512`, bounds-checked against
  `capacity_sectors`. `blk.h` requires physically contiguous, identity-mapped
  buffers, which a single buddy allocation satisfies.

### The guard that makes this safe

`ramdisk_init()` is called from `kmain` immediately after the PCIe device loop,
**only when `blk_count() == 0`**.

That condition is the whole safety argument. All 147 gates boot through
`boot_test.sh`, which attaches at least one `virtio-blk-pci` disk, so
`blk_count()` is never 0 for any of them and the ramdisk is never registered.
**The existing gate suite cannot be perturbed by this change** — not "should
not", *cannot*, because the code path is unreachable when any real block device
exists. The ISO is the only configuration that reaches it.

Then `sfs_format()` on the newly registered device, reusing the exact call the
SFS self-tests already make on `blk_get(2)`. `fs_test_thread`'s existing loop

```c
for (unsigned j = 0; j < blk_count(); j++) {
    int id = vfs_mount(j);
    if (id >= 0) { mnt = id; blk = (int)j; break; }
}
```

then finds it with no change at all. VFS, PRISM, AETHER and the compositor are
untouched.

## 4. What this does and does not give the ISO

**Gives:** a booting OS with a writable root — PRISM starts, `ls`/`touch`/`echo >`
/`cat`/`rm` work, `aetherd` spawns, the roster populates.

**Does not give:** persistence. A ramdisk is volatile, so anything written is
lost at power-off. That is correct behaviour for a live ISO and should be stated
in the release notes rather than quietly implied otherwise.

**Does not give:** the seeded files the FAT root carries (`/HELLO.TXT`,
`/DOCS/NOTE.TXT`, `/BIG8K.TXT`). The ISO's root starts empty. Seeding it would
need content embedded in `kernel.bin`, which is the DDR-971 §8 problem this
design avoids; if the ISO later needs seed files, that is a separate decision
with the headroom budget attached.

## 5. Why not the alternatives

- **Embedding a root image** (DDR-971 §8 A as originally framed) — unnecessary
  once the kernel-formats-blank fact is accounted for, and it would spend
  headroom for no gain.
- **Stage-2 second payload** (§8 B) — a new loader contract, which DDR-896
  deliberately declined to take on for the ISO.
- **ATAPI + ISO 9660** (§8 C) — a driver plus a filesystem, and it buys a
  *read-only* root where a ramdisk gives a writable one. Worth doing if the ISO
  ever needs to carry large content; not worth it to make the ISO boot.

## 6. Verification bar

The bar is the DDR-971 walkthrough re-run, not a sentinel:

1. `make iso`, boot the BIOS arm, and read the log for `mounted`, `PRISM_READY`,
   `prism>`, `aetherd` — all of which were **0** in DDR-971.
2. Same for the UEFI arm.
3. Exercise the PRISM builtins over the serial console and confirm each *does
   what it claims* — the DDR-971 items 3–6 that are currently BLOCKED.
4. `smoke-shell` 5/5 and the §HYGIENE set, to show the `blk_count() == 0` guard
   really does leave the disk-backed path untouched.
5. A new gate, `smoke-iso-userspace`, asserting the ISO reaches `PRISM_READY` —
   so the "green gate, dead image" gap DDR-971 found cannot reopen.

Point 5 is the one that matters long-term. `smoke-iso-x86` proves the loader
handoff and will keep proving only that; the new gate is what would fail if the
root disappeared again.

## 7. What would refute this

- `vfs_mount` refusing the formatted ramdisk → the probe order or the SFS
  superblock write is wrong, not the ramdisk.
- Any existing gate changing behaviour → the `blk_count() == 0` guard is not
  holding, and the change must be reverted before anything else.
- `pmm_alloc_pages(10)` returning 0 on the ISO → 4 MiB contiguous is not
  available that early; drop the order and re-measure rather than falling back
  to a smaller root silently.


## 8. Measurement (R1 — kernel sha256 `9763ce7bb259de7e`, 1,053,054 B)

ISO: `build/pradyos.iso`, 52,805,632 B — **unchanged in size**, because nothing
is embedded.

| gate | result |
|---|---|
| `smoke-iso-userspace` (new) | **PASS** — SFS root + PRISM + AETHER agent + write/read/delete + GPU + lwIP loopback |
| `smoke-iso-x86` | PASS (both arms, unchanged) |
| `smoke-shell` | **5/5** |
| `smoke-blkmq` / `smoke-rqstress-liveness` / `smoke-blk-integrity` / `smoke-fsrm` | PASS |
| `ci-shard-check` | OK — **148** gates, 6 shards, 6 excluded |
| `ci-probe-rodata-check` | OK |

Build warning-clean at `-Werror`.

### The guard was verified, not assumed

The safety argument in §3 is that `blk_count() == 0` makes the ramdisk
unreachable whenever a real disk exists. That was checked rather than trusted:
**`grep -c ramdisk` on a disk-backed `smoke-shell` boot returns 0.** The string
appears nowhere, so the branch genuinely never ran, and the 147 pre-existing
gates cannot have been perturbed.

### One design correction found by measurement

The first implementation registered a single ramdisk. It mounted, and the FS
layer worked on it — `[fs] wrote /KOUT.TXT (17 bytes)`, read back verbatim,
`created+deleted /TMP.TXT OK` — but **PRISM still did not start**, because the
userspace bring-up block is gated on `blk_count() > 2` and uses `blk_get(2)`.

Rather than relax that gate — it is on the path all 147 gates traverse — the fix
mirrors the topology the boot already expects: blk0 a small blank stand-in for
the (unmountable) boot disk, blk1 the 4 MiB SFS root, blk2 the 4 MiB blank
scratch the existing code formats itself. No shared code changed.

That correction is worth recording because the single-ramdisk version *looked*
like it worked: the mount succeeded and files round-tripped. Only checking for
`PRISM_READY` — the thing a user would actually see — showed it had not.
