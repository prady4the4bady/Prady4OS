"""tools/ci/mldsa_sign_ref.py — ML-DSA-44 Sign_internal (FIPS 204 Alg. 7), DDR-1057.

NOT SHIPPED CODE. This is the ORACLE that validates the C signing path, the same
role tools/ci/mldsa_ref.py plays for keyGen, and it exists for the same reason:
DDR-1052 derived and proved the Keccak constants in Python against hashlib BEFORE
any C, and the first generator produced RC[0]=0x03 instead of 0x01. Porting the
signing loop -- ExpandMask, Decompose/HighBits/LowBits, SampleInBall, MakeHint,
three bit-packings and a rejection loop -- straight into freestanding C, where
the only feedback is a 2420-byte answer that either matches or does not, spends
the debugging budget in the worst possible place.

VERIFIED: reproduces NIST ACVP FIPS 204 ML-DSA-44 sigGen vectors byte-exactly,
for the DETERMINISTIC groups (rnd = 32 zero bytes), signatureInterface
`internal`, externalMu false. Fetch them with tools/ci/fetch_mldsa_kat.py.

Deterministic is the whole point: FIPS 204 signs with a random `rnd` by default,
and a randomized signature can only be checked by verifying it -- which passes on
any self-consistent wrong implementation. With rnd fixed, one (sk, message) maps
to exactly one signature and the answer is an ACVP constant.

Plain modular arithmetic, no Montgomery form -- clarity over speed, since this
never runs in the kernel.
"""
import hashlib, importlib.util
import os
_REF = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mldsa_ref.py")
spec = importlib.util.spec_from_file_location("m", _REF)
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
Q, N, D, K, L, ETA = m.Q, m.N, m.D, m.K, m.L, m.ETA
TAU, GAMMA1, GAMMA2, OMEGA = 39, 1 << 17, (Q - 1) // 88, 80
BETA = TAU * ETA                      # 78
LAMBDA_BYTES = 32                     # lambda/4 = 128/4

def shake256(b, n): return hashlib.shake_256(b).digest(n)

def bitlen(x): return x.bit_length()

def simple_bitunpack(by, bits):
    out = []; acc = 0; nb = 0; i = 0
    while len(out) < N:
        while nb < bits:
            acc |= by[i] << nb; i += 1; nb += 8
        out.append(acc & ((1 << bits) - 1)); acc >>= bits; nb -= bits
    return out

def bitunpack(by, a, b):
    bits = bitlen(a + b)
    return [(b - z) for z in simple_bitunpack(by, bits)]

def bitpack(w, a, b):
    bits = bitlen(a + b)
    return m.simple_bitpack([(b - c) for c in w], bits)

def skdecode(sk):
    o = 0
    rho = sk[o:o+32]; o += 32
    Kk  = sk[o:o+32]; o += 32
    tr  = sk[o:o+64]; o += 64
    s1 = []; s2 = []; t0 = []
    for _ in range(L): s1.append(bitunpack(sk[o:o+96], ETA, ETA)); o += 96
    for _ in range(K): s2.append(bitunpack(sk[o:o+96], ETA, ETA)); o += 96
    for _ in range(K):
        t0.append(bitunpack(sk[o:o+416], (1 << (D-1)) - 1, 1 << (D-1))); o += 416
    assert o == 2560, o
    return rho, Kk, tr, s1, s2, t0

def expand_mask(rhopp, mu):
    c = 1 + bitlen(GAMMA1 - 1)                     # 18
    y = []
    for r in range(L):
        v = shake256(rhopp + bytes([(mu + r) & 0xFF, (mu + r) >> 8]), 32 * c)
        y.append(bitunpack(v, GAMMA1 - 1, GAMMA1))
    return y

def decompose(r):
    rp = r % Q
    r0 = rp % (2 * GAMMA2)
    if r0 > GAMMA2: r0 -= 2 * GAMMA2
    if rp - r0 == Q - 1: return 0, r0 - 1
    return (rp - r0) // (2 * GAMMA2), r0

def high_bits(p): return [decompose(c)[0] for c in p]
def low_bits(p):  return [decompose(c)[1] for c in p]

def w1_encode(w1):
    bits = bitlen((Q - 1) // (2 * GAMMA2) - 1)     # 6
    return b''.join(m.simple_bitpack(p, bits) for p in w1)

def sample_in_ball(ct):
    c = [0] * N
    x = hashlib.shake_256(ct)
    buf = x.digest(8 + 1000)
    sgn = int.from_bytes(buf[:8], 'little')
    k = 8
    for i in range(N - TAU, N):
        while True:
            j = buf[k]; k += 1
            if j <= i: break
        c[i] = c[j]
        c[j] = (1 - 2 * (sgn & 1)) % Q
        sgn >>= 1
    return c

def inf_norm(p):
    return max(min(c % Q, Q - (c % Q)) for c in p)

def make_hint(z, r):
    return [1 if decompose((r[i] + z[i]) % Q)[0] != decompose(r[i] % Q)[0] else 0
            for i in range(N)]

def hint_bit_pack(h):
    y = bytearray(OMEGA + K); idx = 0
    for i in range(K):
        for j in range(N):
            if h[i][j]: y[idx] = j; idx += 1
        y[OMEGA + i] = idx
    return bytes(y)

def sign_internal(sk, msg, rnd=bytes(32)):
    rho, Kk, tr, s1, s2, t0 = skdecode(sk)
    s1h = [m.ntt(p) for p in s1]
    s2h = [m.ntt([c % Q for c in p]) for p in s2]
    t0h = [m.ntt([c % Q for c in p]) for p in t0]
    A = [[m.rej_ntt_poly(rho + bytes([s, r])) for s in range(L)] for r in range(K)]
    mu = shake256(tr + msg, 64)
    rhopp = shake256(Kk + rnd + mu, 64)
    kappa = 0
    while True:
        y = expand_mask(rhopp, kappa)
        yh = [m.ntt([c % Q for c in p]) for p in y]
        w = []
        for r in range(K):
            acc = [0] * N
            for s in range(L):
                for j in range(N): acc[j] = (acc[j] + A[r][s][j] * yh[s][j]) % Q
            w.append(m.intt(acc))
        w1 = [high_bits(p) for p in w]
        ct = shake256(mu + w1_encode(w1), LAMBDA_BYTES)
        c = sample_in_ball(ct)
        ch = m.ntt(c)
        cs1 = [m.intt([ch[j] * s1h[i][j] % Q for j in range(N)]) for i in range(L)]
        cs2 = [m.intt([ch[j] * s2h[i][j] % Q for j in range(N)]) for i in range(K)]
        z = [[(y[i][j] + cs1[i][j]) % Q for j in range(N)] for i in range(L)]
        r0 = [low_bits([(w[i][j] - cs2[i][j]) % Q for j in range(N)]) for i in range(K)]
        if (max(inf_norm(p) for p in z) >= GAMMA1 - BETA or
            max(max(abs(x) for x in p) for p in r0) >= GAMMA2 - BETA):
            kappa += L; continue
        ct0 = [m.intt([ch[j] * t0h[i][j] % Q for j in range(N)]) for i in range(K)]
        h = [make_hint([(-ct0[i][j]) % Q for j in range(N)],
                       [(w[i][j] - cs2[i][j] + ct0[i][j]) % Q for j in range(N)])
             for i in range(K)]
        if (max(inf_norm(p) for p in ct0) >= GAMMA2 or
            sum(sum(p) for p in h) > OMEGA):
            kappa += L; continue
        zc = [[(c if c <= Q // 2 else c - Q) for c in p] for p in z]
        return ct + b''.join(bitpack(p, GAMMA1 - 1, GAMMA1) for p in zc) + hint_bit_pack(h)
