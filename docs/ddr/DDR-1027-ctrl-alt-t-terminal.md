# DDR-1027 — Ctrl+Alt+T launches a PRISM terminal window

Status: **IMPLEMENTED + GATED + mutation-checked (M1/M2/M3)**
(§1–§5 landed as design before the code, per §NON-NEGOTIABLE 5; §6 and §7 were
rewritten by what the first run measured — see §6.1 and §9.)
Group E, the last unbuilt row in that group.

---

## 1. What the tracker row understates

The Group E row reads "Ctrl+Alt+T — Launch PRISM terminal window", which sounds
like a keybinding. It is not: PRISM writes to **fd 1** and reads **fd 0**
(`user/prism.c:96`, `:175`), and today those are the serial console. There is no
terminal *window* anywhere in the tree to launch — `smoke-shell` drives PRISM
over the serial line, and every existing surface client
(`user/surfacetest.c`, `user/surfdestroytest.c`) draws coloured rectangles.

So the row is three pieces of work: the chord, a **terminal client** that owns a
surface and a pty-shaped pipe pair, and the plumbing to spawn it.

Each piece is buildable with shipped primitives, which is why this is being
built rather than deferred:

| need | shipped as | evidence |
|---|---|---|
| structured chord with modifiers | `SYS_KEY_POLL` (96) + `KMOD_CTRL`/`KMOD_ALT` | DDR-991/993 |
| spawn a process from ring 3 | `SYS_FORK` (15) + `SYS_EXECVE` (14) | `prism.c:280` `do_run` |
| PRISM reachable by path | kernel writes `PRISM.ELF` to the SFS root at boot | `main.c` `user_boot_from_sfs` |
| bidirectional byte channel | `SYS_PIPE` (17) + `SYS_DUP2` (18) | PROC-A, DDR-778/780 |
| a surface the client draws into | `SYS_SURFACE_CREATE`/`MAP`/`COMMIT` | DDR-706 |
| keys routed to the focused window | `SYS_SURFACE_SENDKEY`/`GETKEY` (55/56) | DDR-708 |
| printable ASCII glyphs in ring 3 | `user/inter_font.h`, 0x20–0x7E at 16px | DDR-728 |

## 2. The one primitive that is missing, and what replaces it

**There is no `O_NONBLOCK` and no `fcntl` in this kernel** — zero matches across
`kernel/proc/pipe.c` and `kernel/syscall/sys_io.c`. A naive terminal that called
`read()` on PRISM's stdout pipe would block whenever PRISM had nothing to say,
which is almost always, and would therefore stop draining its own key ring: the
window would accept no input at all except immediately after output.

`SYS_EPOLL_WAIT` (21) with **timeout 0** is the replacement. It returns 0 when
the pipe is empty and 1 when it is readable, so the client's loop stays a
non-blocking poll of both sources — key ring and pipe — with no new kernel work.
This is why the terminal is an epoll client and not a straight-line reader; it
is the design's only non-obvious shape.

## 3. Why a separate process, not a compositor-internal terminal

`SYS_SURFACE_MAP` is **owner-only**: the compositor read-maps other clients'
surfaces via `SYS_SURFACE_CMAP` and cannot draw into them. A terminal the
compositor owned would work, but would put a child process, two pipes and a
text grid inside the window manager — and the compositor is a single-threaded
render loop whose latency every pointer gate already depends on.

A client process keeps process management out of the WM and makes the terminal
exactly what any other application is. `user/term.c` is that client.

**Not `SYS_SPAWN_AGENT`.** That is the AETHER roster path: it would consume one
of the roster's fixed slots, mint agent capabilities, and appear in
`SYS_AGENT_ROSTER` as though a terminal were an autonomous agent. A terminal is
an application. `fork` + `execve` is the correct door and is what PRISM's own
`run` uses.

## 4. The chord

```c
if (kev[i].code == 't' && (kev[i].mods & KMOD_CTRL) && (kev[i].mods & KMOD_ALT))
```

read from the DDR-991 **event ring**, not the byte stream, for the reason
DDR-995 records: the byte stream carries no modifier state, so a chord cannot be
told from the keystroke there. DDR-992 went further and stopped a non-Shift
chord from emitting text **at all**, so Ctrl+Alt+T produces no `'t'` byte and
the two paths are disjoint at the source — the same property that let Alt+Tab
stop swallowing every bare Tab. No application loses the letter `t`.

`KMOD_CTRL` (0x02) must be added to `compositor.c`'s ABI mirror, which currently
declares only `KMOD_ALT` and `KMOD_META`. The kernel side already derives it
(`ps2kbd.c:108`), so this is a missing mirror constant, not new plumbing.

## 5. `user/term.c`

```
  pipe(to_sh);            term writes  -> PRISM fd 0
  pipe(from_sh);          PRISM fd 1   -> term reads
  fork();
    child:  dup2(to_sh[0], 0); dup2(from_sh[1], 1); close the rest;
            execve("/PRISM.ELF");
    parent: close(to_sh[0]); close(from_sh[1]);
            id = SURFACE_CREATE(w,h); va = SURFACE_MAP(id);
            SET_TITLE(id, "PRISM"); COMMIT(id, x, y); RAISE(id);
            epfd = EPOLL_CREATE; EPOLL_CTL(ADD, from_sh[0]);
            loop:
              while ((c = SURFACE_GETKEY(id)) >= 0)  write(to_sh[1], &c, 1);
              if (EPOLL_WAIT(epfd, &ev, 1, 0) > 0)   read(from_sh[0], buf) -> grid
              redraw dirty rows; YIELD;
```

The grid is a fixed `ROWS x COLS` character array with a write cursor; `\n`
advances a row, a full row wraps, and the bottom row scrolls the array up by
one. **No ANSI, no cursor addressing, no scrollback beyond the visible rows** —
PRISM emits plain lines, and a VT parser is a separate piece of work that this
DDR deliberately does not start.

Glyphs come from `inter_font.h` blitted directly into the surface's BGRA buffer
against the terminal's own background colour. The compositor's `draw_str_inter`
is **not** reusable: it calls `blend_px`, which writes the global framebuffer,
not a surface. Integer alpha blend, no float.

## 6. Sentinels and the gate

`smoke-ctrlaltt`, using the existing `input_inject.sh` (QEMU monitor `sendkey`,
which takes `ctrl-alt-t` directly — the same mechanism `smoke-superkey` uses for
`meta_l-m`).

| arm | sentinel | what it would catch |
|---|---|---|
| A | `PRADYOS_TERM_SPAWN pid=` | the chord did not reach the compositor, or the fork/execve failed |
| B | `PRADYOS_TERM_OK id=` | the client ran but could not create/map/commit its surface |
| C | `PRADYOS_TERM_RX n=` | the fork/dup2/execve/pipe path is broken — PRISM's prompt never came back |
| D | `PRADYOS_TERM_TX ch=` | keys do not reach the window: focus routing, the surface key ring, or the stdin pipe |

A and B alone would pass on a terminal that spawns and draws an empty box, which
is why C and D exist: C proves the child is really PRISM writing down a pipe,
and D proves the input half independently of PRISM's response timing (asserting
on PRISM's *reply* to a typed command would make the gate a race against the
shell's scheduling — the mistake DDR-911 removed from `surfacetest.c`).

Every arm has a reachable failing value, checked against §the dead-arm rule:
none of the four is implied by another. D in particular is not implied by C —
PRISM prints its prompt unprompted, so C fires with no key ever delivered.

## 7. Mutation results

Every mutant fails **exactly one** arm, which is what makes the arms
independent rather than four spellings of one check.

| mutant | change | binding hash | outcome |
|---|---|---|---|
| — (clean) | — | kernel `0d1bcd234707e56d`, `term.elf` `55ad497f47b6d64a` | **PASS**, `8 t-press(es), 4 spawn(s)` |
| **M1** | compositor tests `KMOD_ALT` only | kernel `c2462fb0de8231c9` | **FAIL at arm E** — `PRADYOS_TERM_CHORD mods=4 spawn=1` |
| **M2** | child skips `dup2(from_sh[1], 1)` | `term.elf` `67158f82326cf9ae` | **FAIL at arm C** |
| **M3** | terminal skips `SURFACE_RAISE` | `term.elf` `56028a4dbfe993f8` | **FAIL at arm D** |

**A hash-attribution point, recorded because §R1 alone would mislead here.**
`term.elf` is **not embedded in the kernel image** — it lives on the FAT volume
and is execve'd on demand. So M2 and M3 leave `kernel.bin` **bit-identical** to
the clean build (`0d1bcd234707e56d` for both), and a measurement recorded
against the kernel hash alone would read as two different results from one
binary. The binding artefact for anything in `user/term.c` is `build/term.elf`.
M1 changes the compositor, which *is* embedded, so it moves the kernel hash.

## 8. Scope explicitly NOT taken

- No VT/ANSI escape parsing, no colour, no cursor rendering.
- No resize handling: the grid is sized once at create. A `SURF_EV_RESIZE`
  (DDR-718) arriving would be ignored.
- No `SIGCHLD`/reap: if PRISM exits, the terminal keeps its window and stops
  receiving. Closing the window does not kill the child.
- This does **not** implement ADR-024 §D5's init-driven PRISM respawn. Different
  problem, still unbuilt.


---

## 9. What the first run changed about §6 and §7

Two things in the design above did not survive contact, and both are recorded
rather than quietly edited.

### 9.1 M1 could not be a mutant, so it became a permanent arm

§7 originally proposed testing the chord by injecting a bare `alt-t` and
asserting no *second* spawn. That is unrunnable: `input_inject.sh` **replays its
whole key list four times** (`for _round in range(4)`), and the compositor caps
terminals at four. Both the correct build and a chord-less one therefore report
four spawns, and the count says nothing.

The fix is to stop counting and report the discrimination directly. The
compositor now prints, for **every** `'t'` press, chord or not:

```
PRADYOS_TERM_CHORD mods=6 spawn=1      <- Ctrl(2)|Alt(4): spawned
PRADYOS_TERM_CHORD mods=4 spawn=0      <- Alt only: did not
```

and arm E fails on any `spawn=1` whose `mods` lacks `KMOD_CTRL`. That is a
permanent gate arm rather than a one-off mutation run — it re-checks the chord
on every CI run — and M1 proves it fires. Measured on the clean build: **8
t-presses, 4 spawns**, alternating `mods=6 spawn=1` / `mods=4 spawn=0`.

### 9.2 Four terminals per run, and why that is not a defect

One `ctrl-alt-t` in the gate's key list is four presses on the wire, so four
terminals — each with its own surface and its own PRISM child — and the cap is
reached exactly. This is correct behaviour, not a leak: a user pressing
Ctrl+Alt+T four times should get four terminals. The cap exists only so a stuck
key cannot fork the machine flat.

## 10. Gates

`smoke-ctrlaltt` (shard 0, strict, 180 s) PASS; and, because the compositor
changed, every other gate that drives it re-verified on the same kernel
`0d1bcd234707e56d`: `smoke-compositor`, `smoke-focus`, `smoke-superkey`,
`smoke-modkeys`, `smoke-drag`, `smoke-mouse`, `smoke-surface`, `smoke-agents`,
`smoke-shell` — all PASS. `hygiene_check.sh` all three PASSED (the new gate
needed a `gate_shards.txt` entry; `ci-shard-check` caught its absence, which is
the check working).
