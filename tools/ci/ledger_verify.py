#!/usr/bin/env python3
"""DDR-1150: verify PRADYOS_LEDGER signatures OFF the machine.

  ledger_verify.py <capture> [--pk <pkhex-file>]    verify every signed head
  ledger_verify.py <capture> fp                     print the pk fingerprint

Verification uses tools/ci/mldsa_sign_ref.py's verify_internal -- an
independent Python implementation of FIPS 204 checked against NIST ACVP
(DDR-1057/1058), NOT the kernel's code. A kernel verifying its own signature
passes on any self-consistent wrong implementation (the dead-arm class).

--pk is the real deployment path: the public key the installer printed and the
operator recorded off the machine. Without it the pk is read from the capture,
which proves the signatures are sound but NOT that the key is the published one
-- that half is a deployment property, stated in DDR-1150 sec.2, not a gate one.

Checks (the gate's arms):
  A  every signature verifies under pk
  B  the message format is exact, and a later head differs from an earlier one
     with a strictly larger `written` -- a kernel signing a constant head
     passes A and fails here
  C  a one-byte change to a message must NOT verify -- without this, A passes
     on a verifier that says yes to everything
"""
import hashlib, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mldsa_sign_ref import verify_internal  # noqa: E402

DOMAIN = b'PRADYOS-LEDGER-v1\x00'
MSG_BYTES, PK_BYTES, SIG_BYTES = 58, 1312, 2420


def chunks(txt, tag):
    """{n: bytes} reassembled from '<tag> n=<k> i=<j> <hex>' lines, in i order."""
    parts = {}
    for n, i, hx in re.findall(tag + r' n=(\d+) i=(\d+) ([0-9A-F]+)', txt):
        parts.setdefault(int(n), {})[int(i)] = bytes.fromhex(hx)
    out = {}
    for n, d in parts.items():
        if sorted(d) != list(range(len(d))):
            sys.exit(f'ledger_verify: FAIL -- {tag} n={n} has missing chunks {sorted(d)}')
        out[n] = b''.join(d[i] for i in range(len(d)))
    return out


def fail(msg):
    print('ledger_verify: FAIL -- ' + msg)
    sys.exit(1)


def main(argv):
    if not argv:
        sys.exit(__doc__)
    txt = open(argv[0], errors='replace').read()
    pks = chunks(txt, 'PRADYOS_LEDGER_PK')
    if 0 not in pks or len(pks[0]) != PK_BYTES:
        fail('no complete 1312-byte PRADYOS_LEDGER_PK in the capture')
    pk = pks[0]
    if len(argv) >= 2 and argv[1] == 'fp':
        print(hashlib.sha256(pk).hexdigest()[:16])
        return
    if len(argv) >= 3 and argv[1] == '--pk':
        pub = bytes.fromhex(open(argv[2]).read().strip())
        if pub != pk:
            fail('the capture\'s pk is NOT the published pk (a substituted key)')
        pk = pub

    msgs = chunks(txt, 'PRADYOS_LEDGER_MSG')
    sigs = chunks(txt, 'PRADYOS_LEDGER_SIG')
    if sorted(msgs) != [1, 2] or sorted(sigs) != [1, 2]:
        fail(f'expected signed heads n=1,2; got msg={sorted(msgs)} sig={sorted(sigs)}')

    heads = {}
    for n in (1, 2):
        m, s = msgs[n], sigs[n]
        if len(m) != MSG_BYTES or len(s) != SIG_BYTES or m[:18] != DOMAIN:
            fail(f'n={n}: message/signature format is wrong (len {len(m)}/{len(s)})')
        if not verify_internal(pk, m, s):                               # arm A
            fail(f'n={n}: signature does NOT verify under pk')
        heads[n] = (int.from_bytes(m[18:26], 'little'), m[26:58])
        bad = bytearray(m)
        bad[26] ^= 0x01
        if verify_internal(pk, bytes(bad), s):                          # arm C
            fail(f'n={n}: a tampered head ALSO verifies -- the verifier cannot say no')

    (w1, h1), (w2, h2) = heads[1], heads[2]                             # arm B
    if not w2 > w1:
        fail(f'written did not advance across an audited event ({w1} -> {w2})')
    if h1 == h2 or h1 == bytes(32) or h2 == bytes(32):
        fail('the signed head is constant or zero -- it does not bind the chain')

    fp = hashlib.sha256(pk).hexdigest()[:16]
    print(f'ledger_verify: PASS -- 2 heads verify under pk {fp}; '
          f'written {w1} -> {w2}; tampered copies rejected')


if __name__ == '__main__':
    main(sys.argv[1:])
