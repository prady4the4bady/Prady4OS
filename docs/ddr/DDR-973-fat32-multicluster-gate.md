# DDR-973 — FAT32 multi-cluster reads: the ADR-024 defect does not reproduce; gate it

Status: ACCEPTED. Number verified free in **both** `docs/ddr/` and
`docs/decisions/` (§INV.4): `ls docs/ddr/ docs/decisions/ | grep DDR-973` → empty.

This DDR reports a **refutation plus a gate**, not a bug fix. The backlog item it
closes asked for a fix to a named function; that function does not exist, and the
behaviour it was blamed for could not be reproduced on the current kernel. What
ships here is the measurement that establishes that, made permanent.

---

## 1. The claim, and where it came from

`CLAUDE.md` §OPEN ISSUES and §GROUP B both carry:

> **FAT32 large-file** — `execve` of large musl ELF corrupts — multi-cluster
> `read_cluster_chain` bug (ADR-024)

and Group B names the repair target as "root-cause `read_cluster_chain` for
files spanning more than one cluster".

The origin is `ADR-024 §D5`, written during the 5e PRISM bring-up:

> **Found during bring-up:** `execve` of a *large musl-C* ELF read from FAT32
> corrupts the loaded image (the program jumps mid-instruction with a zeroed
> frame). `execve` had only ever been exercised with the tiny *asm* EXECTEST;
> the SFS `elf_load` path, by contrast, loads 30 KB+ musl-C images fine (cmusl,
> init). **Root cause is most likely FAT32 multi-cluster reads for large files**
> — a separate kernel bug, out of 5e scope.

Two things about that paragraph decide how this item had to be worked:

1. **"most likely" is the whole attribution.** No capture was taken of the FAT32
   read path, no byte was compared, no cluster chain was dumped. The observed
   fact was "the execve'd image misbehaves"; "multi-cluster read" was the
   hypothesis. `build_status.md:311` repeats it with the same hedge ("likely
   FAT32 multi-cluster read").
2. **There is no `read_cluster_chain`.** `grep -rn read_cluster_chain --include=*.c
   --include=*.h .` returns nothing. The FAT32 reader is `fat32_read`
   (`kernel/fs/fat32/fat32.c:309`), which walks the chain inline via `fat_next`.
   The backlog named a repair target that has never existed in this repo — which
   is itself evidence that nobody re-derived the claim after ADR-024 wrote it.

Per §NON-NEGOTIABLE 3 ("no fix without a named mechanism from a real failing
artefact") the only admissible first step was to reproduce.

---

## 2. What the code actually does

`fat32_read` (`fat32.c:309`) walks the chain from the file's first cluster on
every call, copying whole sectors and selecting the requested window by absolute
file offset:

```c
uint64_t pos = 0;                       /* file offset of the current sector */
uint32_t clus = f->cookie;
while (valid_chain(clus) && copied < len) {
    for (uint32_t s = 0; s < c->spc && copied < len; s++) {
        uint8_t *sec = rd_data(c, clus_first_sector(c, clus) + s);
        for (int i = 0; i < 512 && copied < len; i++) {
            uint64_t fo = pos + i;
            if (fo >= off && fo < f->size)
                out[copied++] = sec[i];
        }
        pos += 512;
    }
    clus = fat_next(c, clus);
}
```

The two buffers involved are distinct (`rd_data` → `c->scratch`, `rd_fat` →
`c->fatbuf`), so the chain step cannot clobber the sector being copied — the
single most plausible mechanism for the reported corruption, and it is absent.

**The volume geometry makes "multi-cluster" a low bar here.** `make fat-image`
runs `mkfs.fat -F 32` over 64 MiB, which lands on **1 sector per cluster**:

```text
bytes/sector 512  sectors/cluster 1  cluster bytes 512
reserved 32  numfats 2  fatsz 1009  rootclus 2  totsec 131072
```

So *every* fixture on that volume above 512 bytes is already multi-cluster, and
two of them are read by gates that have been green for months:

| fixture | size | clusters | already read by |
|---|---|---|---|
| `/BIG8K.TXT` | 7,824 B | **16** | `smoke-shell` — `cat /BIG8K.TXT \| cat`, asserting both `BIGHEAD-e5v` and `BIGTAIL-e5v` |
| `/EXECTEST.ELF` | 4,368 B | **9** | `smoke-sysexec`, and `smoke-shell`'s job-control arm, through the full `sys_execve` path |

A 16-cluster byte-exact read through a pipe and a 9-cluster `execve` were
therefore already passing before this DDR was written. Whatever ADR-024 saw, a
chain walk that breaks on the second cluster is ruled out by the existing suite.

---

## 3. The reproduction attempt (§NON-NEGOTIABLE 3)

The ADR-024 case is specifically *a large musl-C ELF*, so the experiment used the
same binary ADR-024 names — `cmusl` — placed on the FAT root and launched
through PRISM's `run` (fork + `execve`), which is the exact deferred path:

```text
mcopy -i build/fat.img build/cmusl.elf ::/CMUSL.ELF     # 30,488 B = 60 clusters
… feeder, after PRISM_READY:
  run /CMUSL.ELF
  echo cmusl-status=$?
```

Kernel under measurement (R1): `build/kernel.bin`, 1,053,054 B,
`sha256 9763ce7bb259de7e0dc991de3f6832a748fad608718a042c32a0654562dde35d`.
Stray-QEMU pre-flight per §INV.3: `pgrep -f "[q]emu-system-x86_64"` → no match.

Result, from `build/repro_serial.log` (425 lines):

```text
PRADYOS_MUSL_OK v1.2.5 2026          <- line 382, the FAT32 execve'd copy (pid 82)
[user] sys_exit(0) pid=82 writes=1 — thread terminating
cmusl-status=0
```

`PRADYOS_MUSL_OK` appears exactly twice in the capture: line 289 is the
boot-time SFS-loaded copy (`main.c:2017`), line 382 is this one. A 30,488-byte
musl-C ELF spanning **60 FAT32 clusters** was read by `sys_execve` in one
`vfs_read`, loaded, and ran to a clean exit 0.

**The defect does not reproduce.** The control in the same run is
`/EXECTEST.ELF` at `exectest-status=0`, so the harness was not simply failing to
report a failure.

---

## 4. Decision

Do **not** write a speculative fix. There is no failing artefact, and §6.0-B and
§NON-NEGOTIABLE 3 both forbid one. Editing `fat32_read` on the strength of a
2-line hedge in an ADR would be the exact error this project has retracted twice
(DDR-969's "uninitialised PID" that was a deliberate sentinel; DDR-966's
`sched_create`-NULL attribution refuted by `spawned=2/2`).

Instead, **make the refutation permanent.** A one-off reproduction attempt that
happens to pass proves nothing about tomorrow's kernel; a gate does. If the
attribution was right and the corruption is a rare timing- or layout-dependent
event rather than the deterministic bug ADR-024 described, a permanent
byte-exact gate is also the only thing that will ever catch it.

---

## 5. The gate — `smoke-fat32-multicluster`

New probe `user/fat32mctest.c`, freestanding (raw syscalls, no libc, no writable
globals per DDR-826), rooted at the **FAT** mount — not SFS — because that is
the volume under test. Opt-in behind `probe_enabled("fat32mc")` (DDR-804) for
the reason the ftruncate probe is: arm C prints a second `PRADYOS_MUSL_OK`, and
an unconditional spawn would put it in every other gate's log.

New fixture `/BIGPAT.BIN`, 65,536 B = **128 clusters**, generated by the
`fat-image` recipe so every gate run starts from a fresh volume. Byte *n* is

```text
(7n + 3 + 31*(n >> 8)) & 0xFF
```

CLAUDE.md's Group B row specifies "pattern 7n+3", and that is the first term.
The `31*(n>>8)` term is not embellishment — without it the gate is vacuous, for
the reason §6 measures.

| arm | what it reads | what a chain-walk defect would do |
|---|---|---|
| **A** — sequential scan | all 65,536 bytes in 4 KiB reads (8 clusters each) | a repeated, skipped or transposed cluster shifts the pattern; the first wrong byte's **absolute offset** is printed, which localises the bad cluster |
| **B** — boundary straddles | 6 short reads at offsets 511, 510, 1023, 4095, 32767, 65530, each reached by `lseek` | each re-walks the chain from the head (that is what `fat32_read` does on every call), so a defect that only appears deep in a chain is exercised 6 more times; the last read runs off EOF and must come back short by exactly 10 |
| **C** — the ADR-024 case | `execve("/CMUSL.ELF")` — 30,488 B, 60 clusters, read by `sys_execve` in **one** `vfs_read` | the reported symptom exactly: a corrupted image jumps mid-instruction instead of printing its marker |

Arm A's read buffer is a **4 KiB stack local**, deliberately. ADR-038 makes the
user stack demand-paged with `USER_STACK_EAGER_PAGES = 8`, and
`vmm_user_range_ok` validates syscall pointers **without faulting them in** — so
a buffer larger than the eager window would be rejected by the very syscall
under test, and the gate would fail for a reason that has nothing to do with
FAT32. Chunking at 4 KiB keeps every read inside the eager window while still
walking up to 128 clusters per call.

**Sentinels.** Pass: `PRADYOS_FAT32_MC_OK bytes=65536 clusters=128 straddles=6`.
Forbidden: `FAT32MC FAIL`. Arm C is asserted separately, by **count**:
`PRADYOS_MUSL_OK` must appear **twice** — the boot's SFS-loaded copy is the
denominator (R17), and one occurrence means arm C's execve did not happen.

---

## 6. Mutation results — and the vacuous first cut this caught

A gate that cannot fail proves nothing (DDR-958 §7 ran this same check). Two
mutants were applied to `fat32_read`, each built and run against the gate, then
reverted and the gate re-run green (§NON-NEGOTIABLE 16).

### M1 — chain repeat: **the first cut of this gate PASSED it**

```c
{ uint32_t nx = fat_next(c, clus); if (pos != 512 * 64) clus = nx; }
```

Cluster 63 is read a second time instead of advancing to 64 — a textbook
multi-cluster defect, and precisely the class of bug this gate exists to catch.
The gate reported PASS: `65536 B / 128 clusters verified, 6 straddles`.

**Why.** The pattern CLAUDE.md's Group B row specifies, `(7n+3) & 0xFF`, has
**period 256**. This volume's clusters are 512 bytes — two whole periods — so
under that pattern *every cluster on the disk holds byte-identical content*.
Reading cluster 63 twice returns exactly the bytes cluster 64 would have
returned. The oracle was blind to the only thing it was built to see, and no
amount of running it would have revealed that: it passes on a correct kernel and
it passes on a broken one.

**Fix.** The pattern is now

```text
byte n = (7n + 3 + 31*(n >> 8)) & 0xFF
```

31 is invertible mod 256, so `31*k` stamps each of the 64 KiB file's 256
blocks distinctly and no two 256-byte windows in the file are equal. The stamp
is per 256-byte block, not per cluster, so the oracle does not depend on the
volume's sectors-per-cluster — a `mkfs.fat` that chose a different cluster size
would not silently re-blind it.

Re-run against M1 with the corrected pattern:

```text
FAT32MC FAIL: pattern mismatch off=32768 got=69 want=131
```

Caught, and localised to the exact byte where the mutated chain diverges.

### M2 — large-read cap, to prove arm C carries weight

```c
if (len > 16384u) len = 16384u;   /* in fat32_read */
```

Arms A and B are unaffected — every read they issue is ≤ 4096 bytes. Only
`sys_execve`'s single 30,488-byte read is truncated:

```text
PRADYOS_FAT32_MC_OK bytes=65536 clusters=128 straddles=6
FAT32MC FAIL: execve /CMUSL.ELF returned off=0
```

and `PRADYOS_MUSL_OK` appeared **once**, so the §5 count assertion fires too.
Arm C is not redundant with arms A and B: it is the only one that exercises a
read larger than 4 KiB, which is the shape ADR-024 actually reported.

### Post-revert

`kernel/fs/fat32/fat32.c` restored (`git diff` empty), rebuilt, gate re-run:
PASS, `2/2 MUSL_OK`. Kernel under the final measurement (R1):
1,061,246 B, `sha256 ab00c00c05fb6fb5e369c0841960f8ef6aa4b16054af3e55527824169f004ea9`.

---

## 7. What this changes in the backlog

- `CLAUDE.md` §OPEN ISSUES **FAT32 large-file** row: the mechanism named there
  (`read_cluster_chain`) does not exist and the symptom does not reproduce.
  Re-stated as gated-and-not-reproduced, citing this DDR.
- `CLAUDE.md` §GROUP D **PRISM `run` re-enable**: `run` is **not disabled**.
  `user/prism.c:572` dispatches it, `do_run`/`do_run_bg` fork+execve, and
  `smoke-shell` already exercises `run /EXECTEST.ELF` twice plus `fg`/`jobs`.
  What ADR-024 actually deferred is narrower: **init-driven `fork`+`execve`
  respawn of PRISM**, which the kernel sidesteps by launching PRISM via
  `elf_load` and setting `parent_pid` to init. That row is corrected, not closed
  — the respawn path is still not built.
- `ADR-024 §D5` gains an addendum recording the refutation, so the next session
  reading the ADR does not re-derive the same hypothesis.

---

## 8. What would reopen this

A `FAT32MC FAIL` line, or `PRADYOS_MUSL_OK` appearing once in
`smoke-fat32-multicluster`. Either is a real failing artefact against a named
predicate, which is what a fix to `fat32_read` has needed since ADR-024 and has
never had.
