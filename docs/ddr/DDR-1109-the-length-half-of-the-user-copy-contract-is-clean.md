# DDR-1109 — THE LENGTH HALF OF THE USER-COPY CONTRACT IS CLEAN

**Assessment. Docs-only: no code change, no gate, NO DEFECT FOUND AND NONE ALLEGED.**
Written in the CI-wait window on `f262ee7` while DDR-1108's build is held — that
hold applies to any kernel change, because a red on a stacked tree cannot be
attributed (DDR-1106's reasoning).

---

## 1. The question DDR-1108 did not ask

DDR-1108 found `sys_io_uring_enter` validating one address and dereferencing
another, and §3 of that record **enumerated the sites of its own shape** — the
three `vmm_resolve` call sites outside `vmm.c` — concluding it was the only one.
That enumeration answers a question about **addresses**.

It does not answer the question about **lengths**, which is the commoner form of
the same family: a syscall that validates a caller-supplied length and then
copies into a fixed-size kernel buffer, where the two quantities are not the
same number. DDR-1108's guard would not have caught such a site and its
enumeration would not have listed one.

Nor does DDR-1041 cover it. That DDR's SMAP sweep established, by measurement
rather than by grep, that *"the kernel NEVER dereferences a raw user pointer
anywhere else"* — a statement about **where** user memory is touched, not about
**how much** of a kernel buffer a touch may fill.

So the length half had never been enumerated. This does that. **The result is a
clean negative** and is reported as one.

---

## 2. What was measured

### 2.1 The validator's two callers

`grep -rn vmm_user_range_ok kernel/ --include=*.c --include=*.h`, excluding
`vmm.c` and `vmm.h`, returns **two** consumers:

* `kernel/syscall/sys_io_uring.c:150` — DDR-1108's, now guarded by
  `if (va & 0xFFF) return -EINVAL;` placed *before* the range check.
* `kernel/mm/uaccess.c` — three call sites, one per primitive.

There is no third. Everything else reaches user memory through `copyin` /
`copyout` / `copyinstr`.

### 2.2 The primitives validate exactly what they copy

Read in full, not inferred:

* `copyin` — `vmm_user_range_ok(cr3, usrc, n, 0)` then `memcpy(kdst, usrc, n)`.
  Same address, same length, same variable.
* `copyout` — the same with `writable=1`, so a write to a read-only or text user
  page is refused at validation (W^X upheld at the copy boundary).
* `copyinstr` — validates **per page as it first steps into it**, `n=1` at the
  byte it is about to read, so the walked address is always the dereferenced
  address.

`vmm_user_range_ok` itself guards its own arithmetic: `end = vaddr + len` with
an explicit `if (end < vaddr) return 0;` wrap check *before* the
`VMM_USER_MIN` / `VMM_USER_MAX` bound. A length large enough to wrap is
rejected, not folded.

### 2.3 Every non-constant-length call site is clamped

74 `copyin`/`copyout` call sites outside `uaccess.c`. **39** pass a `sizeof` of
the destination object and are correct by construction. The remainder — every
site whose length is neither a `sizeof` nor a literal — were read individually:

| site | length | clamp |
|---|---|---|
| `sys_experiment.c:54` | `a2` | `a2 <= 0 \|\| a2 > EXP_MAX_CODE` → `-EINVAL` |
| `sys_agentmem.c:33` | `vlen` | `> MEM_MAX_VAL` → `-EINVAL` |
| `sys_acc.c:108` | `ptlen` | `> ACC_MAX_PT` → `-EINVAL` |
| `sys_vault.c:46` | `slen` | `> VAULT_MAX_SECRET` → `-EINVAL` |
| `sys_ags.c:38`, `:66` | `glen` | `> AGS_MAX_GOAL` → `-EINVAL` |
| `sys_aether.c:125`, `:149` | `len` | `> AETHER_PAYLOAD_MAX` → **clamped** |
| `sys_aether.c:218` | `max` | `> AGENT_ROSTER_N` → **clamped** |
| `sys_aether.c:327` | `n` | kernel-side count from a clamped `max` |
| `sys_socket.c:212`, `:239` | `len` | `> SOCK_IO_MAX` → **clamped** |
| `sys_input.c:28` | `max` | `> INPUT_MAX` → **clamped** |
| `sys_io_uring.c:85`, `:113` | `s->len` | `> 256` → **clamped**, `kbuf[256]` |
| `sys_io.c:32/49/120/305/348/377/397` | `chunk` / `n` / `r` | kernel-side, bounded by `CHUNK` |
| `sys_io.c:214` | `iovcnt * sizeof` | `iovcnt > SYS_IOV_MAX` → `-EINVAL` |
| `sys_io.c:256` | `iov[i].len` | see §2.4 |
| `sys_exec.c:60` | `sizeof p` | constant |
| `sys_proc.c:58` | `need` | `size < need` → `-ERANGE` |
| `sys_file.c:158` | `len + 1` | `while (len < 255 && ...)`, `name[256]` |

Not one site copies more than its destination holds.

### 2.4 Two places where the safety holds for a reason the code does not state

Recorded because they are the shapes that would break first under a future edit,
not because either is wrong today.

**(a) `sys_writev`'s console gather.** `need` is summed over up to
`SYS_IOV_MAX` iovecs whose `len` fields are `uint64_t`, then tested
`need > 0 && need <= GATHER_MAX (256)`, and each `iov[i].len` is copied to
`gather + at`. The invariant `at + len <= need <= 256` holds **provided the sum
did not overflow**. It cannot usefully overflow, and the margin is measured rather than
asserted: `SYS_IOV_MAX` is **16** (`sys_io.c:194`), so reaching 2^64 needs at
least one term of 2^64/16 = **2^60**, against `VMM_USER_MAX` = `0x10000000000`
= **2^40** (`vmm.h:29`) — a factor of **2^20**. `vmm_user_range_ok` rejects such
a term outright (`end > VMM_USER_MAX`, or the wrap guard), so `copyin` returns
`-EFAULT` before writing a byte of it. **The bound is
enforced by `copyin`'s own validation, not by the `need` test** — so a future
change that copied without `copyin` (a `memcpy` from an already-validated
staging buffer, say) would lose the protection silently. The incremental copy
is also clean on the failure path: `gather` is a local and nothing has been
`kwrite`n, which the source comment already states.

**(b) `marshal_vec`'s blob bound is exactly tight, not merely sufficient.** It
checks `*off >= cap` (which alone guarantees only one free byte), then passes
`cap - *off` as `copyinstr`'s `max`. `copyinstr` returns the index of the NUL
only when that index is `< max`, so `len <= cap - *off - 1` and
`*off += len + 1` lands at `cap` at worst, never past it. Correct, and correct
by one byte.

---

## 3. A silent narrowing, recorded and not changed

Seven of the sites in §2.3 **clamp** rather than reject: a caller asking for
more than the bound gets the bound, with no indication. That is the class this
project keeps finding (DDR-1055/1056's splice, DDR-1089's errno erasure,
DDR-1095's 64-entry audit clamp) and it is **not a defect here** — the return
value carries the byte count in every case, which is the POSIX short-write
contract and is what the callers read.

It is recorded for one narrow reason: a reader of `uint8_t kbuf[SOCK_IO_MAX]`
beside `sys_sock_write` should not infer that a caller asking for more was
refused. It was served, shorter. **Nothing is changed** — returning `-EMSGSIZE`
would break green gates for no defect, and DDR-1095 §5 already recorded why a
clamp on a kernel staging buffer is the correct bound.

---

## 4. NOT CLAIMED

* **NO defect found and none alleged.** `uaccess.c`, `vmm_user_range_ok` and
  every call site examined are correct for what they were built to do.
* **NO code change, NO gate, NO new sentinel.** `kernel.bin` is **not rebuilt**,
  so the size/headroom pair and `ci-docstate-check` are unaffected;
  `GLOBAL_FORBIDDEN` 77; 179 gates unchanged.
* **This is not a proof of absence.** What was enumerated is the set of sites
  reached through `vmm_user_range_ok` and through `copyin`/`copyout`/
  `copyinstr`. A site that touched user memory by some *fourth* route would not
  appear in either enumeration — DDR-1041's SMAP sweep is the measurement that
  covers that, and it is cited rather than repeated, because repeating it means
  booting with SMAP forced and every unshielded site naming its own RIP.
* **The 39 `sizeof` sites were not read individually.** They pass the size of
  the destination object, which is correct by construction; they were counted,
  not audited line by line, and that is stated rather than glossed.
* **No open issue moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not
  OPEN-2.
* **DDR-1108 is not extended.** Its fix, its guard and its enumeration stand
  unchanged; this asks the adjacent question and answers it separately.
