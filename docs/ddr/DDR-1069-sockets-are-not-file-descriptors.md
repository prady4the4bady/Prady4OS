# DDR-1069 — epoll for proxy sockets: sockets are not file descriptors, and the missing piece is six lines

**Status:** ASSESSED, **not built**, with the blocker, the measured sizing and the
one real design question named. The DDR-1050/1051 shape.
**Baseline:** `kernel.bin` `b68e241eaaa7b03b` — **unchanged; this is docs-only.**
**Backlog row:** Group C, *"`epoll` / `select` for proxy sockets | `smoke-epoll`"*.

---

## 1. THE BLOCKER IS STRUCTURAL, AND IT IS ONE SENTENCE

**A proxy socket is not a file descriptor**, so it cannot be put in an epoll set.

Measured, not inferred:

- `kernel/proc/fd.h:27-29` defines **three** fd kinds — `FD_CONSOLE`, `FD_VFS`,
  `FD_PIPE`. There is no `FD_SOCK`.
- `grep -rn "FD_SOCK\|fd_alloc.*sock\|sock.*fd_alloc" kernel/` returns
  **nothing**. No bridge exists in either direction.
- `fd_ready_mask()` — the single readiness predicate DDR-1037 deliberately made
  shared by **both** `epoll_wait` and `poll`, so the two can never drift —
  switches on exactly those three kinds.

So the row is not "unbuilt work of the ordinary kind": `epoll_ctl` has nothing to
be handed. `smoke-epoll` does not exist, and neither do the other five Group C
gate names (`smoke-udp`, `smoke-netrevoke`, `smoke-tap`, `smoke-ipv6`,
`smoke-tls`) — **and that is correct**, per DDR-1063 §7c: a planning table naming
a gate for work not yet done is doing its job. **This is NOT another instance of
the never-existed-gate-name class** and must not be counted as one; what
distinguishes this row is only that its first entry is blocked on a structural
fact rather than on effort.

### 1.1 A naming hazard worth fixing on sight

`kernel/syscall/syscall.h:53` documents the connect syscall as:

```c
#define SYS_SOCK_CONNECT   39  /* (host_be, port) -> fd(0..7) | -errno */
```

**It calls the return value an `fd`. It is not one.** It is an index into a
separate 8-slot proxy-socket table, and `pradyos_net.h:18` says so in its own
words — *"Treat it as opaque; only `psock_*` may decode it."* So
`SYS_SOCK_WRITE(fd, …)` and `SYS_WRITE(fd, …)` take integers from **two
different, overlapping numbering spaces**: socket handle 3 and file descriptor 3
are unrelated objects. That is a live trap for anyone who reads the header and
reasonably concludes `epoll_ctl(sockfd)` ought to work — which is very likely how
this backlog row came to be written in the first place.

## 2. WHAT EXISTS, AND WHAT IS ACTUALLY MISSING — MEASURED, AND SMALLER THAN EXPECTED

The `psock_*` surface (`third_party/lwip-port/pradyos_net.h`) is six functions:
`connect`, `state`, `read`, `write`, `close`, `reap_owner`. **There is no
non-destructive readiness call** — `psock_read` *drains* the ring, so
`fd_ready_mask` cannot call it to ask "is this readable?" without consuming the
data it is asking about.

**But the predicate is already in the struct.** `lwip_port.c:695` reads:

```c
while (n < len && s->tail != s->head) {
```

`s->tail != s->head` **is** "this socket has bytes". So the missing primitive is
a `psock_avail(h, owner, sovereign)` of roughly six lines, reusing
`psock_resolve` and the `g_net_lock` acquisition that every other `psock_*` call
already performs. **This assessment began assuming the readiness predicate was
the hard part; measuring it showed the opposite**, which is why the sizing below
is small.

## 3. THE ONE REAL DESIGN QUESTION: WHAT DOES `fork` MEAN FOR AN `FD_SOCK`?

`psock_read` resolves through `psock_resolve(h, owner, sovereign, &s)`, and
**`owner` is the caller's pid** — authority is checked on every call, under the
same lock as the operation (DDR-987 §10). Meanwhile `fd_fork_copy` deep-copies
**every** fd entry to the child.

So an `FD_SOCK` added naively gives a forked child **an fd it cannot use**: the
entry is present, `epoll_ctl` accepts it, and every read or write returns
`-EPERM` because the child's pid is not the owner. That is a semantic trap worth
**deciding** rather than discovering in a gate — and it is a decision, not a
difficulty. Three defensible answers, none obviously right:

1. **Do not inherit** — `fd_fork_copy` drops `FD_SOCK` entries, so the child sees
   a closed fd. Simple, and closest to what the ownership model already means.
2. **Inherit and re-own** — transfer or duplicate ownership to the child. Changes
   `psock`'s authority model, which DDR-731/987 built deliberately.
3. **Inherit as-is** — the child holds a permanently-`-EPERM` fd. Honest to the
   ownership model and useless in practice; the worst of the three, and the one
   that arrives by default if nobody chooses.

## 4. THE SAFE BUILD ORDER, RECORDED SO IT IS NOT RE-DERIVED

1. `psock_avail()` in the lwIP port (§2) — additive, no existing caller changes.
2. `FD_SOCK` kind + a slot field in `struct fd_entry`.
3. **A converter NSI, NOT a change to `SYS_SOCK_CONNECT`.** Six ring-3 programs
   call that syscall today — `sovegresstest`, `privacynettest`, `agent_base`,
   `nethammer`, `capnettest`, `egressaudittest` — so changing its return type
   touches all six and every gate behind them. A separate "give me an fd for this
   socket handle" call leaves the existing ABI alone.
4. `case FD_SOCK` in `fd_ready_mask()` — **this is the whole payoff, and it lands
   once**: DDR-1037 made that function the single answer for both `epoll_wait`
   and `poll`, so `select`-style readiness on sockets arrives with it rather than
   needing its own path.
5. `fd_close` and `fd_fork_copy` per §3's decision.
6. A gate. A peer exists — `smoke-net-tcp-lo` (DDR-753) already drives a TCP
   loopback conversation — so this does not need new network plumbing.

## 5. WHY IT IS NOT BUILT HERE

Not because it is hard — §2 established it is not. Because of **what it touches
against what needs it**:

- It modifies `fd_ready_mask`, which by DDR-1037's design is the **single**
  readiness answer for two syscalls, and the fd table's `fork`/`close` paths.
  Both are load-bearing.
- **Nothing shipping needs it.** All six socket consumers use the blocking
  `SYS_SOCK_READ` with its own timeout, and the only program that would plausibly
  want readiness multiplexing — `agent_base` in live mode — is **excluded from
  CI** (`smoke-agent-live` is developer-run only, checklist §5.4), so the feature
  would ship exercised by a gate written for it and by nothing else.
- §3's fork question is a real decision with three defensible answers, and
  deciding it inside an implementation commit is how a semantic gets chosen by
  accident.

**Revisit when something wants it** — a ring-3 program that must wait on a socket
and a pipe at once is the trigger. Then §4 is the order and §3 is the decision to
take first.

## 6. NOT CLAIMED

- **The network stack is not broken and this is not a defect report.** Blocking
  socket I/O works and is gated (`smoke-net-tcp-lo`, `smoke-nethammer`,
  `smoke-capnet`); what is absent is *readiness multiplexing*.
- **The other five Group C rows are not assessed here.** They are named as
  untouched, not as blocked — `smoke-udp`, `smoke-netrevoke`, `smoke-tap`,
  `smoke-ipv6` and `smoke-tls` were not investigated and nothing about them is
  claimed either way.
- **No measurement of how hard `psock_avail` is beyond reading the loop.** Six
  lines is an estimate from the code shape, not a written-and-compiled figure.
- **`kernel.bin` is untouched.** No code changed in this DDR.
