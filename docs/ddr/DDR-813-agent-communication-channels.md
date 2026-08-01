# DDR-813 (§AETHER-ACC-01) — agent channels: design, and three blockers found first

**Status:** §Design — **blocked on one missing primitive and two spec defects.**
No code. Two of the three are correctness bugs in the specification that would
have produced a system that looks encrypted and is not.
**Date:** 2026-08-01
**Depends on:** DDR-811 (SHA-256, shipped), DDR-812 (metric lockbox, designed),
and a kernel CSPRNG that **does not exist** (Blocker 1).

## §Problem

AETHER agents must communicate so that the owner can read everything and nobody
else can read or write anything. The channel substrate is also what DDR-814 (AGS)
and DDR-815 (update propagation) carry their records over, so its correctness is
load-bearing for the whole fleet-sync feature.

## Blocker 1 — there is no entropy source in this kernel

`grep -il "rdrand|rdseed|random|entropy|csprng|getrandom" kernel/` matches
exactly one file, and the match is **the word "random" inside a comment**. There
is no CSPRNG, no `RDRAND` wrapper, no entropy pool.

ACC needs randomness twice, and both are fatal without it:

* **X25519 keypairs.** A predictable private key means every "encrypted" channel
  is readable by anyone who can run the same kernel. The system would present as
  encrypted and provide nothing.
* **ChaCha20-Poly1305 nonces.** Nonce reuse under the same key is catastrophic
  for this construction specifically — it breaks confidentiality *and*
  authenticity, not just one. A counter avoids reuse within a boot, but the spec
  says keys are regenerated per boot, so a counter restarting at 0 against a
  freshly derived key is safe only if the key genuinely differs, which returns
  the problem to key generation.

**This needs its own DDR before ACC.** It is not a small one: `RDRAND`/`RDSEED`
exist on x86_64 but **not** on riscv64 or aarch64 (ADR-034 targets), so a
portable design needs a jitter/timing entropy fallback plus a health test — and
an entropy source that silently degrades to predictable output is worse than an
obvious absence, for the same reason a hand-rolled hash is worse than none.

## Blocker 2 — the owner CC box needs the agent's ephemeral public key in the envelope

The spec derives the CC box from `X25519(owner_pubkey, agent_privkey)` and states
that agent keypairs are **ephemeral, regenerated each boot, not persisted**.

The owner decrypts using the symmetric side, `X25519(owner_privkey,
agent_pubkey)` — which requires `agent_pubkey`. After a reboot the agent's
keypair is gone. Any message written before that reboot becomes **permanently
undecryptable by the owner**, which defeats the entire purpose of the CC box: the
owner's read path is exactly the one that happens later, offline, after the fact.

**Fix:** the envelope carries `agent_pubkey[32]`. It is public, so including it
costs nothing, and without it the owner-visibility guarantee holds only until the
next reboot.

## Blocker 3 — one `OWNER_PUBKEY[32]` cannot serve both roles

The spec embeds a single `OWNER_PUBKEY[32]` and uses it for **Ed25519**
signature verification (AGS) *and* as an **X25519** peer key (ACC CC box). Those
are different key types on different curves. An Ed25519 key can be mapped to
X25519 (the curves are birationally equivalent), but doing so silently, in a
kernel, with a hand-written implementation, is a well-known source of subtle
protocol bugs.

**Fix:** embed two constants — `OWNER_SIGN_PUBKEY[32]` (Ed25519, AGS) and
`OWNER_BOX_PUBKEY[32]` (X25519, ACC). Both generated from owner-held private
keys that never touch a device. Two 32-byte constants are free; a birational
conversion is not.

## §Design (for when the blockers clear)

Primitives, each justified rather than assumed — all constant-time on **all
three** targets, which is the binding constraint:

* **X25519** for key agreement. Constant-time by construction; P-256 has no such
  guarantee without hardware support, which riscv64 lacks.
* **ChaCha20-Poly1305** for AEAD. No AES-NI dependency; AES-GCM in software is
  both slower and timing-variable on riscv64 and early aarch64.
* **Ed25519** for owner authority (AGS). Deterministic signatures — no per-
  signature nonce, which matters given Blocker 1.
* **HKDF-SHA256** for key derivation, on DDR-811's primitive.

Envelope:

```
agent_pubkey[32]      <- Blocker 2: without this the owner loses access at reboot
nonce[12]
ct       = ChaCha20-Poly1305(K_session, nonce,          plaintext)
cc_box   = ChaCha20-Poly1305(K_owner,   nonce^0xFF..FF, plaintext)
K_session = HKDF-SHA256(X25519(agent_priv, peer_pub),      channel_id || "acc-v1")
K_owner   = HKDF-SHA256(X25519(agent_priv, OWNER_BOX_PUB), channel_id || "owner-cc-v1")
```

Both boxes encrypt the **same plaintext** under different keys — the CC box is a
second encryption, not a copy of the ciphertext, so the owner reads without being
a session participant. The nonce XOR is defence in depth: the distinct HKDF
labels already guarantee `K_session != K_owner`, so nonce reuse across the two
boxes is not itself a break, but making the nonces differ mechanically removes
any dependence on that argument holding.

Storage: `/agent/<id>/channel/<peer_id>`, append-only ring, 1024 entries, oldest
overwritten. `metric_page` records **count and last nonce only** — never content.

Syscalls: `SYS_CHANNEL_SEND`, `SYS_CHANNEL_READ` (owner reads any channel via the
CC box; an agent reads only its own inbox, else `-EPERM`),
`SYS_CHANNEL_KEY_REGISTER`.

## §Key custody

* The owner's **private** keys (Ed25519 signing, X25519 box) never exist on any
  enrolled device. The build embeds only the two public constants, generated by
  `tools/gen_owner_key.sh` from `PRADYOS_OWNER_PUBKEY*` env vars supplied by CI
  secrets — never a tracked file.
* **Device loss exposes:** AEAD ciphertext (unreadable without the owner private
  key) and signed update records (readable, not forgeable).
* **Recovery:** re-enroll with a fresh agent keypair and a new owner-signed AGS
  record. The re-enrolled device **cannot read prior channel traffic** — the
  ephemeral key is gone. That is intentional and is the cost of forward secrecy.

## §Gate — `smoke-acc-channel`

`FORBIDDEN_SENTINEL: PRADYOS_ACC_STUB`. Arms A (`-ENOSYS`), B (CC box omitted →
owner read fails `-ENOCC`, agent-to-agent still works), C (correct: A→B delivered,
owner reads via CC box, agent C gets `-EPERM`).

**Mechanism metric:** two distinct messages must produce **distinct ciphertexts**
(proves the nonce is not replayed — the single most likely silent failure given
Blocker 1), and the CC box must decrypt to the *same plaintext* as the direct
read (proves it covers the message rather than being a copy of the ciphertext).

A fourth check follows from Blocker 2 and must be in the gate: **write a message,
simulate a reboot, and have the owner read it**. Without `agent_pubkey` in the
envelope that read fails — and no arm above would have caught it.
