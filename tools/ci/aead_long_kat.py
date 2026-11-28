#!/usr/bin/env python3
"""tools/ci/aead_long_kat.py -- DDR-1211: the long-message AEAD known answer.

Computes, with an implementation independent of kernel/crypto/aead.c (the
`cryptography` package's ChaCha20-Poly1305, RFC 8439), the tag and two
ciphertext bytes for a 4096-byte message, and prints them as the C constants
user/aeadtest.c embeds. RFC 8439 has no vector longer than 265 bytes, which is
why the bounded MAC input of DDR-1211 went unnoticed: every vector fit.

The inputs are a pure function of the byte index, so the probe rebuilds them
without storing 4 KiB:
  key[i]   = i
  nonce    = 00 00 00 00 || "DDR-1211"
  aad      = "PRADYOS-LONG"
  pt[i]    = (i * 7 + 3) & 0xFF,  i in [0, 4096)

Run once by hand; the output is pasted into user/aeadtest.c. Not run by CI
(the CI image need not have `cryptography`); the probe carries the answer.
"""
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

N = 4096
key = bytes(range(32))
nonce = b"\x00\x00\x00\x00" + b"DDR-1211"
aad = b"PRADYOS-LONG"
pt = bytes(((i * 7 + 3) & 0xFF) for i in range(N))

out = ChaCha20Poly1305(key).encrypt(nonce, pt, aad)
ct, tag = out[:N], out[N:]

print("static const uint8_t LONG_TAG[16] = {",
      ", ".join(f"0x{b:02x}" for b in tag), "};")
print(f"/* ct[0] = 0x{ct[0]:02x}, ct[4000] = 0x{ct[4000]:02x}, ct[4095] = 0x{ct[4095]:02x} */")
print(f"#define LONG_CT0    0x{ct[0]:02x}")
print(f"#define LONG_CT4000 0x{ct[4000]:02x}")
print(f"#define LONG_CT4095 0x{ct[4095]:02x}")
