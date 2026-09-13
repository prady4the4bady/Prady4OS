# DDR-1048 — Checking a runner-environment assumption locally, before pushing

**Status:** IMPLEMENTED (`tools/ci/runner_env.sh`), verified by reproducing a real
past CI failure. **Plus one real defect found and fixed on its first use.**
**Operator instruction:** PR #17, 2026-09-02 — new top priority, ahead of OPEN-2.

---

## 1. The gap, stated exactly

DDR-1045 took three attempts to fix a broken apt step, and **two of them broke
every toolchain-installing CI job**. Its own conclusion was that both attempts
shared one root cause: *a claim about the runner image asserted without being
able to check it*. The claim was:

> "the Ubuntu archives in `/etc/apt/sources.list` remain"

The operator's diagnosis is that this is structural rather than a one-off: there
was no way to test an environment-dependent assumption except by pushing and
watching real CI fail.

## 2. The finding that reframes it

**This development host is Ubuntu 24.04.4 LTS — the same release as
`ubuntu-latest` — and it already carried the file that refutes the claim.**

`/etc/apt/sources.list` here is a pure comment stub whose own text reads:

```
# Ubuntu sources have moved to the /etc/apt/sources.list.d/ubuntu.sources
# file, which uses the deb822 format.
```

`ls /etc/apt/sources.list.d/` shows `ubuntu.sources` (the archives, deb822)
alongside vendor repos (`docker.list`, `deadsnakes`, `ondrej`) — the same
*shape* as the runner, which carries `packages.microsoft.com` instead.

So the missing capability was never a missing environment. **It was not looking
at the one already running.** A five-second `cat` would have stopped attempt 1
before it deleted the archives on four CI jobs. That is the honest lesson, and it
is cheaper than any tooling.

## 3. What was built

`tools/ci/runner_env.sh`, with the emphasis on making assumptions *checkable*
rather than on simulating a runner:

| subcommand | answers |
|---|---|
| `report [root]` | os release; whether `sources.list` is a stub; every `sources.list.d` entry classified **archives vs vendor**; and an explicit CAN / CANNOT parity statement |
| `sandbox [dir]` | an isolated copy of `/etc/apt` with its own lists+cache, so nothing experiments on the host's real apt |
| `update <sbx>` | `apt-get update` in the sandbox, reporting **the real rc** and the 403 count |
| `resolve <sbx> <pkg>...` | does this sources tree actually resolve these packages? |
| `break-attempt1 <sbx>` | applies DDR-1045 attempt 1's exact mutation |

### 3.1 The isolation that makes `resolve` mean anything

`Dir::State::status` is pointed at an **empty file**. Without it `apt-cache
policy` answers from dpkg's *installed* set, so a package already present on the
box resolves no matter how broken the sources are. **The first draft of this tool
"passed" for precisely that reason** — arm B reported `llvm` resolving with every
source file deleted. A reproduction that cannot fail is not a reproduction; this
is the same vacuity trap as DDR-1040's CPU-model no-op, caught here by the
mutant refusing to fail.

## 4. Verification — the operator's stated bar

> *"reproduce one of the two apt-related failures locally in the new environment
> before trusting it"*

Done. CI run 33650542691 (attempt 1) failed with `Unable to locate package llvm`:

| arm | sources tree | `resolve clang lld llvm nasm xorriso` | rc |
|---|---|---|---|
| A | intact | all five resolve (`llvm: 1:18.0-59~exp2`) | 0 |
| B | `break-attempt1` | all five `UNKNOWN (apt printed nothing)` | 1 |

Arm B **is** the CI failure, reproduced locally in seconds with no CI run spent.
It also confirms, against real apt, the *empty-output* no-candidate shape that
DDR-1045 had to add fixture 5 and M3 for — previously inferred, now observed.

## 5. A real defect found on first use, and fixed

Running the **shipped** `apt_prepare.sh` against real apt (it had only ever been
exercised against hand-written stubs) produced:

```
[apt_prepare] apt-get update: clean
[apt_prepare] FAIL: no candidate for: nasm xorriso
```

while bare `apt-cache policy nasm` printed `Candidate: 2.16.01-1build1`. **The
resolve check was failing packages that resolve.**

**Mechanism, measured not guessed:**

```
$ apt-cache policy clang | grep -q 'Candidate:' ; echo ${PIPESTATUS[*]}
141 0
```

`grep -q` exits at the first match and closes the pipe; `apt-cache` is then killed
by SIGPIPE (141). The script runs under `set -o pipefail`, so the pipeline is
**non-zero although grep matched**, and the `!` branch reads that as "no
Candidate:" and marks the package missing. It is a **race** — which package loses
varies per run (one run: `nasm`, `xorriso`; the next: `clang`; another:
`clang lld llvm nasm dosfstools mtools`).

Measured over 20 consecutive runs of the seven-package CI list:

| form | failures |
|---|---|
| shipped (piped `grep -q`) | **every observed run**, each a different subset |
| fixed (capture once, match as a string) | **0 / 20** |

Fix: capture `apt-cache policy` output **once into a variable** and match it as a
string — no pipe, no race, and half the `apt-cache` invocations.

### 5.1 Why the stub selftest could never see it

The `apt_prepare_selftest.sh` stubs are shell functions emitting a few bytes
instantly, so `grep -q` drains them before exiting and SIGPIPE never fires. The
five fixtures and two mutants of DDR-1045 were all correct and all blind to this.
**A stub reproduces the interface, not the timing.**

### 5.2 What is NOT claimed

**CI has been green with the racy form**, and this DDR does not claim CI was about
to break. The race reproduces near-100% *here* and has evidently not fired on the
runner. **Why is NOT established.** A pipe-buffer explanation was proposed and
then **refuted by measurement** — `apt-cache policy llvm` is 211 bytes, far inside
the 64 KiB buffer, so buffer pressure is not the cause. The defect is real and
latent, the fix has no downside, and the mechanism of the host/runner difference
is left open rather than invented (§NON-NEGOTIABLE 3).

## 6. A DDR-1045 premise that does NOT reproduce

DDR-1045 states:

> "apt-get update exits non-zero if ANY configured repo fails and the step runs
> under set -e, so a vendor repo nobody uses was a single point of failure"

Measured here: `apt-get update` with **four 403 Forbidden** vendor responses
exits **rc=0**. It also exits 0 with *zero* sources configured. So as stated the
premise is not reproduced on Ubuntu 24.04.

This does **not** undermine the DDR-1045 fix — that fix tolerates a failing update
and then *proves the index usable by resolving every requested package*, which is
correct whichever exit-code behaviour holds. Only the stated rationale is partly
unverified. The runner's observed exit 100 is real (it is in the CI log); the
discrepancy is recorded, not explained.

## 7. Limits — read these before trusting a result

- **This host is not the runner image.** Vendor repos differ (`docker` /
  `deadsnakes` / `ondrej` here; `packages.microsoft.com` there) and preinstalled
  tooling differs. `report` prints this CAN/CANNOT split so a reader cannot
  quietly assume parity. §5.2 is a live example of a difference that matters.
- **Answerable here:** apt source layout, apt's output shapes, whether a sources
  tree resolves a package, `apt-get update`'s exit code on a vendor 403.
- **Not answerable here:** which vendor repos the runner carries, preinstalled
  tool versions, and anything about the runner's network policy.
- **Not built:** a container of the real runner image. `docker pull` reaches the
  registry but blob fetches from CloudFront are refused by this environment's
  proxy (403), and the proxy's own status endpoint and README are not readable
  from here. Recorded as a blocker, not worked around.
- **Nothing about OPEN-2 yet.** The operator framed this as possibly helping
  OPEN-2's diagnosis. It does not, so far: OPEN-2 needs CI-side *runtime*
  artefacts, and this addresses build-environment *configuration*. Not claimed.

## 8. Files

| file | change |
|---|---|
| `tools/ci/runner_env.sh` | NEW — report / sandbox / update / resolve / break-attempt1 |
| `tools/ci/apt_prepare.sh` | §5 — resolve loop de-raced (capture once, match string) |
