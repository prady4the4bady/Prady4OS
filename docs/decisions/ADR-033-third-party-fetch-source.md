# ADR-033: third-party fetch source vs. content identity (musl submodule mirror)

- **Status:** Accepted 2026-07-26 — **maintainer sign-off obtained** ("use the
  musl mirror permanent fix"), which was the explicit pre-condition recorded in
  **DDR-779**.
- **Date:** 2026-07-26
- **Relation to prior ADRs:** **supersedes exactly one clause of ADR-023** — the
  parenthetical naming `git.musl-libc.org/git/musl` as the fetch source for the
  `third_party/musl` submodule. ADR-023 is otherwise untouched and remains
  binding; in particular its **version pin is unchanged and unchangeable here**
  (musl 1.2.5, commit `0784374d561435f7c787a555aeab8ede699ed298`). Nothing in
  this ADR touches ADR-021 (W^X / NX), which the musl image still satisfies —
  identical bytes cannot change the loader's segment enforcement.

---

## Context — three CI outages, all at the same single point of failure

`.gitmodules` pinned musl to one host, and `ci.yml` checks out with
`submodules: recursive`. That made `git.musl-libc.org` a hard dependency of
**every** CI run, ahead of the build:

| # | Run | Dependency | Failure |
|---|-----|-----------|---------|
| 1 | 30139119085 | `static.rust-lang.org` | nightly channel checksum mismatch (transient) |
| 2 | 30178367399 | `git.musl-libc.org` | submodule clone timed out twice → checkout aborted |
| 3 | 30188805082 | `git.musl-libc.org` | identical: `Failed to connect ... port 443 after 134654 ms`, `Failed to clone 'third_party/musl' a second time, aborting` — only 3 of 113 steps ran |

Outage 3 blocked CI validation of DDR-782 despite that work being complete and
locally green. lwIP, which already resolves to GitHub, was unaffected in all
three — the asymmetry is the tell.

## Decision

Point the `third_party/musl` submodule at a GitHub mirror
(`https://github.com/ifduyue/musl`), **keeping the pinned commit byte-identical**.

### The principle: the fetch source is untrusted; the SHA is the contract

A git submodule records an exact commit SHA, and git verifies content against it
on checkout. A mirror therefore **cannot** serve different source under the same
SHA. Content integrity rests on the pin, not on trusting the host — so the host
is chosen purely for *availability*, and is substitutable in one line.

### Verification performed (this is what DDR-779 blocked on)

DDR-779 could not complete this and said so. It is now done, and the result
corrected the proposal:

- **The mirror DDR-779 named, `bminor/musl`, does not exist** (HTTP 404, while a
  control repo resolves normally). Had it been applied as written, CI would have
  broken differently. Candidate mirrors were enumerated and checked instead.
- **Content identity confirmed by SHA *and tree*, across three independent
  hosts.** `ifduyue/musl`, `tianon/mirror-musl` and `kraj/musl` each return:

  ```
  sha  = 0784374d561435f7c787a555aeab8ede699ed298
  tree = 2deb5f7c62d8c9e9733c9ed77d9210b708bbb69e
  msg  = release 1.2.5
  ```

  and that tree hash equals the tree of the **already-cloned local submodule,
  which was fetched from upstream `git.musl-libc.org`**. Three unrelated mirrors
  agreeing with upstream on the tree hash is stronger evidence than the
  single-mirror check DDR-779 proposed.
- Two candidates (`EOSIO/musl`, `AssemblyScript/musl`) do **not** contain the
  commit and were rejected — the check discriminates rather than rubber-stamping.

Recipe, for re-verification or for swapping mirrors later:

```
gh api repos/<owner>/musl/commits/0784374d561435f7c787a555aeab8ede699ed298 \
   --jq '"sha=\(.sha) tree=\(.commit.tree.sha)"'
# must print tree=2deb5f7c62d8c9e9733c9ed77d9210b708bbb69e
```

(`git fetch <mirror> <sha>` is refused by GitHub's server config, and a full
clone timed out from this network — the API check is equivalent and cheap. Note
a pipeline like `git clone … | tail` reports *tail's* exit status and will
happily print success for a failed clone.)

## Consequences

- **Upstream `git.musl-libc.org` remains canonical** for provenance and for
  future version bumps; the mirror is an availability measure only. A bump must
  re-run the verification above against upstream before the pin moves.
- **Residual risk, stated plainly:** the mirror is a third-party account and
  could be deleted or stop syncing. Deletion is a loud failure (clone error), not
  a silent one, and the fix is a one-line URL change to another verified mirror —
  which is precisely why the SHA pin matters. A stale mirror cannot hurt us: we
  fetch one fixed commit, never a moving ref.
- Not chosen now, and deliberately: vendoring musl into the tree (removes the
  dependency entirely, but adds a 1.2 MB tree ADR-023 explicitly kept out of our
  history) and `actions/cache` keyed on the pinned SHA (complementary, can be
  layered later without another ADR since it changes no source of truth).
- **Security invariants:** none engaged. No kernel code, no capability, no
  syscall, no on-disk format — the built bytes are identical because the source
  bytes are provably identical. S1–S8 all untouched.
