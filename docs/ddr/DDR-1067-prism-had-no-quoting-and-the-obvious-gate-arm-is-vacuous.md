# DDR-1067 — PRISM had no quoting at all, and the obvious gate arm for it is vacuous

**Status:** DESIGN (§NON-NEGOTIABLE 5 — written before the code).
**Baseline:** `kernel.bin` `dde6c5d10748842d`, 1,286,538 B.
**Backlog row:** Group D, *"PRISM pipes / redirection / quoting / job control /
scripting"* — and that row is **stale in the DDR-1063 §7b way**, which is worth
recording before anything else.

---

## 1. THE ROW IS PARTLY BUILT, AND ONLY ONE PART IS MISSING HERE

`smoke-shell`'s own PASS line, read from a green run rather than assumed:

```
[shell] PASS — PRISM_READY + prompt + echo + help + ls + ps + touch/rm +
uname/date/uptime/dmesg/free + redirect(> >> < 2>) + truncate/append + stderr +
pipes(N-stage, >4KiB) + erase(DDR-1039), clean, no panic.
```

So **pipes and redirection are built and gated**; the row reads as though none of
it were. What remains of that row is **quoting**, job control and scripting. This
DDR does quoting and corrects the row rather than leaving it to read as four
unbuilt things.

## 2. THE DEFECT

`user/prism.c:176`, in full:

```c
/* Split s in place on runs of spaces; fills argv (up to maxv), returns argc. */
static int tokenize(char *s, char **argv, int maxv) {
    int argc = 0;
    while (*s && argc < maxv) {
        while (*s == ' ')
            *s++ = 0;
        if (!*s)
            break;
        argv[argc++] = s;
        while (*s && *s != ' ')
            s++;
    }
    return argc;
}
```

**There is no quote handling anywhere.** Consequences, each reachable from the
prompt today:

- `echo "hello world"` passes **three** arguments — `"hello`, `world"` — and the
  quote characters survive into them.
- **A filename containing a space cannot be named at all.** `touch "my file"`
  creates two files, and neither is the one asked for.
- `run /X.ELF "a b"` passes the wrong argv. This one is newly reachable:
  DDR-1032b wired PRISM's `run` through to `execve`'s argv marshalling, so a
  quoting defect now propagates into the child process rather than stopping at
  the shell.

## 3. THE OBVIOUS GATE ARM IS VACUOUS — MEASURED BEFORE WRITING IT

The natural arm is `echo "one two"` asserting `one two`. **It proves nothing.**
`user/prism.c:591`:

```c
} else if (!strcmp(cmd, "echo")) {
    for (int i = 1; i < argc; i++)
        printf("%s%s", argv[i], i + 1 < argc ? " " : "");
```

`echo` **joins its arguments with a single space**, so `echo "one two"` and
`echo one two` produce **byte-identical output**. A gate built on that arm passes
on a shell with no quoting whatsoever — the dead-arm class, and the third time it
has been caught in design text before any code was written (DDR-1039 §3.1 and
DDR-1058 were the first two).

Two arms that do discriminate, and why each does:

### 3.1 `run /ARGTEST.ELF "gamma delta"` → `PRADYOS_ARGC=2`

`user/argtest.asm` prints `PRADYOS_ARGC=<n>` and one `PRADYOS_ARGV=` line per
entry. PRISM's `run` uses the `execv(3)` convention with the path as `argv[0]`,
so:

| line typed | argv delivered | printed |
|---|---|---|
| `run /ARGTEST.ELF "gamma delta"` | `{"/ARGTEST.ELF", "gamma delta"}` | `PRADYOS_ARGC=2` |
| the same without quotes | `{"/ARGTEST.ELF", "gamma", "delta"}` | `PRADYOS_ARGC=3` |

**`PRADYOS_ARGC=2` appears nowhere else** — `smoke-shell` already runs
`run /ARGTEST.ELF alpha beta` (DDR-1032b) and `smoke-execve-argv` asserts
`PRADYOS_ARGC=3`, so the two counts cannot be confused in one log. And
`PRADYOS_ARGV=gamma delta` is a **single argv entry containing a space**, which
no unquoted line can produce. Both directions, per DDR-1039's rule.

### 3.2 `echo "q  9k2"` — the collapsed run

Two spaces inside the quotes. Quoted, `echo` prints `q  9k2`; unquoted, the
tokenizer collapses the run and `echo`'s single-space join prints `q 9k2`. So
even the builtin *can* discriminate — but only on **repeated internal
whitespace**, which is the thing the tokenizer destroys and `echo`'s join cannot
restore. Kept as a second, independent arm because it exercises the builtin path
rather than the `execve` path.

## 4. DESIGN

`'...'` and `"..."`, both literal. A quote character opens a run that ends at the
matching quote; everything between is one token, spaces included. The quote
characters themselves are removed. Quotes may appear mid-token
(`ab" cd"` is one token `ab cd`), because that falls out of the same loop and
special-casing it would be more code, not less.

**In-place, like the original.** `tokenize` shifts bytes down within `s` as it
strips quotes; the result is always no longer than the input, so no buffer is
needed and every existing caller is unaffected.

**An unterminated quote is an ERROR, not a guess.** `echo "abc` returns -1 and
the caller prints `prism: unterminated quote` and runs nothing. Silently treating
it as terminated would make a typo execute a command the user did not write —
and this shell has no continuation prompt to offer instead.

### 4.1 Deliberately NOT done

- **No backslash escapes.** `\"` inside a quoted string, `\ ` outside it. Adding
  them is a separate, larger change (it interacts with the `$?` handling below)
  and is recorded here rather than half-built.
- **No expansion inside double quotes.** In a POSIX shell `"$?"` expands and
  `'$?'` does not. PRISM's `$?` substitution (`prism.c:159-170`) is a **whole-token
  suffix match** applied after tokenizing, so `"$?"` continues to behave exactly
  as `$?` does today. That is a real divergence from POSIX and is stated rather
  than implied.
- **QUOTING DOES NOT PROTECT THE OPERATORS, and this DDR's own first draft
  claimed it did.** I wrote that a quoted `">"` would become a literal argument
  — *"which is the correct shell behaviour"* — and then checked it against the
  design instead of asserting it. It is **false**: `|`, `>`, `>>`, `<` and `2>`
  are matched by `strcmp` on the token **after** the quotes are stripped
  (`prism.c:385`, `:469`), so `echo ">"` still redirects. Making it literal needs
  a parallel *was-quoted* flag threaded through the pipeline split and the
  redirect scan — a real change to two loops, not a side effect of this one.
  **Recorded as a limitation rather than shipped as an unverified claim**, and
  the correction is kept because writing it down and then checking it is what
  caught it.

---

## 5. MEASURED

`kernel.bin` `dde6c5d10748842d` → **`cc8135a9463eefed`**, **1,286,538 B
unchanged** (the change is byte-neutral: PRISM's text grew and shrank within the
same padded region), so §CURRENT BUILD STATE's size/headroom pair is unaffected.

**M1 is the pre-DDR-1067 tokenizer, restored verbatim** — not a synthetic defect
(the DDR-1046 standard). Kernel `2f89f3829acb888d`.

| tree | rc | what the capture says |
|---|---|---|
| **fixed** (`cc8135a9463eefed`) | **0 — PASS** | `PRADYOS_ARGC=2` · `PRADYOS_ARGV=gamma delta` · `prism> q  9k2` · `prism: unterminated quote` |
| **M1** (`2f89f3829acb888d`) | **2 — FAIL** | `PRADYOS_ARGC=3` **twice** · `prism> "q 9k2"` · `prism> unterminated"` |

Reverting M1 returns `cc8135a9463eefed` **bit-for-bit**.

### 5.1 M1's log carries all three halves of the defect, verbatim

- **`PRADYOS_ARGC=3` twice.** The second is the quoted run: it delivered three
  argv entries where two were asked for. The first is the pre-existing unquoted
  arm, and having both in one log is what makes `ARGC=2` an unambiguous target.
- **`prism> "q 9k2"`** — one line showing *both* remaining halves: the quote
  characters survived into the argument, and the two-space run was collapsed to
  one. This is also the line that proves the echo arm is live, since a shell
  with no quoting prints a visibly different string.
- **`prism> unterminated"`** — the unterminated quote was silently accepted and
  echoed as an ordinary word.

### 5.2 The vacuity check was done, not assumed

`grep -c "q  9k2"` on the passing capture returns **1**, and the only other line
containing `9k2` is `cat: cannot open /NOPE9k2.TXT` — which has no double space,
so it cannot satisfy the arm. Checked rather than reasoned, because a marker
reused elsewhere in the same log is how an arm quietly stops discriminating.

## 5.3 Regression — 9 of 9, hash-verified

On `cc8135a9463eefed`, hash checked before and after the suite (DDR-1060 §9's
pin):

```
smoke-shell rc=0        smoke-selftest rc=0    smoke-blkmq rc=0
smoke-execve-argv rc=0  smoke-aether rc=0      smoke-rqstress-liveness rc=0
smoke-ctrlaltt rc=0     smoke-fs rc=0
smoke-iso-userspace rc=0
kernel_after == kernel
```

The set is chosen for what touches PRISM rather than for size:
`smoke-execve-argv` covers the kernel half of argv marshalling that this change
now feeds differently; `smoke-ctrlaltt` runs PRISM **over a pipe pair inside a
terminal window** (DDR-1027), i.e. a second, non-serial consumer of the same
tokenizer; and `smoke-iso-userspace` drives PRISM **from the shipped ISO**, so
the change is exercised on the release artefact and not only on the dev image.
`smoke-selftest` is included because the shell gate's global-forbidden scan
(76 patterns) is what it meta-tests.

## 6. NOT CLAIMED

- **No kernel change.** `tokenize()` is ring-3 shell code; the kernel is
  untouched except for the embedded PRISM image.
- **No new gate.** The arms belong on `smoke-shell`, where PRISM's line handling
  already runs — the same reasoning DDR-1039 recorded for refusing
  `smoke-readline`. Gate count stays **177**.
- **Quoting does not protect the operators** (§4.1) — `echo ">"` still
  redirects. That was this DDR's own wrong first claim and is corrected there.
- **No backslash escapes, and no expansion inside double quotes.** `"$?"`
  behaves as `$?` does, because the `$?` substitution is a whole-token suffix
  match applied after tokenizing.
- **The Group D row said four things and only one is done here.** Pipes and
  redirection were already built and gated (§1); job control and scripting
  remain, and the row is corrected to say so rather than left reading as four
  unbuilt items.
