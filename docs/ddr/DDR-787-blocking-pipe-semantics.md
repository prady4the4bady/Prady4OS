# DDR-787 — blocking pipe semantics (reader EOF correctness + writer backpressure)

**Status:** **finding + design — NOT yet implemented.** Kernel-side; supports
master-doc **Section B, item 12** (pipes), and it corrects an assumption behind
already-shipped DDR-780 / DDR-786 behaviour.

## The prerequisite check found a bigger problem than the one this slice was scoped for

The slice was queued as "writer loses data past 4 KiB". That is real, but it is
the *smaller* half. Reading `kernel/proc/pipe.c` and the `FD_PIPE` paths in
`kernel/syscall/sys_io.c`:

### 1. The READ side returns premature EOF — this affects every pipeline, not just big ones

```c
long r = pipe_read(e->pipe, kbuf, chunk);
if (r <= 0)
    break;                            /* empty (non-blocking baseline) */
...
return total;                         /* 0 bytes == EOF to the caller */
```

`pipe_read` returns 0 whenever the ring is *momentarily* empty, and `sys_read`
turns that into a 0-byte return, which every reader (including PRISM's
`do_cat_stdin`) treats as **end of file**. Nothing waits for the writer.

So `a | b` is **timing-dependent**: if `b` is scheduled before `a` has written,
`b` sees EOF immediately and prints nothing. The DDR-780/786 gates pass because
in practice the earlier-forked stage runs first under this scheduler — that is
scheduling luck, not a guarantee. **This is a latent correctness bug in shipped
behaviour**, and it is why it is recorded here rather than folded into a
"performance" slice.

### 2. The WRITE side silently truncates (the originally-scoped issue)

`pipe_write` fills until `head - tail == PIPE_SIZE` (4096) and returns short;
`fd_write_user` then `break`s. A stage producing >4 KiB faster than its reader
drains **loses the remainder with no error**.

### 3. Neither can be fixed safely today: `struct pipe` cannot tell readers from writers

```c
struct pipe { uint64_t buf; uint32_t head, tail; int refcount; };
```

There is a **single** `refcount`. Blocking requires the opposite-side termination
condition:

- a reader may only block while **at least one writer fd remains** — otherwise it
  must return EOF, or it blocks forever;
- a writer may only block while **at least one reader fd remains** — otherwise it
  must fail (`EPIPE`), or it blocks forever.

With one refcount neither condition is computable, so **any blocking
implementation built on today's `struct pipe` would be unbounded — a direct S2
violation.** Splitting the refcount into `readers` / `writers` is therefore a
prerequisite, not an optional extra.

## Design (for implementation, once accepted)

- `struct pipe`: replace `refcount` with `readers` and `writers`, maintained in
  `install()`, `pipe_incref` (needs the end's role — `sys_dup2` copies
  `e->flags`, which already distinguishes `PIPE_WRITE_END`) and `pipe_close`.
  Free the pipe when both reach zero.
- **Reader:** while the ring is empty and `writers > 0`, wait; then re-check.
  Return 0 only when empty and `writers == 0`.
- **Writer:** while the ring is full and `readers > 0`, wait; then re-check.
  Return `-EPIPE` when `readers == 0` (no signals — `SIGPIPE` is a non-goal;
  PRISM must handle the errno).
- **Waiting mechanism:** the tree already blesses a yield-poll for exactly this
  shape — `FD_CONSOLE`'s blocking read loops on `kgetc_nb()` + `yield()`
  (`sys_io.c`). Reusing that keeps the change small and avoids adding a wait
  queue to `struct pipe`. `sched_block_on(spinlock_t*)`/`sched_unblock(tcb*)`
  exist and would be more efficient, but they need a waiter list per pipe and a
  wake on every read/write — more machinery and more ways to lose a wakeup.
  **Recommend yield-poll first** (correct, obviously bounded by the
  writers/readers condition), and only move to `sched_block_on` with a measured
  reason. Either way the loop's termination is the refcount condition, never a
  timeout.

## Gates — both must discriminate

1. **Reader EOF (the latent bug):** a pipeline whose *reader* is deliberately
   scheduled first must still deliver its data. Cheapest deterministic form: pipe
   a marker through several stages and assert it arrives — with a stage count high
   enough that "the writer happened to run first" cannot hold for all of them.
2. **Writer backpressure:** push **> 4 KiB** through `a | cat` and assert
   **byte-exact** delivery (e.g. a known line count, plus first and last markers).
   Today this silently truncates at 4096 bytes, so the assertion fails before the
   fix and passes after.

## Architecture prerequisite checklist

- **New syscalls / NSI:** none — behaviour change inside `SYS_READ`/`SYS_WRITE`
  on `FD_PIPE`. NSI stays 75.
- **Kernel structures:** `struct pipe` gains `readers`/`writers` (replacing
  `refcount`); `pipe_incref`/`pipe_close` take or infer the role. `fd.c`'s
  `fd_free` and `sys_dup2`'s pipe-sharing path must be updated in lockstep —
  **a missed increment leaks a pipe or frees it early**, so this is the risky part
  and deserves its own review.
- TCB/roster, PMM/VMM, capabilities, AETHER, network, compositor, FS/on-disk:
  none. Scheduler: no new hook — `yield()` only, as the console path already does.
- **Security invariants:** **S2 is the crux.** Today's non-blocking pipes are
  trivially bounded; blocking is only bounded *because* of the readers/writers
  condition, so that condition is load-bearing and must be established before any
  waiting is introduced. **S6** — a reader/writer fault must not wedge the peer:
  the last-close of either side must wake the other (EOF / `EPIPE`). No
  capability, W^X or NX change; S1/S3–S5/S7/S8 not engaged.
- **No invariant is weakened**, but S2 is *relied upon* in a new way, so this
  warrants explicit maintainer awareness before it lands.

## Non-goals

`SIGPIPE` (return `-EPIPE`; signals are a separate slice), growing `PIPE_SIZE`,
`O_NONBLOCK`/`fcntl`, `poll`/`select` on pipes, and named pipes.
