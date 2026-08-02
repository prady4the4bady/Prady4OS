# DDR-818 — HKDF-SHA256, the cheapest of ACC's four missing primitives

**Status:** §Design — code follows DDR-816's two CI greens.
**Date:** 2026-08-02
**Prerequisite for:** DDR-813 (ACC derives both box keys with it), DDR-815.
**Depends on:** DDR-811 (SHA-256, shipped and CI-green).
**Sequenced by:** DDR-813 §Blocker 4 — 818 → 819 → 820 → 821 → 813.

## §Problem

ACC derives two independent AEAD keys from one X25519 shared secret:

```
K_session = HKDF-SHA256(X25519(agent_priv, peer_pub),      channel_id || "acc-v1")
K_owner   = HKDF-SHA256(X25519(agent_priv, OWNER_BOX_PUB), channel_id || "owner-cc-v1")
```

The distinct info labels are what make `K_session != K_owner`, which is what lets
the same nonce appear in both boxes without that being a break. So HKDF is not a
convenience here — the envelope's safety argument rests on it.

Nothing in `kernel/crypto/` provides it: the directory holds `sha256.{c,h}` and
`rng.h`, and the only `hkdf` match anywhere in `kernel/` is zero.

## §Design

`kernel/crypto/hkdf.c` + `hkdf.h`, built directly on DDR-811.

RFC 5869 is two steps, and both are thin over HMAC:

```
PRK       = HMAC-SHA256(salt, IKM)                      -- extract
OKM       = T(1) || T(2) || ... where                   -- expand
  T(0) = empty
  T(n) = HMAC-SHA256(PRK, T(n-1) || info || byte(n))
```

So the real work is **HMAC-SHA256**, which does not exist either and is part of
this slice:

```
HMAC(K, m) = H((K' ^ opad) || H((K' ^ ipad) || m))
K' = H(K) if len(K) > 64, else K zero-padded to 64
```

API, matching the no-allocation/no-stdlib rules of DDR-811:

```c
void hmac_sha256(const void *key, uint32_t klen,
                 const void *msg, uint32_t mlen, uint8_t out[32]);

int  hkdf_sha256(const void *salt, uint32_t saltlen,   /* salt may be NULL   */
                 const void *ikm,  uint32_t ikmlen,
                 const void *info, uint32_t infolen,
                 uint8_t *okm,     uint32_t okmlen);   /* <0 if okmlen > 8160 */
```

`okmlen` is bounded at `255 * 32` per RFC 5869; exceeding it returns an error
rather than silently truncating, because a caller that asked for more key
material than the construction can produce has a bug that must surface.

Caller-provided buffers throughout. No allocation — the same reasoning as
DDR-811: these run at boot and on paths where the allocator is not guaranteed.

## §Test vectors — RFC 5869 Appendix A

Three cases, chosen because each exercises something the others do not:

| # | case | what only this one covers |
|---|---|---|
| 1 | basic, SHA-256, 22-byte IKM, 42-byte OKM | the ordinary path |
| 2 | **long inputs** — 80-byte IKM, salt and info, 82-byte OKM | multi-block HMAC *and* `T(n)` iterating past `n=1`, which a 42-byte OKM never reaches |
| 3 | **zero-length salt and info** | the `salt == NULL` branch, which RFC 5869 says must behave as a string of `HashLen` zeros — a special case that is easy to get wrong and invisible in cases 1 and 2 |

Case 2 is the one that catches an expand loop which only ever produces `T(1)`,
and case 3 is the one that catches treating a NULL salt as a zero-length string
instead of 32 zero bytes. Both are real, both would pass case 1.

**Provenance, same caveat as DDR-811:** these vectors are written from knowledge
of the published RFC, not fetched — this environment has no network. The gate
compares raw bytes, so a wrong constant fails immediately rather than shipping,
and since the implementation is written independently of the vectors, both being
wrong the same way is not a reachable failure mode.

## §Blast radius

New files only. `hkdf.o` joins the kernel link **when it has a caller** — that is
DDR-813, not this slice. DDR-811 established the precedent: an unreferenced
object in the kernel image is dead code, and its arm A was vacuous three times
because nothing linked it.

The probe compiles `sha256.c` and `hkdf.c` a second time with the user code model
and links both — one implementation, two builds.

## §Gate — `smoke-hkdf`

Opt-in via `QEMU_PROBES=hkdf`. `FORBIDDEN_SENTINEL: PRADYOS_HKDF_STUB`.

* **A** — primitive unlinked → the probe cannot build. (Stated as "cannot build",
  not "fails to link": DDR-811 proved `ld -nostdlib` resolves undefined symbols
  to 0 rather than erroring, so the assertion must be on the artefact.)
* **B** — expand loop truncated to `T(1)` only → case 2's 82-byte OKM is wrong
  → **FAIL**, while cases 1 and 3 still pass. This is the arm that justifies
  including case 2 at all.
* **C** — all three vectors match byte-for-byte → **PASS**.

**Mechanism metric:** the probe compares every OKM byte and prints the index of
the first mismatch. "Returned 0" or "output is non-zero" would pass against a
stub that memsets a constant.

Distinct kernel SHAs per arm, with a full artefact scrub between them — DDR-812's
first A/B was invalid because two arms shared a binary, and DDR-811's arm A was
vacuous three times for the same reason.
