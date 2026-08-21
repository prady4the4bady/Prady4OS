# DDR-958 — `fat32_rename`

Status: ACCEPTED. Written before the code it governs (R16).
Number verified free in **both** `docs/ddr/` and `docs/decisions/` (CLAUDE.md §0.4).

## 1. Problem

`SYS_RENAME` (NSI 95), `vfs_rename` and `sfs_rename` all shipped in DDR-956, and
PRISM has an `mv` builtin. None of it works for a shell user: `fat32_ops`
declares no `.rename`, so `vfs_rename` returns `-ENOSYS` for every path PRISM can
name (DDR-956 §7). PRISM runs on the FAT default root.

DDR-957 explored the other unblock — rooting PRISM at SFS — and measured it to a
dead end: it needs a boot reordering **plus** migrating `/EXECTEST.ELF`,
`/HELLO.TXT` and `/BIG8K.TXT` onto the post-reformat volume, then re-verifying
~20 `smoke-shell` assertions (DDR-957 §10, §11). `fat32_rename` is the smaller
path: one function in one file, no fixture migration, no boot reordering.

## 2. Why the obvious implementation is wrong

DDR-956 §7 named the hazard and it is real. The tempting shape is "overwrite the
11 name bytes of the 8.3 directory entry in place" — a same-directory rename in
one sector write.

It corrupts any file that has a VFAT long name. A long-named file is stored as
*n* LFN fragment entries immediately followed by its 8.3 entry. Overwriting only
the 8.3 name leaves the fragments in place, and `dir_scan` (`fat32.c:133`)
prefers the accumulated long name over the 8.3 name when matching
(`hit = have_lfn && ci_eq(want, …)`). The file would answer to its OLD name and
not its new one — a rename that visibly did nothing, with a directory whose two
name records disagree.

Rewriting the LFN chain instead means generating fragments, the 0x0F ordinal
sequence and the short-name checksum in byte 13 of every fragment. This driver
has never written an LFN (`write_new_entry`, `fat32.c:450`, emits 8.3 only), so
that would be a new on-disk writer, not a rename.

## 3. Decision — copy the entry, then tombstone the original

Do not touch the source entry's name. Instead:

1. Resolve both parents with `resolve_parent`; scan the source entry.
2. If an existing destination is in the way, free its chain and tombstone it
   (exactly `fat32_unlink`'s tail) — the name is now unoccupied.
3. Allocate a free 32-byte slot in the destination directory and write a
   **verbatim copy of the source's 32-byte entry** into it, with only bytes 0–10
   replaced by the destination's 8.3 key.
4. Tombstone the source entry (`0xE5`), leaving its cluster chain alone.

The file's data never moves; only which directory record points at it changes.

### Why this is the correct shape here
- **No LFN writing and no LFN corruption.** The new record is a short-name entry
  with no fragments before it, which is exactly what `write_new_entry` already
  produces for every file this driver creates. The source's orphaned fragments
  are harmless: `dir_scan` clears `have_lfn` when it steps over the `0xE5`
  tombstone that follows them, so they cannot be mis-attributed to a later
  entry. `fat32_unlink` already leaves fragments in the same state — this
  introduces no new on-disk condition.
- **Attributes, timestamps, first cluster and size carry over for free**,
  because the copy is byte-for-byte. A field-by-field copy would silently drop
  whatever field the next person forgets; POSIX `rename` also must not restamp
  the modification time, and copying does that by construction.
- **Cross-directory rename works unchanged**, since nothing about the data is
  directory-relative.

## 4. Semantics — matched to `sfs_rename`, deliberately

A shell `mv` must not mean different things on different volumes, so each rule
below is the one `sfs_rename` (`sfs.c:1169`) already implements:

| case | behaviour |
|---|---|
| source absent, or either parent absent | `-1` (`sys_rename` maps to `-ENOENT`) |
| destination resolves to the SAME entry | `0`, no writes — POSIX no-op |
| destination exists as a regular file | replaced; its clusters are freed |
| destination exists as a directory | `-1`, never clobbered |
| **source is a directory** | `-1` — see §5 |

"Same entry" is decided by comparing the resolved `(ent_clus, ent_off)` of the
two lookups, not by comparing path strings. That is what makes `mv foo.txt
FOO.TXT` a no-op rather than a self-destructive rename: FAT short names are
case-folded by `comp_key`, so both paths name one directory record, and a
string comparison would not have noticed.

## 5. Directory rename is refused, not silently broken

`fat32_unlink` already restricts itself to regular files
(`if (di.attr & ATTR_DIRECTORY) return -1;`) and `fat32_rename` matches it.

Moving a directory to a different parent requires rewriting its `..` entry to
point at the new parent; leaving it stale makes `..` resolve to the old parent
and any subsequent `walk_dir` through it lands in the wrong place. Renaming a
directory *within* one parent would be safe, but shipping "rename works for
directories, except across directories, and returns the same `-1` either way"
is a worse contract than refusing the whole class. `sfs_rename` supports
directory rename; that asymmetry is stated here rather than papered over.

## 6. Atomicity — FAT has none, and the write order is chosen accordingly

`sfs_rename` puts both key changes in one journal transaction. FAT32 has no
journal, so this is three separate sector writes and a crash can land between
them. The order in §3 is chosen so each window degrades as gracefully as it can:

| crash window | resulting state |
|---|---|
| after step 2 | destination gone, source intact — `mv` failed, nothing else broke |
| after step 3 | the file is reachable under **both** names, one chain, two records |

The reverse order (tombstone the source first) has a window where the file is
reachable under **neither** name, i.e. data loss, so it is rejected. Note the
second window is not merely untidy: unlinking either name would free a chain the
other still points at. That is inherent to a journal-less FAT rename, is
documented in the code, and is why step 2 precedes step 3 — doing the
destination removal first also means step 3 can reuse the slot just freed, so
the common same-directory `mv` writes the new record into the destination's own
slot and no duplicate key is ever on disk, even transiently.

If step 3 fails (the disk is full and the directory cannot be extended), the
destination has already been removed and `-1` is returned. That is the one
outcome where a failed `mv` has destroyed something; it is recorded here rather
than avoided, because avoiding it requires either a journal or the transient
duplicate-key state this order exists to prevent.

## 7. Shared helpers — two extractions, no behaviour change

`fat32_create` and `fat32_rename` both need "find a free 32-byte slot in this
directory, extending it by one zeroed cluster if it is full", and `fat32_unlink`
and `fat32_rename` both need "stamp `0xE5` on this entry". Duplicating either
would be ~30 lines of cluster-walk in a second place.

- `dir_alloc_slot(c, dir, &slot_clus, &slot_off)` — the tail of `fat32_create`,
  lifted verbatim.
- `dirent_tombstone(c, ent_clus, ent_off)` — the last three lines of
  `fat32_unlink`, lifted verbatim.

Both callers re-read the sector after `dir_alloc_slot` returns rather than
holding a pointer across the call: `rd_data` hands out `c->scratch`, one shared
page per mount, and `alloc_cluster`/`zero_cluster` use it too.

## 8. Gates — `smoke-rename`, driven through PRISM's `mv`

### The probe ELF does not fit, and that is a measurement
The natural shape is `renametest.c` (already in the tree from DDR-956, never
registered) spawned behind `probe_enabled`, and it was built that way first —
Makefile rule, `user_image.asm` incbin, two spawns in `main.c`, one FAT-rooted
and one SFS-rooted.

It does not fit. Every embedded probe costs a page-aligned **8 KiB** inside
`kernel.bin`, and `kernel.bin` sits **3,714 B** under the 1 MiB stage-2 read
window (`Makefile:589`, DDR-733 as raised by DDR-827). With the probe embedded
the kernel came to **1,053,054 B — 4,478 B over the ceiling**, which is the same
wall DDR-956 hit ("gate BLOCKED on the 1 MiB boot window"). Raising the window
means changing the stage-2 chunk count and the image size together, against a
2 MiB PT_HI runtime ceiling: a boot-path change under a binding ADR, needing its
own DDR. It is not something to append here.

So the probe wiring was reverted and `user/renametest.c` deleted rather than
left in the tree as a file nothing can build (CLAUDE.md §3). It is recoverable
from git history if the boot window is ever raised.

### What the gate does instead
`smoke-rename` drives PRISM's `mv` builtin over the serial console, in exactly
the shape `smoke-shell` uses: a FIFO feeds commands once `PRISM_READY` appears,
and every assertion greps the captured `build/rename_serial.log`.

This costs zero image bytes and is a **better** test than the probe would have
been: DDR-956 shipped `mv` non-functional for every shell user, and this gate
asserts on the thing that was broken rather than on a synthetic caller.

| arm | asserts | what it catches |
|---|---|---|
| 1 | `mv: /RENSRC.TXT -> /RENDST.TXT` | `-ENOSYS` — the DDR-956 state |
| 2 | the payload reads back from the destination | an entry created without the source's chain/size |
| 3 | `cat: cannot open /RENSRC.TXT` | a copy-style rename that leaves the source |
| 4 | `mv: cannot rename /NOSUCH9z.TXT` | **stub-catcher**: a handler returning 0 passes 1–3 and cannot pass this |
| 5 | an existing destination is replaced, old bytes gone | §4 overwrite semantics, and the `free_chain` in it |
| 6 | the OLD **long** name stops resolving | **the §2 corruption** — nothing else in the tree catches it |
| 7 | no `[BUG]`/`PANIC`, no user trap after `PRISM_READY` | a bad slot offset faults instead of printing a wrong string |

Arm 6 is why this DDR exists. `/LongFileName.txt` is the FAT image's only VFAT
long-named file (`Makefile:638`); its 8.3 alias is `LONGFI~1.TXT`, which is not
what `comp_key` derives from that path, so after the rename the only way the old
name can still open is through stale LFN fragments. Arms 1–5 all pass under the
§2 in-place implementation because they use short names only.

Arm 7 is scoped, not blanket: `WXVIOL.ELF` and `METRIC.ELF` fault **on purpose**
every boot (W^X and read-only-page regressions), so a bare `#PF` grep fails on
those and says nothing about rename.

Arms 5 and 6 are positional — a marker is echoed after the `mv` and only the
lines below it may satisfy the assertion — because a whole-log grep cannot tell
"the file holds this" from "some earlier command printed this".

### What is NOT gated
`sfs_rename` stays where DDR-956 left it: implemented, callable, **ungated**.
PRISM runs on the FAT default mount, so a shell-driven gate cannot reach the SFS
root, and reaching it needs the probe ELF, i.e. the boot window above. This is
stated rather than papered over: the `.rename` op is proven on FAT only.

## 9. Verification — measured, not planned

All on this host (QEMU 8.2.2, TCG, no KVM), kernel `6a254f13b9fd2b9fc9e8f2597eca9767`,
`kernel.bin` 1,044,862 B (3,714 B under the 1 MiB window):

| check | result |
|---|---|
| `make image` | rc=0, 0 warnings at `-Werror` (clang + nasm) |
| `make smoke-rename` | **PASS** — all 7 arms |
| `make smoke-shell` | PASS — full assertion line |
| `make smoke-fs` | PASS — 14 FS patterns |
| `tools/ci/fs_regression.sh` | **9/9 PASS**, uaf=0 on every gate |
| `make ci-shard-check` | OK — 146 gates / 6 shards / 6 excluded |
| `make ci-probe-rodata-check` | OK — 56 ELFs |
| `make ci-start-align-check` | OK — 39 entry points |

`fs_regression.sh` is the regression check the §7 extractions require: it covers
every FS gate, and `fat32_create` / `fat32_unlink` are on the path of all of
them.
