"""tools/ci/mldsa_ref.py — ML-DSA-44 keyGen reference (FIPS 204), DDR-1053.

NOT SHIPPED CODE. This is the ORACLE that validates the C implementation, and
it exists because of DDR-1052's lesson: there, the round constants were derived
and then proved by re-implementing Keccak in Python against hashlib BEFORE any
C was written -- and the first generator produced RC[0]=0x03 instead of 0x01.
Porting a scheme this intricate straight to freestanding C, where the only
feedback is a KAT that either matches or does not, wastes the debugging budget.

VERIFIED: reproduces NIST ACVP FIPS 204 ML-DSA-44 keyGen vectors tcId 1-5
byte-exactly, both pk (1312 B) and sk (2560 B). Fetch them with
tools/ci/fetch_mldsa_kat.py.

Plain modular arithmetic, no Montgomery form -- clarity over speed, since this
never runs in the kernel.
"""
import hashlib, json
Q=8380417; N=256; D=13; K=4; L=4; ETA=2; ZETA=1753

def shake128(b,n): return hashlib.shake_128(b).digest(n)
def shake256(b,n): return hashlib.shake_256(b).digest(n)
def brv8(x): return int(f'{x:08b}'[::-1],2)
ZETAS=[pow(ZETA, brv8(i), Q) for i in range(256)]

def ntt(w):
    w=w[:]; m=0; ln=128
    while ln>=1:
        start=0
        while start<256:
            m+=1; z=ZETAS[m]
            for j in range(start,start+ln):
                t=z*w[j+ln]%Q
                w[j+ln]=(w[j]-t)%Q
                w[j]=(w[j]+t)%Q
            start+=2*ln
        ln//=2
    return w

def intt(w):
    w=w[:]; m=256; ln=1
    while ln<256:
        start=0
        while start<256:
            m-=1; z=(-ZETAS[m])%Q
            for j in range(start,start+ln):
                t=w[j]
                w[j]=(t+w[j+ln])%Q
                w[j+ln]=(t-w[j+ln])%Q
                w[j+ln]=z*w[j+ln]%Q
            start+=2*ln
        ln*=2
    f=8347681
    return [x*f%Q for x in w]

def rej_ntt_poly(seed):
    x=hashlib.shake_128(seed); buf=x.digest(3*256*3); a=[]; i=0
    while len(a)<256:
        c=buf[i]|(buf[i+1]<<8)|((buf[i+2]&0x7F)<<16); i+=3
        if c<Q: a.append(c)
    return a

def rej_bounded_poly(seed):
    buf=hashlib.shake_256(seed).digest(3*256); a=[]; i=0
    while len(a)<256:
        b=buf[i]; i+=1
        for z in (b&0x0F, b>>4):
            if z<15 and len(a)<256:
                a.append((2-(z%5))%Q)
    return a

def power2round(r):
    r=r%Q; r0=r%(1<<D)
    if r0>(1<<(D-1)): r0-=(1<<D)
    return ((r-r0)>>D, r0)

def simple_bitpack(a,bits):
    out=bytearray(); acc=0; nb=0
    for c in a:
        acc|=(c&((1<<bits)-1))<<nb; nb+=bits
        while nb>=8: out.append(acc&0xFF); acc>>=8; nb-=8
    if nb: out.append(acc&0xFF)
    return bytes(out)

def keygen(xi):
    h=shake256(xi+bytes([K,L]),128)
    rho,rhop,Kk=h[:32],h[32:96],h[96:128]
    A=[[rej_ntt_poly(rho+bytes([s,r])) for s in range(L)] for r in range(K)]
    s1=[rej_bounded_poly(rhop+bytes([i&0xFF,i>>8])) for i in range(L)]
    s2=[rej_bounded_poly(rhop+bytes([(i+L)&0xFF,(i+L)>>8])) for i in range(K)]
    s1h=[ntt(p) for p in s1]
    t=[]
    for r in range(K):
        acc=[0]*256
        for s in range(L):
            for j in range(256): acc[j]=(acc[j]+A[r][s][j]*s1h[s][j])%Q
        acc=intt(acc)
        t.append([(acc[j]+s2[r][j])%Q for j in range(256)])
    t1=[];t0=[]
    for p in t:
        pr=[power2round(c) for c in p]
        t1.append([x[0] for x in pr]); t0.append([x[1] for x in pr])
    pk=rho+b''.join(simple_bitpack(p,10) for p in t1)
    tr=shake256(pk,64)
    sk=rho+Kk+tr
    for p in s1: sk+=simple_bitpack([(ETA-c if c<=ETA else ETA-(c-Q)) for c in p],3)
    for p in s2: sk+=simple_bitpack([(ETA-c if c<=ETA else ETA-(c-Q)) for c in p],3)
    for p in t0: sk+=simple_bitpack([((1<<(D-1))-c) for c in p],13)
    return pk,sk
