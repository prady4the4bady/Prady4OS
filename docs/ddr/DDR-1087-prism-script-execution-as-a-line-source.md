# DDR-1087 — PRISM `source`: a script is a LINE SOURCE, not a second interpreter

**Status:** DESIGN (code follows in this commit)
**Date:** 2026-09-07
**Branch:** `dev/phase1-seyp3n`

---

## 1. The row, measured rather than taken

Group D carries two rows ending in *"scripting"*:

* *"PRISM pipes / redirection / quoting / job control / scripting"* — DDR-1067
  corrected it: pipes and redirection were **already** shipped and gated, quoting
  landed there, **"job control and scripting remain."**
* *"B#12 PRISM job control"* — DDR-1068 corrected it: `&`/`jobs`/`fg`/`kill %n`
  are built, `bg` is a **recorded refusal**, `wait` landed there,
  **"Remaining on this row: scripting only."**

Given DDR-1085 §1 and DDR-1086 §1 — three consecutive rows whose stated state was
wrong — this one was measured before being believed. **It is accurate.** `grep`
over `user/prism.c` for `script|source|shebang|"#!"` returns only the header
comment saying scripting does not exist, and the builtin list at `prism.c:621` is
the authoritative set: no `source`, no `.`, no `#` handling, no control flow.

**Scripting is genuinely unbuilt.** Recording the negative result because the
last three measurements went the other way.

---

## 2. The design: an alternative line source

`main()`'s dispatch is ~300 lines **inline in the loop body** — there is no
`execute_line()` to call. So `source` has two shapes:

1. **Refactor** the loop body into a function and call it per script line.
2. **Switch the line source**: `readline()` returns the next script line while a
   script is loaded, and falls back to `stdin` when it is exhausted.

**(2), and the reason is blast radius.** Every gate that drives PRISM goes
through `readline()`, but (2) touches only that function plus a prompt guard,
and is *additive* — with no script loaded the function is byte-for-byte the
behaviour it has today. (1) would rewrite the single most-asserted-on code path
in the shell, days from a held release, to gain nothing this increment needs.

It is also the more honest model: **a script is not a second interpreter.** It is
the same interpreter reading from somewhere else, so quoting, redirection, pipes,
`$?` and job control all work inside a script for free and cannot drift from
their interactive behaviour.

---

## 3. NO KERNEL CHANGE — and the test needs none either

The obvious way to test this is to have the kernel place a `/SCRIPT.TXT` on the
FAT volume the way `fat_place_exec_image` places `/EXECTEST.ELF`. **That is not
needed, because redirection is already shipped and gated.** The injector writes
the script itself:

```
echo echo <first-marker>  >  /PRISMSCR.TXT
echo echo <last-marker>   >> /PRISMSCR.TXT
source /PRISMSCR.TXT
```

So this change is **ring-3 only**: `user/prism.c` and the `smoke-shell` recipe.
`kernel.bin` is untouched, no probe ELF is added, `ci-probe-rodata-check` is
unaffected, and the CLAUDE.md size/headroom pair does not move.

It also composes two shipped features to test a third, which is stronger than a
kernel-planted literal would have been: the bytes the script executes provably
travelled through PRISM's own redirection path.

---

## 4. Vacuity: measured, and the obvious arm is LIVE — but WEAK

Stated plainly rather than manufactured, because six of the last seven DDRs found
the obvious arm vacuous and this one is not:

**Can the marker appear without the script running?** No. The injector types
`source /PRISMSCR.TXT`, not the marker; nothing else in the boot prints it; and
with `source` unimplemented PRISM prints `prism: unknown command: source` and the
marker never appears. **The arm is live.**

**It is, however, weak on its own:** a `source` that read only the FIRST line of
the file would pass a single-marker arm. So the script carries **two** lines and
both markers are asserted — the second is what proves the file was consumed to
the end, in order.

That is the whole reason for two lines, and it is what makes M2 (§6) land
somewhere M1 does not.

---

## 5. Three refusals, each with a reason rather than a shrug

* **No nesting.** `source` inside a script is **refused**, not recursed. There is
  one script buffer; a nested `source` would overwrite the outer script's bytes
  *while the outer script is mid-execution*, so the shell would resume executing
  the wrong file at an arbitrary offset. Refusing is the bounded answer (S2), and
  a depth-bounded stack of buffers is not worth 2 KiB × depth of ring-3 BSS for a
  feature nothing yet nests.
* **An oversized script is REFUSED, not truncated.** A truncated script executes
  a *prefix* — some of the user's commands, silently, and then stops. That is
  worse than not running at all, and it is the failure mode a user would least
  expect to be silent.
* **No `#` comments in this increment, and the reason is that I cannot test
  them.** The only script-authoring mechanism available is `echo … > file`
  through this same shell, and a comment stripper would consume the `#` *and the
  redirect that follows it* on the authoring line, so the comment could never be
  written. Deferring on an untestability I measured, not on effort. A real editor
  or a kernel-planted script would unblock it.

---

## 6. Mutants

| mutant | change | expected capture | fails |
|---|---|---|---|
| **M1** | `source` dispatch removed — the pre-fix shell | `prism: unknown command: source`, neither marker | **both arms** |
| **M2** | `readline` serves the first script line then reverts to stdin | first marker present, **last marker absent** | **the last-line arm alone** |

Different arm sets, neither carrying the other — the DDR-1044 M2/M3 check. M2 is
the load-bearing one: it is a `source` that *works* on any single-line script and
silently drops the rest, which a one-marker gate would have shipped.

---

## 7. NOT CLAIMED

* **This is not a scripting language.** No variables, no `if`, no `while`, no
  functions, no `#!` shebang, no argument passing to a script. `source` runs a
  file of commands in the current shell. Stated, not implied.
* **No `#` comments** (§5).
* **No kernel change**, no new syscall, no new gate (178 unchanged) — the arms go
  on `smoke-shell`, the DDR-1039/1070 reasoning.
* **`bg` remains refused** (DDR-1068/DDR-881) and this does not revisit it.
* **No open issue moves**; not an apfreeze, not OPEN-2.
