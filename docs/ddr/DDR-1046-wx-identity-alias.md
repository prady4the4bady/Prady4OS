# DDR-1046 — kernel text was writable through the identity alias

**Status:** FIXED + audited + gated (`smoke-wxkernel`) + M1/M2
**Group:** A — the `Kernel W^X identity-alias removal` row (DDR-757 residual).

---

## §1 — The hole

The kernel image is mapped **twice**:

| mapping | built by | protection |
|---|---|---|
| higher half, 512 × 4 KiB | `vmm_protect_kernel` | text **RX**, rodata **R+NX**, data **RW+NX** — and audited |
| identity, one 2 MiB PD entry at `0x400000` | `stage2.asm:534` as `0x83` = P\|RW\|PS | **NX set by DDR-757. RW kept.** |

DDR-757's own comment was candid about it:

> `/* NX kills execute-via-alias; RW kept (documented residue). */`

So **kernel text was writable through the alias.** W^X was half-enforced —
execute-via-alias closed, write-via-alias open — and a stray write through a
physical address could patch kernel code.

**And the audit could not see it.** `vmm_protect_kernel`'s verdict loop walks
only the higher-half PTEs, so it printed `[wx] kernel W^X OK` on a kernel whose
text was writable. That blind spot is why the residue survived: the gate that
exists to catch writable text reported success.

---

## §2 — MEASURED before changing anything

The obvious worry is that the 2 MiB page also covers rodata, data and BSS, so
making it read-only could fault a legitimate write. Rather than reason about it,
it was measured: clear RW, boot, see what happens.

```
PRADYOS_WX_ALIAS present=1 rw=0 nx=1
[wx] kernel W^X OK
... 423 lines, steady state at t=14500, no fault
```

Line-for-line normal against a 416–418-line baseline at the same tick. **Nothing
in this kernel writes the kernel image through a physical address.**

Two facts make that unsurprising, and both were checked in the source rather
than assumed:

- **`PMM_MIN_PHYS = 0x01000000`** (`pmm.c:14`) — the PMM never allocates below
  16 MiB, so no allocated frame lives in this 2 MiB page.
- **The page tables are at `0x300000`–`0x306000`**, which is PD entry **1**, a
  different 2 MiB page. `table_at()`'s identity access is untouched.

### §2.1 — The readback is not decoration

The first measurement only showed that nothing crashed — which cannot
distinguish "the alias is read-only and nothing writes it" from "the write
protect never applied". The entry is now read **back** and printed:
`PRADYOS_WX_ALIAS present= rw= nx=`. Same reason DDR-1040 reads `CR4` back and
DDR-1044 reads `MCi_STATUS` back: *"I cleared the bit"* and *"the bit is clear"*
are different claims.

---

## §3 — The fix

Two lines of behaviour, and one of coverage:

1. `table_at(lo_pd)[2] &= ~VMM_RW` — no write-via-alias.
2. `|= VMM_NX` retained (DDR-757's half) — no execute-via-alias.
3. **The alias is audited with everything else.** `ok` now fails if the entry is
   absent, writable, or executable. Without this the verdict would still say OK
   on a kernel with a writable alias — the exact state that shipped until now.

The `present` check matters too: unmapping the alias entirely would also
"satisfy" RW=0, and that is a different change with different consequences
(§5), so the audit requires the mapping to still exist.

---

## §4 — Mutants, MEASURED

Clean kernel `762004ab9a1bef13`; the tree rebuilt bit-for-bit to it after the
last revert.

| | mutation | kernel.bin | result |
|---|---|---|---|
| **M1** | keep RW on the alias — **literally the pre-DDR-1046 tree** | `11d7f0c3793d308c` | **rc=2**, `kernel W^X FAIL` |
| **M2** | do not set NX on the alias | `1b99da4fc0550449` | **rc=2**, `kernel W^X FAIL` |

**M1 is the important one, and it is not a synthetic defect — it is the state
this repository was in before this commit.** It now fails, and it fails through
the *audit* (`kernel W^X FAIL`, a `FORBIDDEN_SENTINEL`), not merely through the
new sentinel. That is the whole result: the same kernel that previously printed
`[wx] kernel W^X OK` is now rejected.

---

## §5 — What this does NOT do

- **The alias is not removed**, which is what the queue row's title literally
  says. It is made read-only and non-executable. Unmapping it entirely is a
  larger change: `table_at()` depends on the identity map generally, and while
  the page tables live in a *different* 2 MiB page today, that is a property of
  the current physical layout rather than a guarantee. Read-only + NX removes
  the exploit primitive — a writable, executable kernel image — without
  depending on that layout holding.
- **Nothing about real hardware.** QEMU 8.2.2 TCG, `-cpu` default. The NX half
  is also conditional on `g_nx_ok`, as it has always been.
- **Only the kernel's own alias.** Other identity-mapped physical memory is
  still RW by design; that is what the PMM hands out.
