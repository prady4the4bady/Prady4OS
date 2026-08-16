= DDR-938 — §6.2-D1 must gate TWO `sfs_format` sites, not one

**Status:** ACCEPTED. Corrects the scope of **DDR-931** (which is otherwise
sound and is NOT superseded — its 12-gate enumeration is verified correct).
**Date:** 2026-08-16
**Blocks:** §6.2-D1 (provisioned SFS as default boot root). Read before writing
any D1 code.

## What DDR-931 gates

DDR-931 proposes putting the destructive SFS self-test behind
`probe_enabled("sfsselftest")`, naming one call site — `kernel/main.c:1128`
(now `:1141` after intervening edits):

```c
if (sbd && sfs_format(sbd) == 0) {          /* :1141 — destructive self-test */
```

## The site it misses

There is a **second** unconditional format, from DDR-760:

```c
/* DDR-760: persistent SFS root (SFS-as-root half 2/2). The destructive tests
 * above left blk2 dirty + unmounted; reformat it CLEAN, remount, provision
 * /etc/aether/config, and root a probe there … */
if (sfs_format(sbd) == 0) {                 /* :1965 — NOT gated by DDR-931 */
```

`:1965` exists because `:1141`'s tests deliberately leave the disk dirty. Its
comment states that dependency outright. So the two are coupled: gating `:1141`
off removes the mess that `:1965` was written to clean up, while leaving
`:1965` free to wipe the disk anyway.

## Why the DDR-770 check does not save it

`:1965` is followed by DDR-770 logic keyed on `prov_mnt`:

> only KERNEL-provision the config when no host `mkfs.sfs` image already
> carries one (`prov_mnt < 0`)

That check decides whether to **write the config**. It does not decide whether
to **format**, and it runs *after* the format has already executed. `prov_mnt`
is computed earlier (`:1124-1131`) from the pre-format disk, so by the time it
is consulted, the evidence it was derived from has been destroyed.

Net effect: **a host-provisioned `mkfs.sfs` image is wiped by `:1965` even with
`:1141` fully gated off.** §6.2-D1's entire premise — that the provisioned
image survives boot and becomes the root — fails on that line alone.

## Decision

§6.2-D1 must treat the two sites as one change:

1. **`:1141`** — gate behind `probe_enabled("sfsselftest")` exactly as DDR-931
   specifies. Unchanged.
2. **`:1965`** — must become conditional on there being nothing worth keeping:
   format only when `prov_mnt < 0` (no valid host-provisioned root was found
   pre-format). When `prov_mnt >= 0`, skip the format and mount the existing
   volume — that *is* "provisioned SFS as default boot root".
3. The `prov_mnt` probe at `:1124-1131` becomes load-bearing rather than
   advisory, so its failure modes matter now. It currently accepts any file
   whose first five bytes are `mode=`. That was adequate when a false positive
   only skipped config provisioning; once it also decides whether to preserve
   the disk, a false **negative** silently wipes a good root. Decide explicitly
   whether that check needs strengthening before relying on it.

## Verification bar (in addition to DDR-931's)

DDR-931's bar stands: the 12 enumerated gates re-run individually, the new
`smoke-sfs-boot-root` gate 3x, `ci-shard-check`, warning-clean build.

Added by this DDR: **a boot with a host-provisioned image must leave that
image's contents intact** — assert on a file written by `mkfs.sfs` that the
kernel never creates, so "the disk survived" cannot be satisfied by the kernel
having re-provisioned an identical-looking one. Without that, a gate asserting
`PRADYOS_SFSROOT_OK` passes whether the host image survived or was silently
reformatted and rebuilt, which is the failure this DDR exists to prevent.

## Note on how this was found

Found while checking a claim I had made earlier in the session — that D1's
blast radius was 16 gates rather than DDR-931's 12. **That claim was wrong**
(the extra four came from `awk` matching sentinel names inside comments and
across recipe boundaries), and DDR-931's 12 is correct. The second format site
turned up only because the retraction required reading the actual call sites
instead of grep output. Recorded because the useful finding came from checking
my own error, not from the original claim.
