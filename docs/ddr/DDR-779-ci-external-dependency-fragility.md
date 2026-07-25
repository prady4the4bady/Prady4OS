# DDR-779 — CI is blocked by third-party fetch outages (musl submodule)

**Status:** **finding + proposal — NOT implemented. Needs maintainer sign-off**
(supply-chain change; see "Why this was not done autonomously").
**Master-doc reference:** infrastructure, supporting all of Section B delivery.

## Problem — CI is currently blocked, and this is the second outage this session

Run **30178367399** (commit `21e4a51`, DDR-778) failed after 4m39s at step 2,
`actions/checkout@v5` — **before any project code was fetched**:

```
fatal: unable to access 'https://git.musl-libc.org/git/musl/':
       Failed to connect to git.musl-libc.org port 443 after 135369 ms
fatal: clone of '...' into submodule path 'third_party/musl' failed
Failed to clone 'third_party/musl' a second time, aborting
```

Verified independently: `git ls-remote https://git.musl-libc.org/git/musl` is
**unreachable** from here too, so this is an upstream outage, not a runner glitch.
**Until it resolves, every push produces a failed run** — pushing more commits
cannot help.

This is the **second** external-dependency outage in a single session:

| # | Run | Dependency | Failure |
|---|---|---|---|
| 1 | 30139119085 | `static.rust-lang.org` | `channel-rust-nightly.toml` checksum mismatch → `toolchain 'nightly-…' is not installable` (transient; passed on retry) |
| 2 | 30178367399 | `git.musl-libc.org` | submodule clone timed out twice → checkout aborted (**ongoing**) |

Each costs a full ~2.5 h cycle and blocks promotion to `main`.

## Root cause

`.gitmodules` pins musl to a single upstream host:

```
[submodule "third_party/musl"]  url = https://git.musl-libc.org/git/musl
[submodule "third_party/lwip"]  url = https://github.com/lwip-tcpip/lwip
```

and `ci.yml` uses `submodules: recursive` at checkout. So `git.musl-libc.org`
availability is a **hard single point of failure for every CI run**, ahead of the
build. (lwip already resolves to GitHub and is unaffected — the asymmetry is the
tell.)

## Proposal (for sign-off, not applied)

Point the musl submodule at a well-known GitHub mirror (e.g. `bminor/musl`),
keeping the pinned commit unchanged.

**Why this is safe in principle:** the submodule records an exact commit SHA
(`0784374d561435f7c787a555aeab8ede699ed298`). Git verifies content by SHA, so a
mirror cannot serve different content under the same SHA — content integrity does
not depend on trusting the mirror host.

**Mandatory pre-condition, NOT yet satisfied:** confirm the mirror actually
contains that exact commit. `git ls-remote` only enumerates ref *tips* and the
pinned SHA is an ancestor, so it returned no match — **inconclusive, not
negative**. Verification requires an actual fetch:
`git fetch <mirror> && git cat-file -e 0784374d^{commit}`.

Alternatives, in rough order of robustness: vendor musl into the tree (largest,
removes the dependency entirely); cache `third_party/musl` in CI via
`actions/cache` keyed on the pinned SHA (network only on a cache miss); or split
submodule fetching out of `checkout` into a retried step so an outage fails one
step rather than the whole run.

## Why this was NOT done autonomously

Changing where third-party source is fetched from is **supply-chain-material**. I
could not complete the SHA verification cheaply (it needs a full clone), the
outage is most likely transient, and applying an unverified dependency-source
change to work around it would be poor judgment — precisely the kind of decision
that should be a human's. The finding and the verification recipe are recorded
here so the change is a small, evidence-backed step whenever the maintainer wants
it.

## Immediate operational guidance

- **Do not interpret current CI failures at `actions/checkout@v5` as
  regressions.** Check step 2 first; if it names `git.musl-libc.org`, it is this.
- **Do not push commits purely to retrigger CI while musl is down** — each push
  burns a run that cannot get past checkout.
- Local development is unaffected: `third_party/musl` is already cloned, so
  `make image` and all local gates work normally.
