# DDR-1104 — Group C: the TLS row names a library whose primitives are already in-tree

**Status:** assessment, Markdown-only. No code change, no gate, no defect fixed.
**Rows:** the last three unaudited Group C rows — `smoke-tap`, `smoke-ipv6`,
`smoke-tls`. **None of the three gates exists.** This completes the audit of every
backlog table.

---

## §1 — TLS: the row contradicts itself, and the crypto it would import is shipped

The row reads: *"TLS shim — **mbedTLS or equivalent**; **no out-of-tree libs in OS
image**"*.

**Those two clauses cannot both hold.** mbedTLS *is* an out-of-tree library, and
the constraint is in any case **already violated by two shipped components**:
`third_party/` contains **`lwip`** (linked into the kernel; the whole NET-A/B
layer) and **`musl`** (linked into user programs). So "no out-of-tree libs in OS
image" is not a live constraint — it is contradicted by the row's own named
approach *and* by the artefact that ships.

**And the crypto mbedTLS would be imported for is already here, in pure C, with no
stdlib and no allocation, portable across x86_64/aarch64/riscv64** — read out of
`kernel/crypto/`:

| primitive | file | provenance |
|---|---|---|
| **ChaCha20-Poly1305 AEAD** | `aead.h` (RFC 8439) | DDR-819 |
| **X25519 key agreement** | `x25519.h` (RFC 7748) | DDR-820 |
| **HMAC-SHA256 / HKDF-SHA256** | `hkdf.h` (RFC 2104 / 5869) | DDR-818 |
| SHA-256 / SHA-512 | `sha256.h`, `sha512.h` | DDR-811 family |
| Ed25519 | `ed25519.h` | — |
| SHA-3 / SHAKE, ML-DSA | `keccak.h`, `mldsa.h` | DDR-1052/1054/1057/1058 |

**`TLS_CHACHA20_POLY1305_SHA256` is a real TLS 1.3 cipher suite and every one of
its primitives is in that table.** X25519 is TLS 1.3's default key-share group;
HKDF-SHA256 is precisely its key schedule (RFC 8446 §7.1 is HKDF-Expand-Label over
HKDF-SHA256). DDR-819's header even records *why* ChaCha20 rather than AES-GCM —
constant-time as a property of the algorithm rather than of table lookups, on two
architectures with no AES instructions.

**So the genuine remaining work is NOT cryptography.** It is the **record layer**,
the **handshake state machine**, and **X.509 parsing plus a trust anchor**.

**And the trust anchor is DDR-1059's problem, unchanged.** That DDR measured: no
TPM, no PCRs, no secure boot anywhere in `kernel/` or `boot/`, and the only key
material is `g_owner_seed` — **32 literal bytes compiled into the image**. A TLS
client that cannot anchor trust independently of the artefact it ships in is
**exactly the DDR-1059 shape**: a control that reads considerably stronger than it
is. *"We have TLS"* without a trust store is the same claim as *"post-quantum
signed ledger"* without key custody — and DDR-1059 already refused that one.

**NOT BUILT**, and the row's constraint and named approach both need replacing
before it could be: the honest statement is *"an in-tree TLS 1.3 shim over the
existing ChaCha20-Poly1305 / X25519 / HKDF primitives"*, blocked on the same trust
anchor DDR-1059 named, not on a missing library.

---

## §2 — IPv6: off by a recorded deferral, and the ring-3 ABI is IPv4-shaped

`third_party/lwip-port/lwipopts.h:45` — **`#define LWIP_IPV6 0 /* deferred
(ADR-025 §D5) */`** — beside `LWIP_IPV4 1`. So it is a **recorded deferral in the
source**, not merely unbuilt, and the row's condition (*"after NET-C stable"*)
is not the one the source states.

**And flipping that flag would not deliver the row, which is the trap worth
naming.** The ring-3 surface is IPv4-shaped by type, not by convention:
`sys_socket.c:42` declares `struct net_allow { uint32_t host_be; uint16_t port; }`
and `netallow_check`/`netallow_add` both take `uint32_t host_be` — **a 32-bit
address**. DDR-1070 then stores that same `host_be` per proxy socket for the egress
audit record. So an IPv6 address **cannot be expressed** to the allowlist, to
`SYS_SOCK_CONNECT`, or in an `ACTION_NET_EGRESS` record.

This is the **DDR-1091 shape** (the UDP door: the transport is one question, the
ring-3 door and its enforcement model another). Compiling lwIP's v6 stack would
give the kernel v6 and leave every ring-3 consumer, the CAP_NET allowlist and the
audit trail v4-only.

---

## §3 — TAP: a different *shape* of gate, and the overclaim I nearly wrote

`boot_test.sh:652` runs `-netdev user,id=net0 -device virtio-net-pci,netdev=net0`
(and `:618` an e1000e on the same `user` backend) — **QEMU's user-mode/slirp
backend on every gate**. There is no TAP anywhere.

The shipped substitute is real and is gated: the in-kernel echo server bound at
**127.0.0.1:8007** (`lwip_port.c:374`), which `smoke-nethammer` drives at 20,000
connects with `conn_err=0` and which DDR-1070's phase 4 uses precisely because it
can prove a socket live by reading its echo back.

**The claim I nearly wrote and did not:** that a TAP gate *cannot* run in CI. A TAP
device needs `CAP_NET_ADMIN`/root host-side (`ip tuntap add`), and **GitHub-hosted
runners do provide passwordless sudo** — so it is **not impossible**. What is true
is narrower: it would be the **only gate needing host-side privileged network
setup**, where all 179 others need nothing but QEMU. That is a different *shape* of
gate, with its own failure modes on a runner image this project has already been
bitten by twice (DDR-1045's two wrong fixes, DDR-1048). Recorded as a cost, not as
an impossibility.

---

## §4 — NOT CLAIMED

- **No code change.** `kernel.bin` not rebuilt, so the size/headroom pair and
  `ci-docstate-check` are unaffected. GLOBAL_FORBIDDEN 76; **179 gates unchanged**;
  no new gate, and none of the three named gates should be built.
- **No defect is found in any code and none is alleged.** lwIP's v4-only build,
  the v4-shaped allowlist, the slirp backend and every crypto primitive are correct
  for what they were built to do. What is corrected is the **rows'** account.
- **Nothing is closed and nothing is unblocked.** TLS is **not** nearer to shipping
  because its primitives exist — §1 relocates the blocker from "a missing library"
  to "the record layer, the handshake, and a trust anchor this system does not
  have", which is a **larger** honest statement, not a smaller one.
- **`LWIP_IPV6` is NOT flipped**, no socket ABI is widened, and no NSI is reserved.
- **DDR-1059 is not revisited** — it is cited because its measurement (no root of
  trust; the only key is a compile-time constant) applies unchanged to X.509 trust.
- **No gate was run for this DDR.** What was measured: the existence check for all
  three names; `lwipopts.h:44-45`; `sys_socket.c:42-59`; `boot_test.sh:618`/`:652`;
  `ls third_party/`; `ls kernel/crypto/`; and the headers of `aead.h`, `x25519.h`
  and `hkdf.h` read for provenance and scope.
- **No open issue moves** (OPEN-1/2/12/13 untouched). Not an apfreeze, not OPEN-2.
