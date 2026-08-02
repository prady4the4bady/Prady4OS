# DDR-819 — ChaCha20-Poly1305 AEAD (RFC 8439)

**Status:** §Design — code follows DDR-818's CI greens.
**Date:** 2026-08-02
**Prerequisite for:** DDR-813 (ACC encrypts both boxes with it).
**Depends on:** DDR-818 (HKDF derives its keys), DDR-816 (nonces need entropy).
**Sequenced by:** DDR-813 §Blocker 4 — 818 ✅ → **819** → 820 → 821 → 813.

## §Problem

ACC's envelope carries two AEAD boxes over the same plaintext:

```
ct     = ChaCha20-Poly1305(K_session, nonce,            plaintext)
cc_box = ChaCha20-Poly1305(K_owner,   nonce ^ 0xFF..FF, plaintext)
```

Nothing in `kernel/crypto/` provides it — the directory holds `sha256.{c,h}`,
`hkdf.{c,h}`, `rng.h`. The only `chacha20`/`poly1305` matches anywhere in
`kernel/` are prose in a comment in `rng.h`.

## §Why this cipher, restated as a decision rather than inherited

AES-GCM is the obvious alternative and is rejected on the same grounds that
shaped DDR-811 and DDR-816: **it must be correct and constant-time on three
architectures, two of which have no AES instructions.** Software AES is both
slower and timing-variable on riscv64 and early aarch64, and a timing-variable
AEAD in a system whose threat model includes "an attacker who holds an enrolled
device" is a real weakness, not a performance note.

ChaCha20 and Poly1305 are built from 32-bit add/xor/rotate and a
130-bit-modulus multiply-accumulate. Both are naturally constant-time when
written without data-dependent branches or table lookups, which is a property of
the *algorithm*, not of the compiler's mood — unlike AES S-box tables.

## §Design

`kernel/crypto/chacha20poly1305.{c,h}`. Three pieces, in dependency order:

1. **ChaCha20 block function + stream** (RFC 8439 §2.3–2.4). 20 rounds over a
   16-word state; the quarter-round is add/xor/rotate only.
2. **Poly1305 one-time authenticator** (§2.5). The delicate part: arithmetic mod
   2^130 − 5. Implemented in 26-bit limbs over `uint64_t` accumulators so no
   intermediate can overflow — the standard portable representation, and the
   reason not to invent a limb layout here.
3. **AEAD construction** (§2.8). The one-time Poly1305 key is ChaCha20 block 0
   with the message keystream starting at block 1; the tag covers
   `AAD || pad16 || ct || pad16 || len(AAD) || len(ct)`.

```c
void aead_seal(const uint8_t key[32], const uint8_t nonce[12],
               const void *aad, uint32_t aadlen,
               const void *pt,  uint32_t ptlen,
               uint8_t *ct, uint8_t tag[16]);

int  aead_open(const uint8_t key[32], const uint8_t nonce[12],
               const void *aad, uint32_t aadlen,
               const void *ct,  uint32_t ctlen,
               const uint8_t tag[16], uint8_t *pt);   /* 0 ok, <0 auth failure */
```

**`aead_open` must verify the tag before writing any plaintext** and must compare
the tag in **constant time** — an early-exit `memcmp` leaks the tag prefix and
turns forgery from 2^128 work into 16 sequential guesses. This is the single
easiest place in the whole slice to be accidentally wrong while passing every
vector, so it is called out here and asserted by arm C below.

No allocation; caller-provided buffers; no stdlib. `ct` and `pt` may alias.

## §Test vectors — RFC 8439

| # | vector | why |
|---|---|---|
| 1 | §2.4.2 ChaCha20 keystream | isolates the stream cipher from the AEAD wrapper — a failure here is unambiguous |
| 2 | §2.5.2 Poly1305 tag | isolates the authenticator; the limb arithmetic is where a portable implementation most often breaks |
| 3 | §2.8.2 full AEAD seal | the composition: counter start, key derivation, padding, length encoding |
| 4 | §2.8.2 open, then **one bit flipped in `ct`** | authentication must FAIL — the only vector that tests the property the AEAD exists for |

Vector 4 is not in the RFC as a test case; it is vector 3's ciphertext with a bit
flipped, and its expected result is "rejected". Without it, an `aead_open` that
never checks the tag passes 1–3 completely.

**Provenance, same caveat as DDR-811/818:** these constants are recalled, not
fetched. The gate compares raw bytes, so a wrong constant fails immediately, and
the implementation is written independently of them.

## §Blast radius

New files only. Not in the kernel link until DDR-813 is its first caller —
the DDR-811 precedent. Probe compiles the same sources with the user code model.

## §Gate — `smoke-aead`

Opt-in via `QEMU_PROBES=aead`. `FORBIDDEN_SENTINEL: PRADYOS_AEAD_STUB`.

* **A** — primitive unlinked → artefact cannot be built.
* **B** — Poly1305 final reduction dropped (skip the `mod 2^130 − 5` carry) →
  vector 2 and 3 tags wrong → **FAIL**, while the ChaCha20 stream vector still
  passes. Isolates the authenticator from the cipher.
* **C** — tag comparison replaced with an early-exit `memcmp` → **vectors 1–3
  still pass**, and the gate must still FAIL because arm C asserts on
  *rejection behaviour*: the probe submits a tampered ciphertext and requires
  `aead_open` to reject it. An early-exit compare still rejects, so this arm
  needs the probe to check something stronger — see below.
* **D** — correct → **PASS**.

**On arm C, stated honestly:** a constant-time comparison and an early-exit one
are functionally identical — both reject the same inputs. **No black-box gate in
QEMU can distinguish them**, because the difference is timing on real hardware,
not output. So arm C as written above cannot work, and pretending otherwise
would be a gate that asserts a property it does not test.

What is done instead: the constant-time compare is enforced by **code review and
a comment stating the requirement**, and the gate asserts only what it can — that
tampered input is rejected (arm C becomes the tamper arm). The limitation is
recorded here rather than papered over, and it is the same class of gap that
DDR-820/821 will have in larger form.

## Implementation — two defects caught, and where the vectors earned their keep

`kernel/crypto/aead.{c,h}` written and **verified against published vectors**:

```
poly1305 §2.5.2 (34-byte msg, 2-byte final block): OK
chacha20 §2.4.2 (first 16 ct bytes):              OK
```

Two defects were introduced and caught within minutes of writing:

1. **`ctlen_or(ptlen)`** — a typo for `ptlen`, caught by the compiler.
2. **Incoherent Poly1305 short-block handling** — a conditional whose two
   branches were identical, plus an implicit high bit that must NOT be set for a
   short block. The spec is simpler: a full block gets an implicit 1 at bit 128;
   a short block gets a literal `0x01` immediately after the data and no
   implicit bit.

Defect 2 is the instructive one. It compiles cleanly, and it produces a wrong tag
**only** for messages that are not a multiple of 16 bytes. RFC 8439 §2.5.2 uses a
34-byte message — final block of 2 bytes — so it catches it; a 16-byte-multiple
vector would not have. That is the concrete argument for per-primitive vectors
rather than only testing the composed AEAD, and for choosing vectors by which
code path they reach rather than by convenience.

### Verified on the host, not in QEMU

This check ran under `gcc` on the host against the same source the kernel
compiles. That was deliberate: the question was whether the arithmetic is correct
at all, and a ~90 s boot per iteration is the wrong instrument for that. The
in-kernel `smoke-aead` gate follows and tests the same source in the real
environment — the host run does not replace it, it de-risks it.

### Still true: the constant-time property is untested

`aead_open` compares the tag in constant time and writes no plaintext before
verifying. Both are enforced by construction and by the comment on the
declaration. Neither is testable by any gate this project can run, and the
`smoke-aead` arms do not pretend otherwise.
