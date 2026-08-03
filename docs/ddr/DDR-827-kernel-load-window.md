# DDR-827 — ACC does not fit: the 768 KiB stage-2 load window is full

**Status:** §Blocker recorded, fix designed, NOT implemented
**Date:** 2026-08-03
**Blocks:** DDR-813 (ACC syscalls 77/78), and therefore DDR-814 (AGS) and
DDR-815 (rotation).

## §What happened

ACC is the first kernel feature that needs the **whole** crypto stack resident.
Every primitive before it followed the DDR-811 precedent — built, gated by a
ring-3 probe that links the source a second time, and deliberately **not** in
the kernel link until a caller existed. ACC is that caller.

Linking `acc.o` plus its five dependencies (`x25519`, `fe25519`, `hkdf`, `aead`,
`ed25519`, `sha512`) and `sys_acc.o`:

```
before:  kernel.bin  774,502 bytes
after:   kernel.bin  799,078 bytes   (+24,576)
limit:               786,432 bytes   (768 KiB, DDR-733)
over by:              12,646 bytes
```

The Makefile's own size gate caught it, and the resulting image **does not
boot** — `smoke` fails with no kernel sentinel, because stage 2 reads only
24 × 64 sectors from LBA 17 and the tail of the kernel is never loaded.

**The tree has been reverted to a booting state** (774,502 bytes, `smoke`
PASSES). `acc.{c,h}`, `sys_acc.c`, the NSI 77/78 numbers and the ACC audit codes
are all committed; only the *link* and the `sys_acc_register()` call are backed
out, with a comment at the call site pointing here.

## §Why this is worth a DDR rather than a quick bump

The obvious move is "raise the chunk count". The Makefile message even says how.
But the constraint is a chain, and changing one link silently breaks another:

1. **Stage-2 read window** — `24 × 64` sectors from LBA 17 = 786,432 B.
2. **Disk image size** — `truncate -s 1M`. The kernel starts 8,704 B in, so a
   1 MiB image holds at most **1,039,872 B** of kernel. Raising the window to
   32 chunks (1,048,576 B) exceeds what the image can even contain, so the image
   must grow **in the same change** or stage 2 reads past the end.
3. **The 2 MiB PT_HI runtime ceiling**, which the Makefile checks immediately
   after the size gate. The kernel is relocated to 4 MiB and its page tables
   assume a bounded image; that ceiling is the real upper bound, not the load
   window.

Three coupled numbers in three different files. That is exactly the shape of
thing this project has been bitten by seven times — change one, the others drift
silently. So it gets a design note and its own verification, not a sed.

## §Design — raise the window to 1.5 MiB, in one commit

- `boot/stage2/stage2.asm`: chunk count **24 → 48** (48 × 64 × 512 = 1,572,864 B).
- `Makefile`: `truncate -s 1M` → `-s 2M` on `build/pradyos.img`, and the size
  gate 786,432 → 1,572,864.
- Re-check the PT_HI ceiling assertion still holds at the new size; 1.5 MiB of
  kernel plus the 4 MiB relocation base stays under 2 MiB of page-table reach
  **only if** that ceiling is measured from the load base, which must be
  confirmed by reading the assertion rather than assumed.

**Verification, and it must be all four:**
1. `make image` — size gate passes at the new limit.
2. `make smoke` — boots. This is the one that actually proves stage 2 read the
   whole kernel; the size gate only proves the file is small enough.
3. `make smoke-user` + `make smoke-fs` — the tail of the image is really there
   (a short read corrupts the *end* of the kernel, so a gate that only touches
   early code would pass against a broken load).
4. A gate at `-smp 4` — the AP trampoline lives in the image too.

## §Alternative considered and rejected

**Keep ACC out of the kernel and expose it as a ring-3 library.** Rejected: the
kernel owns the keys. The entire reason ACC is a syscall is that a ring-3 agent
must never see `K_session`, the ephemeral scalar, or the owner's private key. A
library version would be a different, weaker feature wearing the same name.

## §Status of DDR-813 after this

- `kernel/crypto/acc.{c,h}` — written, **host-verified** (seal/open round-trip,
  tamper-ct and tamper-sig → `ACC_ERR_AUTH`, replay → `ACC_ERR_REPLAY`,
  owner-read-after-reboot → `ACC_OK`), both spec bugs fixed.
- `kernel/syscall/sys_acc.c` — written, capability split decided
  (seal = CAP_AGENT, open = CAP_SOVEREIGN), copyin/copyout throughout.
- NSI 77/78 and `AR_ACC_SEALED`/`AR_ACC_OPENED`/`AR_ACC_REJECTED` — committed,
  append-only.
- **NOT linked, NOT registered, NOT gated.** `smoke-acc` cannot exist until the
  syscalls do.

ACC is **not shipped** and must not be counted as such until DDR-827 lands and
`smoke-acc` is green.
