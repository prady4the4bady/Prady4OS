# DDR-812 (§S5 / F#68) — metric lockbox in the page ring 3 cannot write

**Status:** §Design — code follows DDR-811's two CI greens.
**Date:** 2026-08-01
**Supersedes the §S5 storage decision recorded in:** DDR-810 (which blocked on
three questions; all three are now resolved and are implemented here).
**Depends on:** DDR-811 (SHA-256) — this is `sha256.o`'s first kernel caller, so
it also joins the kernel link in this slice.
**Relates to:** DDR-795 (`metric_page`), §J-03 (audit chain head lives here).

## §Problem

The owner needs a ground-truth record of OS health — kernel identity, boot count,
uptime, gate tallies, agent liveness — that a compromised process cannot rewrite.
"Cannot rewrite" is the entire feature; a record that the adversary can edit and
re-hash proves nothing.

## §Design — authoritative record in `metric_page`, NOT SFS

DDR-810 established why, and it is worth restating because the SFS design is the
intuitive one and it is wrong:

* the VFS gates writes on **`CAP_FS_WRITE` alone** (`vfs.c:91,112,140`);
* every `CAP_SOVEREIGN` process holds `CAP_FS_WRITE`;
* therefore `/metric/lockbox` in SFS is writable by exactly the processes the
  lockbox exists to guard against.

A path-based deny would be a **string comparison** standing in for an invariant,
and namespaces have aliases — relative paths, future mounts, any rename
primitive. `metric_page` (DDR-795) is already a frame mapped read-only + NX into
every user address space: the guarantee comes from **page tables**, not from a
name. A ring-3 store faults and the kernel converts it to a clean process kill,
reusing the path ADR-021's `wxviol` probe exercises.

`metric_page_t` has 4032 bytes of padding, so the record fits without a layout
change beyond claiming part of it.

### Record layout

Appended inside the existing padding, so `metric_page_t` stays exactly 4096 B:

```
kernel_sha[32]        SHA-256 of the running kernel image
boot_count      u64   monotonic; SATURATES at UINT64_MAX, never wraps
last_boot_ts    u64   seconds since epoch from the RTC
rtc_present     u8    0 => last_boot_ts is MEANINGLESS, not "epoch"
gate_pass       u32
gate_fail       u32
total_uptime_s  u64
agent_liveness[N] u8  bitmask, N from AGENTS.md
record_sha[32]        SHA-256 over every field above, in DECLARATION ORDER
```

Two details that are load-bearing rather than stylistic:

* **`rtc_present`** — if no RTC is present the timestamp is not silently coerced
  to 0-as-a-plausible-value. A reader must be able to distinguish "booted at the
  epoch" from "we do not know when this booted". Recording absence explicitly is
  the difference between a fact and a guess.
* **"declaration order"** is stated here *and* in a comment on the struct. A
  future reader must never have to reverse-engineer the hash input; if the order
  is ambiguous, verification silently diverges between producer and consumer,
  which is the failure mode this whole feature exists to prevent.

### Write path — the write-once guarantee

Exactly two call sites: after boot gates complete and before the first user
process spawns; and on clean shutdown before SFS is unmounted. A single
`metric_page_commit()` is the only writer.

DDR-810's spec asked for a compile-time assertion that only those two sites call
it. That is **not expressible in C** — there is no standard construct that
constrains a function's call sites. Rather than write an assertion that cannot
work and call it enforced, this is what is actually implemented:

* `metric_page_commit()` is `static` to a single translation unit containing both
  call sites, so the linker enforces that nothing else can name it;
* a boot-phase guard rejects a call outside the two permitted phases at runtime;
* `smoke-metric-lockbox` arm D asserts the property from outside, which is the
  only check that tests behaviour rather than intent.

Stating this plainly because "enforce with a compile-time assertion" would
otherwise read as done when it is not.

### SFS mirror — explicitly non-authoritative

A copy is written to `/metric/lockbox_mirror` for offline analysis. On read, the
kernel compares it with the page; divergence emits `METRIC_MIRROR_DIVERGE` to the
audit log. The mirror is **never** a trust source — it is writable by
`CAP_FS_WRITE` holders, which is precisely why it cannot be authoritative, and
divergence is therefore a detection event rather than a fault.

## §Invariants

1. Ring 3 cannot alter the authoritative record — enforced by page tables.
2. `SYS_METRIC_READ` never returns a record whose `record_sha` does not verify.
3. `boot_count` never decreases and never wraps.
4. A tampered mirror changes nothing about what `SYS_METRIC_READ` returns.

## §Blast radius

* `kernel/aether/metric_page.c/.h` — record appended in the padding, new
  `metric_page_commit()`.
* `kernel/syscall/` — new `SYS_METRIC_READ`, sovereign-gated.
* `kernel/errno.h` — new `ETAMPER`.
* `Makefile` — `sha256.o` joins the **kernel** link (DDR-811 deferred it for
  lack of a caller; this is that caller).
* `kernel/main.c` — two commit call sites.

## §Gate — `smoke-metric-lockbox`

`FORBIDDEN_SENTINEL: PRADYOS_LOCKBOX_STUB`. Opt-in via `QEMU_PROBES=lockbox`.

* **A** — not committed at boot → `SYS_METRIC_READ` returns `-ENOENT`.
* **B** — committed, `record_sha` corrupted before the read → `-ETAMPER`, and
  **no record bytes are returned**.
* **C** — correct → record returned, and the probe **recomputes SHA-256 over the
  returned fields itself** and compares. Asserting the syscall returned 0 would
  pass against an implementation that never verifies anything.
* **D — the arm that matters.** A `CAP_SOVEREIGN` ring-3 process attempts to
  write the metric page directly. The write must fault into a clean kill, and a
  subsequent `SYS_METRIC_READ` must return the kernel's committed record
  unchanged. Against DDR-810's SFS design this arm fails; that is why it exists.

Arms must have distinct kernel SHAs, and the SHA is printed per arm — DDR-811's
arm A passed with an identical SHA and proved nothing until that was checked.
