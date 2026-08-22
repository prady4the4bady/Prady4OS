# DDR-971 — Manual end-to-end verification of the built ISO: it boots a kernel, not an OS

Status: ACCEPTED — **verification FAILED**. Number verified free in **both**
`docs/ddr/` and `docs/decisions/` (§INV.4).
Subject: `build/pradyos.iso` built from `main` @ `7c6c67a`.

This is the first time anyone has booted the built ISO and read the whole serial
log instead of grepping it for one sentinel. The result changes the release
plan, so it is recorded before anything else.

## 1. What was actually done

| step | result |
|---|---|
| `make iso` on `main` @ `7c6c67a` | **OK** — `build/pradyos.iso`, **52,805,632 B**, sha256 `8a5e6507e18954e1` |
| `make smoke-iso-x86` (BIOS + UEFI arms) | **PASS** — the gate is green |
| read the FULL BIOS serial (145 lines) | done |
| read the FULL UEFI serial (129 lines) | done |
| control arm: same kernel booted the normal way | done — and it is what makes this diagnosis safe |

Tooling note: `xorriso` and OVMF were **absent** from the build container and had
to be installed before `make iso` could run at all. The ISO target has therefore
never been exercised in this environment before today.

## 2. The finding

**The ISO boots the kernel to `NEXUS KERNEL OK` and then idles forever with no
userspace.** From the BIOS arm, verbatim:

```text
[ahci] controller ready, disks=0
[blk] no block device
[fs] no mountable filesystem found
```

and then 23 consecutive heartbeats of

```text
[hb] t=... rqdepth=1 rqcpus=1 pmmfree=28171 preempt=5689 supp=0 curpid=0
```

`rqdepth=1`, `curpid=0`, `preempt` advancing: the scheduler is healthy and has
nothing to run. The UEFI arm is identical (`[blk] no block device`,
`[fs] no mountable filesystem found`).

## 3. The control arm — this is NOT a kernel defect

Same commit, same `kernel.bin`, booted by `smoke-shell` with the three
virtio-blk disks the normal gates attach:

| probe | normal boot | ISO boot |
|---|---|---|
| `mounted` | 4 | **0** |
| `PRISM_READY` | 1 | **0** |
| `prism>` prompts | 50 | **0** |
| `aetherd` | 4 | **0** |
| `[user] ELF loaded` | 26 | **0** |
| `AGENT ROSTER slots=` | 1 | **0** |

The kernel is fine. The **ISO packaging** is the defect.

## 4. Root cause

Three facts compose:

1. **The ISO carries only two files** — `build/isoroot/boot/pradyos.img`
   (2,097,152 B, the boot disk) and `build/isoroot/boot/esp.img` (50,331,648 B,
   the UEFI ESP). Neither contains a mountable root.
2. **The root filesystems are separate disks.** Every working gate attaches
   `build/fat.img` (67,108,864 B) and `build/sfs.img` (16,777,216 B) as
   additional `virtio-blk-pci` devices (`boot_test.sh:107-113`). The ISO ships
   neither, and `smoke-iso-x86` attaches no `-drive` at all beyond `-cdrom`.
3. **The kernel cannot read the medium it booted from.** Stage-1/stage-2 reach
   the ISO through BIOS `INT 13h` under El Torito hard-disk emulation, but once
   in long mode the kernel speaks only virtio-blk, AHCI and NVMe. The CD is
   ATAPI, which is why AHCI reports `disks=0`. There is **no ramdisk/initrd
   facility** — `grep -rln "ramdisk\|initrd" kernel/` returns nothing, and
   `blk_register()` (`blk.c:9`) has exactly two implementors, virtio_blk and
   nvme.

So the ISO is a **kernel-only boot medium**. On real hardware it would behave
exactly as observed here: boot, print, idle.

## 5. Why the gate passed anyway

`smoke-iso-x86` asserts `NEXUS KERNEL OK` (plus `[uefi] handoff` on the UEFI
arm). `NEXUS KERNEL OK` prints at **line 30 of 145**, immediately after the TSS
is loaded — roughly 60 lines before userspace would start. Everything the gate
checks is true; everything a user would care about is untested.

This is the same class as the vacuous checks already catalogued in
`build_status.md`: an assertion that cannot distinguish the working system from
the broken one. It is not a lazy gate — DDR-896 wrote it deliberately to prove
"the same unmodified kernel reaches the same state through a different loader",
and it does prove exactly that. The error is in reading it as evidence the ISO
is usable.

## 6. Manual checklist items 3–6: BLOCKED, not passed

The requested walkthrough could not be performed and **is not recorded as
passing**:

| check | status |
|---|---|
| PRISM shell: `ls ps cat echo touch rm mv date uptime dmesg free uname` | **BLOCKED** — PRISM never starts from the ISO |
| AETHER daemon/agents visible in roster/ps | **BLOCKED** — `aetherd` never spawns |
| lwIP TCP loopback passing real traffic | **BLOCKED** — not reached |
| compositor/framebuffer + window lifecycle | **BLOCKED** — no `virtio-gpu` in the ISO run, compositor never starts |

Those capabilities *are* separately evidenced on the normal boot path (the
control arm above, and `smoke-shell`'s own assertions drive all thirteen
builtins). What is unevidenced is that **the ISO** delivers them.

## 7. Decision

**`v1.0.0` is NOT tagged.** The instruction was to tag only after the manual
pass succeeds. It did not succeed. Tagging now would attach a release version to
an image that boots to an idle kernel.

`main` @ `7c6c67a` is still a good commit — merged, promoted, and green three
times over. The defect is in what `make iso` packages, not in what was merged.

## 8. What a fix requires

The ISO must carry a root filesystem the kernel can reach **after** the boot
loader hands off. The options, with the constraint that `kernel.bin` is
1,053,054 B against a 1,572,864 B gate (519,810 B of headroom):

- **A — ramdisk `blk_device` backed by an embedded image.** Architecturally the
  cleanest: `blk_register()` is already the seam virtio_blk uses, so VFS, PRISM
  and AETHER need no changes. Cost: the image must fit the remaining headroom,
  so it cannot be the 64 MiB test FAT image; it needs a purpose-built minimal
  root. Also raises the stage-2 read window question again (§INV.18).
- **B — stage-2 loads a second payload from the boot disk into memory** as a
  ramdisk. Keeps `kernel.bin` small; costs a new loader contract, which DDR-896
  explicitly avoided taking on.
- **C — ATAPI/`ISO 9660` read support**, so the kernel can mount the CD it
  booted from. Most "correct" and by far the most work: a new driver plus a new
  filesystem.

**A is the recommended direction** and B is the fallback if headroom binds.
None of the three is a small change, and none should be rushed into a release
tag — which is the substantive reason the tag waits rather than a procedural one.

## 9. What would refute this

- The ISO booting to `PRISM_READY` on any machine → the diagnosis is wrong and
  the missing userspace was environmental. Nothing in the two logs supports
  that: `[blk] no block device` is the kernel's own report.
- A hidden root inside `esp.img` that the kernel can mount → would make this a
  configuration bug rather than a packaging gap. The ESP is a FAT image the UEFI
  loader reads via firmware services **before** handoff; the kernel has no
  driver that can reach it afterwards.
