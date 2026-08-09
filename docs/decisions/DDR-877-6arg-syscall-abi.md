= DDR-877 — six-argument syscall ABI (Group 3 item 19)

**Status:** Accepted
**Date:** 2026-08-09
**Scope:** `arch/x86_64/syscall_entry.asm`, `kernel/syscall/syscall.{c,h}`, all 91
syscall handlers, `kernel/syscall/sys_mmap.c`, `user/systest.asm`,
`smoke-sysmmap`.

## What changed

`syscall_dispatch` and `syscall_fn` take six arguments instead of four, carrying
the full SysV syscall register set: **RDI, RSI, RDX, R10, R8, R9**.

## Every handler takes six, even the ones that use two

The alternative — a per-arity table, or a 4-arg table plus a 6-arg table — means
the dispatcher has to know each handler's arity. A wrong entry in that map reads
a register the caller never set, and passes the garbage on as a real argument.
Nothing crashes; the handler just acts on a value from a previous syscall.

One uniform signature removes the possibility. Handlers that ignore `a5`/`a6`
say so with `(void)a5; (void)a6;`, which `-Wunused-parameter` under `-Werror`
enforces — an unused argument cannot be forgotten silently.

## The marshal ordering is load-bearing

```asm
push r9        ; C arg7 (a6) — must sit at [rsp] at CALL
mov  r9, r8    ; C arg6 (a5)
mov  r8, r10   ; C arg5 (a4)
mov  rcx, rdx  ; C arg4 (a3)
...
```

R8 and R9 are both sources and destinations. `a6` is pushed and `a5` moved
*before* R8 is overwritten. Reversing any two of these lines silently passes a
stale register — a plausible-looking wrong argument, not a crash. The seventh
argument goes on the stack per SysV, so it is pushed last and `add rsp, 8`
follows the call.

## mmap is where this is proved, and it was already wrong

`SYS_MMAP` took `(addr, len, prot, flags)`. POSIX mmap takes six. A caller
passing `fd` and `offset` in R8/R9 — which `user/systest.asm` **already did**,
correctly — had them silently discarded and got anonymous zero pages back. So
"map this file at this offset" returned success and something else entirely.

That is the recurring structural defect in this codebase, seventeen instances
deep: *a check that absorbs invalid input instead of rejecting it, so the drift
is silent and looks like success.* Widening the ABI without fixing mmap would
have left it in place with more registers available to ignore.

`sys_mmap` now reads both and refuses what it cannot do:

- `fd != -1` → **`-ENOSYS`**. File-backed mapping is not implemented. Note this
  rejects `fd == 0` too: some libcs pass 0 for anonymous maps, and 0 is a real
  open descriptor — accepting it means silently ignoring a request to map stdin.
- `offset != 0` → **`-EINVAL`**. Offset is meaningless for an anonymous mapping.

## The gate proves the kernel *reads* a5 and a6, not just that they arrive

`smoke-sysmmap` already had an accept arm passing `r8 = -1, r9 = 0`, so a broken
marshal turns `a5` into garbage and mmap fails. That proves the registers arrive.
It does **not** prove the kernel reads them — a kernel still discarding fd and
offset passes that arm unchanged.

Two reject arms were added:

```
SYSMMAP FD REJECTED     (fd = 3   -> -ENOSYS)
SYSMMAP OFF REJECTED    (off = 4096 -> -EINVAL)
```

They expect **different** errno values on purpose. If the two were symmetric, an
r8/r9 swap in the marshal would pass both. With distinct errors, a swap fails
both.

## Scope

The ABI is widened; no syscall other than mmap grows an argument in this change.
Handlers that would benefit — `pread`/`pwrite`, `mmap` with `MAP_FIXED`,
file-backed mapping itself — are separate items with their own gates. Widening
the transport and then wiring one real consumer is the whole change; adding
speculative arguments to handlers that do not use them would be adding unread
registers back in a different shape.

Gates green: `smoke-sysmmap`, `smoke`, `smoke-user`, `smoke-fs`, `smoke-fs-rw`,
`smoke-fs-sfs-rw`, `smoke-fs-ext4`. Zero warnings under `-Werror`.

**Group 3 item 19 complete.**
