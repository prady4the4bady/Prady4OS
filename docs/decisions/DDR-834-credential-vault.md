# DDR-834 — secure credential vault: cloud-bridge credentials encrypted at rest

**Status:** accepted
**Date:** 2026-08-05
**Governs:** `kernel/aether/vault.{c,h}`, `kernel/syscall/sys_vault.c`, NSI 87 / 91
**Table:** 6.9
**Builds on:** DDR-818 (HKDF), DDR-819 (ChaCha20-Poly1305), ADR-032 (SFS)

## The problem

The cloud bridge needs credentials to reach an external endpoint. Today there is
nowhere to put them that survives a reboot without also leaving them readable to
anyone who can read the disk image. A credential in a plaintext file is a
credential that leaks with the disk — and this project's images are routinely
copied, attached to QEMU, and committed near build artefacts.

## Decision

An SFS-backed vault at `/VAULT.BIN`, every record encrypted with
ChaCha20-Poly1305 under a key derived from the owner seed:

```
K_vault  <- HKDF-SHA256(owner_seed, salt=0, "PRADYOS-VAULT-v1")
record   <- name[16] || nonce[12] || tag[16] || ctlen[4] || ct[ctlen]
ct, tag  <- ChaCha20-Poly1305(K_vault, nonce, credential, aad = name)
```

**The name is the AEAD's additional authenticated data.** Without that, a record
could be renamed on disk — swapping the credential returned for `PROD_TOKEN` with
the one stored for `TEST_TOKEN` — while every tag still verified. Binding the name
into the tag makes the association between a name and its secret part of what the
tag covers.

## Both calls are CAP_SOVEREIGN

| call | NSI | capability |
|---|---|---|
| `SYS_VAULT_PUT` | 87 | CAP_SOVEREIGN |
| `SYS_VAULT_GET` | 91 | CAP_SOVEREIGN |

A CAP_AGENT `GET` was considered — the cloud bridge is the consumer, and agents
run the bridge — and rejected. Handing plaintext credentials to any CAP_AGENT
caller would mean a single compromised agent drains the vault, which is precisely
the blast radius the vault exists to bound. **The vault protects credentials at
rest and against a compromised agent; it cannot protect them from a compromised
sovereign, and it does not pretend to.** A bridge that needs a credential runs
sovereign or is handed one by the owner.

## Why the NSI numbers are not adjacent

87 and 91 are the free slots. 82-86 are reserved for Section E
(`SYS_MEMORY_WRITE/READ`, `SYS_CHECKPOINT_AGENT/RESUME_AGENT`,
`SYS_APPROVE_CODE_REWRITE`) and 88-90 for the `prad` package manager. Taking
adjacent numbers now would collide with work already scheduled, and renumbering a
syscall after it ships is a wire-format break. Recorded so the gap is not later
mistaken for an accident.

## Bounds (S2)

- `VAULT_MAX_NAME` 16, `VAULT_MAX_SECRET` 256, `VAULT_MAX_RECORDS` 32.
- A `PUT` to an existing name **replaces** it; the vault is a map, not a log.
- A full vault returns `-ENOSPC`. It never evicts: silently dropping a credential
  would surface much later as an authentication failure with no explanation.

## The gate — `smoke-vault`, four arms

1. **put → get round-trip** — the secret comes back byte-for-byte.
2. **get an unknown name** — `-ENOENT`, not an empty success. A vault that
   returns zero bytes for a missing key hands the caller a "credential" that is
   silently wrong.
3. **tamper the ciphertext on disk, then get** — must be REJECTED. This is the
   arm the feature exists for: arms 1-2 pass on a vault that stores plaintext.
   The probe corrupts `/VAULT.BIN` through the ordinary file syscalls, so it is
   testing at-rest integrity the way an attacker with disk access would.
4. **non-sovereign put and get** — `-EPERM`, audited. Without this the capability
   claim above is untested.

## The rule this earns

**Encrypting a record without binding its name into the authentication leaves the
mapping forgeable even though every record verifies.** Confidentiality of the
values is not integrity of the association; an attacker who can reorder or rename
sealed records controls which secret answers which request, and every tag still
checks out.
