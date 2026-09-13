# DDR-1053 — FIPS 204 ML-DSA vectors ACQUIRED: the predicted blocker does not exist

**Status:** KAT acquisition COMPLETE and verified. Implementation is the next
step, not this one.
**Corrects:** DDR-1052 §7 and `docs/PRE_LAUNCH_CHECKLIST.md` §5.1b.1, both of
which named this as the step most likely to end in a DDR-1038-shaped blocker.

---

## 1. The predicted blocker, and why it does not apply

DDR-1052 §7 recorded the next step's risk in advance:

> *"ML-DSA KATs cannot come from `hashlib` — Python has no ML-DSA — and
> `csrc.nist.gov` is blocked by the proxy, so if pinned FIPS 204 vectors cannot
> be obtained the honest output is a DDR-1038-shaped blocker naming that."*

Both premises are still true. The conclusion does not follow, because there is a
third route that was measured but not pursued: **NIST publishes the ACVP test
vectors in its own GitHub repository**, and `raw.githubusercontent.com` is
reachable (checklist §5.1b.1 fact 3 established exactly that).

```
https://github.com/usnistgov/ACVP-Server
  gen-val/json-files/ML-DSA-keyGen-FIPS204/prompt.json          10,062 B
  gen-val/json-files/ML-DSA-keyGen-FIPS204/expectedResults.json 873,632 B
```

`prompt.json` self-identifies as `algorithm: ML-DSA, mode: keyGen, revision:
FIPS204`, with three parameter sets (ML-DSA-44/65/87) and 25 tests each.

**This is the authority, not a mirror or a third-party reimplementation** — it is
NIST's own repository, reached by the one route this environment permits.

## 2. Why keyGen

`keyGen` is **deterministic**: a 32-byte seed maps to exactly one `(pk, sk)`
pair, with no signing randomness. A mismatch is therefore unambiguous. `sigGen`
vectors exist too and are the next target, but a scheme that cannot reproduce
key generation byte-exactly has nothing to sign with.

## 3. Corroboration, so a parseable file is not silently trusted

The fetched lengths are checked against the FIPS 204 parameters for ML-DSA-44
independently of the file's own claims: **seed 32 B, pk 1312 B, sk 2560 B**. Those
are exactly what the standard specifies (`pk = 32 + 4·320`; `sk = 32 + 32 + 64 +
4·96 + 4·96 + 4·416`). `tools/ci/fetch_mldsa_kat.py` refuses the fetch if any
vector has a different shape — a file that parses but is the wrong thing is not
accepted.

## 4. Provenance is a committed tool, not a session artefact

`tools/ci/fetch_mldsa_kat.py` regenerates `kernel/crypto/mldsa_kat.h` from the
URLs above. Anyone who doubts the pinned bytes can re-derive them. The header
says **do not edit by hand** for the same reason.

Two ML-DSA-44 vectors (ACVP tcId 1 and 2) are pinned in full — seed, pk and sk,
byte-exact. Full bytes rather than hashes: a hash tells you the answer is wrong,
the bytes tell you *where*, and for a scheme with this many interacting stages
that difference is most of the debugging.

## 5. A verified Python reference, and why it exists

`tools/ci/mldsa_ref.py` implements ML-DSA-44 keyGen and **reproduces ACVP vectors
tcId 1-5 byte-exactly, both pk and sk**.

It is not shipped code. It is the oracle for the C port, and it exists because of
DDR-1052's lesson: there the Keccak round constants were derived and then proved
in Python against `hashlib` before any C was written, and **the first generator
produced `RC[0] = 0x03` instead of `0x01`**. Porting something with this many
stages — `ExpandA` rejection sampling, `ExpandS`, NTT/inverse-NTT,
`Power2Round`, three different bit-packings — straight into freestanding C,
where the only feedback is a 1312-byte answer that either matches or does not,
spends the debugging budget in the worst possible place.

With the reference verified, every stage of the C port has a ground-truth value
to compare against, not just the final key.

## 6. What is NOT done

- **No ML-DSA implementation ships in this change.** The kernel does not yet
  contain ML-DSA; this is acquisition and verification only. Nothing here makes
  the OS post-quantum.
- **keyGen only.** `sigGen`/`sigVer` vectors are reachable at the same source
  (5.0 MB and 3.1 MB) and are not yet fetched.
- **ML-DSA-44 only.** The 65 and 87 parameter sets are in the same file and are
  not pinned.
- **Nothing is claimed about constant-time behaviour**, then or now.

## 7. Files

| file | change |
|---|---|
| `tools/ci/fetch_mldsa_kat.py` | NEW — regenerates the KAT header from NIST ACVP, with a shape check |
| `kernel/crypto/mldsa_kat.h` | NEW — 2 pinned ML-DSA-44 keyGen vectors (seed/pk/sk) |
| `tools/ci/mldsa_ref.py` | NEW — verified Python oracle, 5/5 ACVP vectors |
