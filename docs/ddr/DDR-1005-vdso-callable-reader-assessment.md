# DDR-1005 — the "vDSO callable reader" row: what is built, and why the rest should not be built now

**Status:** ASSESSMENT. No code change. Corrects the Group E backlog row.

---

## 1. The row, and what it implies

CLAUDE.md Group E carries:

| Item | Detail | Gate |
|---|---|---|
| vDSO callable reader (`vdso_entry.asm`) | ring-3 seqlock reader (IMP-C) | `smoke-vdso-read` |

Read plainly, that says ring 3 cannot yet read the vDSO and a seqlock is needed to
let it. **Both halves are wrong**, and the third implied deliverable — an
executable entry point inside the page — is a security-posture change rather than
a missing convenience.

## 2. Ring 3 already reads the vDSO, with no syscall, and it is gated

`user/systest.asm` is a **ring-3** probe:

```asm
VDSO_VA    equ 0x00007FFFFFF00000     ; line 41
...
mov     r12, VDSO_VA                  ; line 346
...
m_vdsop:     db "VDSO: clock ns="     ; line 732
```

It loads the user VA, reads the clock with a plain `mov`, formats it, and prints
`VDSO: clock ns=<value>`. `smoke-vdso` (`Makefile:1737`) asserts exactly that
string, and `tools/ci/gate_shards.txt:161` places it on **shard 7, strict**.

So the gate is **not** kernel-side and **not** vacuous — the sentinel it matches
can only be produced by a user-mode program that successfully read
`VDSO_USER_VA`. That was worth checking rather than assuming: a gate asserting a
kernel print would have proven nothing about the user mapping, and this file's
whole subject is a row that claims more is missing than is.

Two further readers use it in anger, not merely in a test:

- `user/actiondagtest.c:97` — `static uint64_t vdso_ns(void) { return *(volatile uint64_t *)VDSO_USER_VA; }`
- `user/compositor.c:750` — the same load

**The value the row exists to deliver — syscall-free clock reads from ring 3 —
is already shipped and exercised.**

## 3. The seqlock is not deferred work; it is unnecessary at one field

`vdso_page.h` states the reason in the source, and it is correct:

> "The single 8-byte `wall_time_ns` is read atomically by a user `mov`, so no
> seqlock is needed on the read side yet; `seq` is reserved for the future
> multi-field / executable-reader extension."

A seqlock exists to make a reader detect that a writer changed *a set of fields*
mid-read, or a value wider than the machine can load atomically. The vDSO record
has exactly one payload field, an aligned `uint64_t`, and an aligned 8-byte load
is atomic on x86_64. **There is nothing to tear.**

Building the protocol now would add a mechanism that prevents no reachable
defect. §NON-NEGOTIABLE 3 — "no fix without a named mechanism from a real failing
artefact" — is the same discipline applied to a feature: there is no artefact and
no mechanism, only a slot named `seq` reserved for a second field that does not
exist. The seqlock becomes owed the moment a second field is added; not before.

## 4. `vdso_entry.asm` is the one genuinely unbuilt part, and it is a posture change

The user mapping is **non-executable, deliberately** (`vdso_page.c:30`):

```c
vmm_map_in(cr3, VDSO_USER_VA, vdso_page_phys,
           VMM_USER | VMM_NX | PTE_SW_SHARED);
```

with the header's rationale: *"W^X holds — no mapping is ever both writable and
executable (kernel view is RW-not-X, user view is R-only)."*

A *callable* vDSO — Linux's `__vdso_clock_gettime` shape, where ring 3 `call`s
into code living in the page — requires that page to be **executable in user
mode**. The kernel keeps a writable view of the same frame through the identity
map. So the frame would be simultaneously writable (ring 0) and executable
(ring 3).

That is not a W^X violation under the letter of the rule as this project states
it — the two views are separate mappings and neither is itself W+X — but it is
exactly the arrangement W^X exists to make rare, and it hands ring 3 an
executable page whose contents ring 0 can rewrite at any time. **It deserves its
own DDR and a threat-model paragraph, not a backlog checkbox.**

Against that cost, the benefit over the status quo is: a `call` instead of a
`mov`. The existing readers show the `mov` is sufficient for every consumer in
the tree.

## 5. Recommendation

1. **Correct the row.** The ring-3 reader is built and gated (`smoke-vdso`,
   shard 7, strict). `smoke-vdso-read` should not be created as a second gate for
   a property `smoke-vdso` already asserts — a duplicate gate that passes for the
   same reason is cost without coverage.
2. **Do not build the seqlock** until a second field lands. Record it as owed at
   that point, in `vdso_page.h`, where the reasoning already lives.
3. **Do not build `vdso_entry.asm` for 1.0.** It is a security-posture change
   (user-executable page over a kernel-writable frame) whose only gain is call
   syntax. If it is wanted post-1.0, it needs its own DDR covering the W^X
   argument, not this row.

## 6. What this file does NOT claim

It does not claim the vDSO is finished as a subsystem. `wall_time_ns` is its only
field; there is no `clock_gettime`-equivalent surface, no CLOCK_MONOTONIC vs
CLOCK_REALTIME distinction, no timezone or resolution data. Those are real gaps.
The claim is narrower and exact: **the ring-3 reader named by the Group E row
exists, works, and is gated, and the two things still missing from that row
should not be built now** — one because it prevents nothing, the other because it
trades a security property for syntax.
