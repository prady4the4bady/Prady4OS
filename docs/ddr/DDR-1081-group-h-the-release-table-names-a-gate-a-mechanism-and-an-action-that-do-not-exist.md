# DDR-1081 — GROUP H AUDITED: THE RELEASE TABLE NAMES A GATE THAT DOES NOT
# EXIST, A MECHANISM THAT WAS SUPERSEDED BY OWNER APPROVAL, AND AN ACTION THE
# PROJECT CANNOT EXECUTE

Status: RECORDED. Docs-only. **No code change, no gate change, no kernel
change.** `kernel.bin` untouched; 178 gates unchanged; `GLOBAL_FORBIDDEN` 76
unchanged; no open issue moves (OPEN-1/2/12/13 untouched).

Group H is the last of the eight backlog tables to be audited — E (DDR-1071),
F (DDR-1072), A and B (DDR-1073), G (DDR-1075), and now H. It is also the one
that decides what "done" means, because every row in it is a release
precondition and the last row is the `v1.0.0` tag.

Measured against the Makefile, `tools/ci/gate_shards.txt`, `ci.yml`,
`kernel/syscall/syscall.h` and the arch sources — **not** inferred from any DDR.

---

## 1. THE FINDINGS, IN THE ORDER THEY WOULD COST A SESSION TIME

### 1.1 — Row 1 names a gate that does not exist, and omits the one that carries the claim

The row reads:

| ISO x86_64 | multiboot2 + grub-mkrescue | `smoke-iso-x86_64` |

`grep -cE '^smoke-iso-x86_64:' Makefile` returns **0**. The real target is
**`smoke-iso-x86`** (Makefile:1137), and it asserts BOTH boot paths off ONE
ISO — BIOS via El Torito hard-disk emulation, then UEFI via OVMF pflash,
requiring `[uefi] handoff` and `NEXUS KERNEL OK` on each.

This is not the DDR-1063 §7c class. §7c established that a planning table
naming a gate for work **not yet done** is doing its job. Here the work IS
done, the gate EXISTS under a different name, and it is **CI-registered at
strict tier** — `gate_shards.txt:62` puts `smoke-iso-x86` on shard 1, 240 s,
strict. It is the DDR-1040 `smoke-wx` shape: a wrong name for shipped,
gated work.

**And the row names the weaker of the two ISO gates.** `smoke-iso-userspace`
(DDR-972, `gate_shards.txt:44`, shard 0, 300 s, **strict**) does not appear in
the table at all — and it is the gate that exists precisely because
`NEXUS KERNEL OK` is not enough. Its own Makefile header says so:

> `smoke-iso-x86` asserts NEXUS KERNEL OK, which prints at line 30 of 145 —
> about 60 lines before userspace starts. DDR-971 measured an ISO that passed
> that gate and then idled forever at rqdepth=1 curpid=0 with no PRISM, no
> aetherd and no filesystem.

`smoke-uefi` (`gate_shards.txt:52`, shard 0, strict) is a third. So the ISO is
verified three ways on **every CI suite**, and the release table records one of
them, by a name that does not resolve.

### 1.2 — The stated mechanism is a design this project SUPERSEDED with owner approval

"multiboot2 + grub-mkrescue" is not what ships and has not been for some time.
Makefile:1105, in the ISO section's own header:

> Multiboot2 is SUPERSEDED (owner-approved, DDR-896). The ISO carries the two
> loaders this project already proved independently rather than adding a third
> handoff contract that would hand control in 32-bit protected mode.

Measured across the tree: `grep -rniE 'grub-mkrescue|multiboot'` over `*.c`,
`*.h`, `*.asm`, `Makefile`, `*.yml`, `*.sh` returns **exactly one line** — that
comment. There is no GRUB and no multiboot2 anywhere. The `iso` target is
`xorriso -as mkisofs` with `-b boot/pradyos.img -hard-disk-boot` plus an
`-eltorito-alt-boot` ESP entry: this project's own stage1/stage2 and its own
UEFI loader.

**This is worse than a stale gate name.** A stale name costs a `grep`. A stale
*mechanism* on a release row instructs the next session to build the third
handoff contract DDR-896 deliberately refused, in 32-bit protected mode, days
from a tag. And the Makefile records that the obvious alternatives were tried
and MEASURED to fail — floppy emulation does not implement INT 13h AH=42h,
which stage1 issues; no-emulation hands 2048-byte CD sectors and puts every LBA
four times too deep.

### 1.3 — CI installs two packages for that superseded design (RECORDED, NOT ACTED ON)

`.github/workflows/ci.yml:196` installs `grub-pc-bin grub-efi-amd64-bin`.

Measured on this host, which is the host that built and verified the
release candidate (§CURRENT BUILD STATE: *"RELEASE CANDIDATE VERIFIED on
`ace232f` — `smoke-iso-x86` (BIOS **and** UEFI arms), `smoke-iso-userspace`,
`smoke-uefi`"*):

    dpkg -l | grep -cE '^ii +grub-(pc-bin|efi-amd64-bin) '   ->  0
    command -v grub-mkrescue                                  ->  not present

So the two packages are unnecessary **by construction, not by argument**: the
machine that produces the verified ISO on both arms does not have them.

This is **not a correctness risk** and is not presented as one. DDR-1045's rule
still holds — they are Ubuntu-archive packages, and `apt_prepare.sh` resolves
exactly what the caller names, so their presence cannot break an install. What
they are is a package list that is a claim about what the build needs, with two
entries for a design that was superseded.

**Not changed here.** Editing `ci.yml` alters CI behaviour on four jobs, and the
operator's standing instruction on surfaced gaps is to report rather than act.
Recorded for that decision.

### 1.4 — Rows 2 and 3 say "packaging only", which is literally true and materially misleading

| ISO aarch64 | EFI/U-Boot packaging (kernel already boots in CI — packaging only) |
| ISO riscv64 | OpenSBI + U-Boot packaging (kernel already boots in CI — packaging only) |

The kernels do boot in CI: `arch-bootstrap` (ci.yml:284) builds both under
clang cross-compilation and runs `make smoke-aarch64` / `make smoke-riscv64`,
which is `boot_arch.sh` grepping for `NEXUS KERNEL OK`. That part is true, and
those two gates are correctly in `shard_check.sh`'s EXCLUDE set because they
run in their own job rather than the shard matrix.

**What boots is 278 lines.** Measured:

    kernel/arch/aarch64/{boot.S,kernel.ld,start.c}   48 + 38 + 67 = 153
    kernel/arch/riscv64/{boot.S,kernel.ld,start.c}   35 + 36 + 54 = 125

`arch_main` on aarch64 brings up the PL011, prints the sentinel and the
exception level, and enters `for(;;) wfe`. riscv64 is the same shape ending in
`wfi`. No MMU (`start.c`'s own comment: *"No MMU, no caches: the reset
attributes are used as-is"*), no PMM, no VMM, no scheduler, no VFS, no
userspace. ADR-034 says so deliberately: *"Scope is BOOT ONLY — the smoke-gate
set is not ported yet."*

So **there is no OS to package.** "Packaging only" is true of the ISO
mechanics and false about the deliverable: satisfying the §WHAT "DONE" MEANS
boxes *"aarch64 ISO built and bootable"* and *"riscv64 ISO built and bootable"*
would certify an ISO whose payload prints four lines and halts.

**And this file already states the scope correctly, elsewhere.**
§PRE-APPROVED EXCEPTIONS carries *"`arch/aarch64` full port — boot-only scope
per ADR-034 — ISO uses boot-only kernel"* and the same for riscv64. So the same
document carries both readings, and the **work** copy is the misleading one —
the DDR-1072 §3 shape (an item present twice, once as work and once as a logged
deferral, with the work copy not saying so), now on the release table.

### 1.5 — Those exception entries name a path that holds nothing

They say `arch/aarch64` and `arch/riscv64`. Measured: `git ls-files arch/`
returns 13 files, of which those two directories contribute
**`.gitkeep` and nothing else** — every other entry is `arch/x86_64/*.asm`.
The boot-only code lives at `kernel/arch/<arch>/`.

Minor, and recorded rather than fixed here for the same reason as §1.3: a
reader checking "is the port there?" at the named path finds an empty directory
and can reasonably read that as "nothing exists", when 278 lines do.

### 1.6 — Row 6 prescribes an action the project cannot execute

| 3× consecutive CI greens on `main` tip | Before tagging | `gh run rerun` |

§INV.15 of the **same file**, corrected 2026-08-23, says the opposite in as
many words:

> **The third green comes from `workflow_dispatch`, not `gh run rerun`.**
> … *"`gh run rerun` needs admin rights the project PAT does not have, and any
> other way to start a run is a push, which changes the SHA."* … This line
> previously mandated `gh run rerun`, which the project cannot execute.

The invariant was corrected; **two other sites were not** — CLAUDE.md:733 (this
row) and CLAUDE.md:524 (PHASE 1 ITEM 3, *"3 consecutive CI greens on the SAME
tip SHA (`gh run rerun` for the third)"*).

This is DDR-1073 §5's shape exactly — a prescribed remedy that was never
runnable, contradicted elsewhere in the same document — and it sits on the last
step before the tag. A session following the release table would spend its
third green on a command that returns a permissions error, and §INV.15 also
records the reason the substitute is *better* evidence, not merely available:
independent dispatched runs beat re-attempts of one run.

### 1.7 — §INV.12 names the wrong syscall for NSI 87

Row 4 and §INV.12 both say *"87 is `SYS_READ_AUDIT` — do NOT reuse"*.

Measured in `kernel/syscall/syscall.h`:

    #define SYS_VAULT_PUT     87
    #define SYS_READ_AUDIT    37

**The conclusion holds and the reason does not.** 87 IS occupied, so prad must
not take it — but by `SYS_VAULT_PUT`, and `SYS_READ_AUDIT` is 37. This matters
because §INV.12 is in the section a session is told to trust *without
re-deriving*: a session that checked the stated reason would find a different
syscall at 87 and have to decide which half of the file to believe. That is the
position §INV.14's own correction describes — *"Verify against `syscall.h`, not
against this line."*

A stronger fact the row misses, measured by enumerating every `#define SYS_*`:

    free below 110: 0, 88, 89, 90, 103, 104, 105, 106, 107, 108, 109
    max defined:    102

**88/89/90 are the only free numbers below 103** — exactly three, exactly what
prad needs, and the next allocation after them starts at 103, which is what
§CURRENT BUILD STATE already says. So the *allocation* in row 4 is right and
better-founded than its parenthetical.

`smoke-prad` does not exist and prad is genuinely unbuilt. That IS the
DDR-1063 §7c class doing its job, and is not counted as a defect here.

---

## 2. WHAT IS ACCURATE, STATED BECAUSE AN AUDIT THAT ONLY REPORTS ERRORS IS NOT AN AUDIT

**Row 5 is correct in every particular.** `smoke-invariants` exists
(Makefile:2918) and requires exactly
`PRADYOS_INVARIANTS_OK S1,S2,S4,S5,S6,S8` with `INVARIANT FAIL` forbidden — so
S3 and S7 are genuinely absent, as the row says, and the six named as passing
are the six the sentinel names. The gate's header states the property that
makes it different in kind from the other 177: *"Every OTHER gate asserts a
feature works; this one asserts attacks FAIL. A refactor that deletes a
capability check breaks no feature gate — only the refusals stop happening, and
nothing else notices."*

There is also a half of S5 that **cannot** be a runtime arm and is covered
anyway: `ci-audit-noerase-check` asserts at build time that no
erase/clear/reset/purge syscall is registered against the audit log, because
*"the ABSENCE of a syscall is not something a syscall can test. … A runtime arm
would have to invent the hole it tests for."*

**Row 7 is correct and is the operator's.** `v1.0.0` is deliberately untagged
and the `main` promotion is unstarted; nothing here changes that and no
promotion is in flight.

---

## 3. WHAT THIS CHANGES

Group H reads, after measurement, as:

* **x86_64 ISO: SHIPPED, and gated three ways at strict tier on every CI
  suite** (`smoke-iso-x86` shard 1, `smoke-iso-userspace` shard 0,
  `smoke-uefi` shard 0) — not pending work. The release candidate was verified
  on `ace232f` with all three.
* **aarch64 / riscv64: a 278-line boot-to-console slice, by design (ADR-034),
  correctly recorded as a pre-approved exception and incorrectly implied by
  the work rows to be a packaging step away from an OS ISO.**
* **prad: genuinely unbuilt**, and 88/89/90 are measured to be the only three
  free NSI numbers below 103.
* **S1–S8: six pass, S3/S7 depend on Group F**, exactly as the row says.
* **The three greens: obtainable, but not by the command the row names.**

The gate COVERAGE did not change. Only the RECORD of it — which is the same
sentence DDR-1071 §4 ended on, and the fifth table in a row where it applies.

---

## 4. THE STRUCTURAL POINT, NOW WITH EIGHT TABLES BEHIND IT

DDR-1071 §4 proposed that stale rows are what this update discipline
**produces by default**, because correction is a side effect of adjacent work
rather than a process — so the row whose work finished cleanly and drew no
follow-up is exactly the row that stays stale.

Group H is the strongest case for that reading, because **nobody has worked
adjacent to it.** The release is held: `v1.0.0` untagged by operator decision,
`main` promotion unstarted. So no session has had reason to touch these rows
since they were written, and they are the most stale of the eight — a gate
name that never resolved, a mechanism superseded by owner approval, an action
corrected in the same file and not here, and a syscall attribution that is
simply wrong.

DDR-1071 §5 and DDR-1072 §2 between them refused a mechanical checker, and
that refusal still stands: the false-negative case (`smoke-horizon`, a gate
that exists while the row is legitimately half done) and the false-positive
case (`smoke-sendipc`/`smoke-runexp`, a green strict gate whose name matches a
row it does not cover) are both semantic, and nothing in the tree can read a
semantic claim. **Group H supplies a third case they did not have and it is
the worst of the three**: row 1's named gate does not exist at all, so a
name-existence rule would flag it correctly — but row 2's named gate also does
not exist and flagging that would be wrong, because rows 2 and 3 are §7c doing
its job. The *same* signal, "named gate is missing", is a defect on row 1 and
correct behaviour on rows 2, 3 and 4. Nothing mechanical separates them.

`ci-docstate-check` (DDR-1063) remains the shape that does work here: it
asserts an **arithmetic identity** between two numbers in a document, which is
checkable. Everything in this DDR is a semantic claim, which is not.

---

## 5. FOUND WHILE UPDATING THE RECORD: DDR-1063'S STATED LIMITATION, OBSERVED FOR THE FIRST TIME

`docs/PRE_LAUNCH_CHECKLIST.md` §6 is the RELEASE-GATE STATE table, and its own
header says *"Re-measure rather than increment"* and *"A derived quantity is
stale the moment its input changes."* Measured: three of its rows were stale —
gates `177` (now 178, DDR-1078), DDR free range `DDR-1063+` (now DDR-1082+), and

    kernel.bin  1,282,442 B  —  290,422 B headroom

which is the DDR-1065 kernel, **two kernels out of date**. The current pair is
`1,290,634 / 282,230`.

**And `ci-docstate-check` reported OK on it**, because
`1,282,442 + 290,422 = 1,572,864` exactly. That is not a defect in the check —
it is DDR-1063's own §NOT CLAIMED, verbatim: *"a stale but self-consistent pair
still passes."* This is the **first observed occurrence** of that limitation,
and it occurred in the file that records the limitation.

The refusal DDR-1063 made still looks right and is worth restating with this
instance behind it: the check asserts an **arithmetic identity**, which is
checkable, and deliberately does not assert **currency**, because a currency
check would redden on correct in-progress work and get removed. What this
occurrence establishes is narrower and useful — **passing `ci-docstate-check` is
not evidence that a live-state number is current**, and three sessions'
worth of DDRs had been landing beside a stale one.

**A refinement, found by looking at the third pairing rather than assuming.**
The check also reports `docs/PRE_LAUNCH_CHECKLIST.md:699` as
`1,175,946 / 396,918` — the pre-post-quantum figures, i.e. literally the wrong
pair DDR-1063 was written about. **That one is correct as written**: its
surrounding text says the numbers are the PRE-work ones, left deliberately, and
the paragraph is a dated statement about what was true when ML-DSA was being
scoped. So the checker cannot distinguish a deliberately historical pair from a
live-state pair, and here only a human annotation does. Recorded because the
obvious next move — "make them all current" — would have destroyed a correct
historical record. Only §6 was corrected.

Nothing is built for this. A rule of the form "a pairing outside a
history-marked block must equal the measured size" needs the tree to read what
"history-marked" means, which is the same semantic boundary §4 just described.

---

## 6. NOT CLAIMED

* **No code change.** `kernel.bin` untouched, so the CLAUDE.md size/headroom
  pair is unaffected and `ci-docstate-check` is unaffected. 178 gates
  unchanged; no new gate; `GLOBAL_FORBIDDEN` 76 unchanged.
* **No gate was run for this DDR**, locally or in CI. What was measured is
  target existence in the Makefile, shard registration and non-exclusion in
  `gate_shards.txt`/`shard_check.sh`, the recipes read in full for
  `smoke-iso-x86`, `smoke-iso-userspace`, `smoke-aarch64`, `smoke-riscv64` and
  `smoke-invariants`, the `iso` and `$(ISO_HD)` rules, the two arch `start.c`
  files read in full, `syscall.h` enumerated, and `ci.yml`'s package list.
  The x86 ISO gates' green status comes from CI having run them, not from a
  run in this session.
* **§1.3 is not acted on.** The two GRUB packages stay in `ci.yml`; removing
  them changes CI behaviour on four jobs and is the operator's call.
* **§1.5 is not acted on.** The empty `arch/<arch>/` directories stay.
* **No release action is taken or proposed.** `v1.0.0` stays untagged, the
  `main` promotion stays unstarted, and no promotion is in flight — so §1.6
  costs nothing today and would have cost the third green on the day it
  mattered.
* **Nothing is claimed about the aarch64/riscv64 ports being wrong.**
  ADR-034's boot-only scope is a recorded decision and the ports implement it
  correctly; what is corrected is the Group H rows' description of what
  remains.
* **The Group H header's deadline (`2026-08-24`) is left alone.** §WHAT "DONE"
  MEANS records that `docs/OPERATOR_DIRECTIVE_2026-08-23.md` §1 extended it to
  `2026-08-28`; both dates are now past and which one governs is an operator
  matter, not a measurement.
* **§5 fixes only `PRE_LAUNCH_CHECKLIST.md` §6.** The §5.1b.1 pairing is left
  exactly as written because it is a correct dated statement, and no checker
  change is made — `ci-docstate-check` is unaltered and still passes, now on
  three pairings of which two are current live state and one is annotated
  history.
