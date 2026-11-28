# DDR-1211: The AEAD tag is computed in a stream, with no length bound

Status: accepted. Found while building C2.1 P1a (DDR-1190 A1.11). Fixed here,
in its own PR, before P1a.

## 1. The defect

`aead_tag` (`kernel/crypto/aead.c`) built the Poly1305 input in a 1,024-byte
stack buffer:

    AAD || pad16 || ct || pad16 || len(AAD) || len(ct)

For any longer input it wrote a tag of 16 zero bytes and returned. Its comment
said "caller's verify will fail". That holds only if one side computes the tag
correctly, and both sides call the same function:
- `aead_seal` stores the zero tag;
- `aead_open` recomputes it, gets zeros again, and the comparison passes.

So for every message with `aadlen + ctlen` above about 992 bytes, the tag is a
constant. **Any ciphertext of that length is accepted, and nothing is
authenticated.**

## 2. The measured artefact (the named mechanism)

DDR-1190 P1a seals each 4,096-byte swapped-out page with `aead_seal`. On the
P1a branch (`c21-p1-swap` at `db2212f`, plus the gate wiring), `smoke-swapent`
run locally (QEMU, TCG) produced these lines:

    [swapdbg] corrupt pid=37 slot=1 va=0x0000008801284000
    PRADYOS_SWAP_TAMPER armed evict=0 corrupt=0
    [swapdbg] in pid=37 slot=1 va=0x0000008801284000
    PRADYOS_SWAP_TAMPER SURVIVED v=17

- **TAMPER:** a sealed page with one byte flipped was opened, and swapped in.
- **REPLAY:** a slot's earlier sealed page and tag, put back under a newer
  counter, were also accepted. They decrypted to garbage (`v=230`). A correct
  tag would have been checked under the newer counter's nonce and failed.
- **C2:** `read()` into a corrupted page returned 25.

`aead_open` was called and returned 0 in all three. The `[swapdbg]` lines came
from temporary instrumentation, which is not in the tree. The swap code was
correct: it called `aead_seal` and `aead_open` exactly as `aead.h` documents.

## 3. Who is affected today

[Certain, from the source at `5a7a0d0`] No shipped caller reaches the bound:

| Caller | Largest AAD + message | Under 1,024 B? |
|---|---|---|
| `acc.c` (ACC envelopes) | 0 + `ACC_MAX_PT` = 512 B | yes |
| `vault.c` (secrets) | `VAULT_MAX_NAME` 16 + `VAULT_MAX_SECRET` 256 B | yes |
| `user/aeadtest.c` | RFC 8439 vectors, at most 265 B | yes |

So this is a latent fail-open, not a live bypass. Swap's 4 KiB pages are the
first caller over the bound. The RFC 8439 vectors are all short, so no gate
could have seen it. That is why section 5 adds a long known answer computed by
an independent implementation.

## 4. The fix

Poly1305 gets an incremental form, `poly1305_init`, `poly1305_update` and
`poly1305_final`. It keeps the arithmetic of the existing one-shot
`poly1305_mac`: the same 26-bit limbs, clamping and final reduction.
`poly1305_mac` becomes `init`, then one `update`, then `final`, so the RFC
8439 section 2.5.2 vector still tests the same code.

`aead_tag` streams its five pieces through `poly1305_update`:
1. the AAD;
2. zeros to a 16-byte boundary;
3. the ciphertext;
4. zeros to a 16-byte boundary;
5. the two lengths, as 64-bit little-endian values.

No buffer holds the whole input, so there is no bound and no fail-open
branch.

`poly1305_update` buffers a partial block. Only `poly1305_final` processes a
short last block, with the `0x01` terminator and no implicit bit 128,
whatever the sizes of the earlier calls. RFC 8439 section 2.5.1 defines the
MAC over the concatenated message, so splitting the input across calls must
not change it.

Constant-time comparison (DDR-819) is unchanged.

## 5. Gate: `smoke-aead` gains four arms

The probe `user/aeadtest.c` links this same `kernel/crypto/aead.c`, built for
user space. The long inputs are pure functions of the byte index:
- key `i`;
- nonce `00 00 00 00 || "DDR-1211"`;
- AAD `"PRADYOS-LONG"`;
- plaintext `(7i + 3) mod 256`, 4,096 bytes.

The expected tag and three ciphertext bytes come from
`tools/ci/aead_long_kat.py`, which uses the `cryptography` package's
ChaCha20-Poly1305, an implementation independent of this one. They are pasted
into the probe, so CI does not need the package.

| Arm | Assertion | Fails on the unfixed tree? |
|---|---|---|
| L1 | sealing the 4,096-byte message yields the known tag, and `ct[0]`, `ct[4000]`, `ct[4095]` | yes (the zero tag) |
| L2 | opening it succeeds, and the plaintext round-trips | no: a sanity arm, which catches a streaming fix that breaks open |
| L3 | with `ct[4000]` flipped, open is refused | yes (accepted) |
| L4 | with the AAD's last byte flipped, open is refused | yes (accepted) |

**Mutants** (run in CI, each shown red before merge):

| Mutant | Change | Arms it reddens |
|---|---|---|
| A1 | the old 1,024-byte bound and zero tag put back | L1, L3, L4 |
| A2 | `aead_tag` stops absorbing the ciphertext after 1,024 bytes | L1, L3 (the flip is at offset 4000) |
| A3 | the AAD left out of the tag | L1, L4 |

A1 is the defect itself. A2 is a truncation that would pass every short
vector. A3 is an unauthenticated AAD. Each mutant reddens L1. Beyond that,
each has its own distinguishing arm: L3 for A2, L4 for A3.

## 6. Size and honesty

- **Size.** `kernel.bin` changes by the size of the streaming code, against
  the 1,769,472 B gate. The number is measured in the PR.
- **Labels.** QEMU (TCG) only, not verified on real hardware.
- **Defects.** This closes no listed open defect (OPEN-1/2/12/13, U1 to U3).
  It is a new defect, found and fixed here.
- **Constant time.** It is still not testable by any gate this project runs
  (DDR-819).
