/* kernel/crypto/rng.h — kernel entropy, DDR-816.
 *
 * Every remaining §S feature is cryptographic, and randomness is where their
 * absence is fatal rather than degrading: a predictable X25519 private key makes
 * every "encrypted" channel readable by anyone running the same image, and nonce
 * reuse under ChaCha20-Poly1305 breaks confidentiality AND authenticity together.
 *
 * FAILS CLOSED. If no real source is present, rng_bytes() fails and crypto
 * refuses to initialise. There is deliberately no jitter/timing fallback: under
 * TCG, timing variance is largely a host-scheduler artefact and is neither
 * measurable nor trustworthy from inside the guest, and a source that silently
 * degrades to predictable output is strictly worse than one that is obviously
 * absent — absence is detectable, degradation is not.
 */
#pragma once
#include <stdint.h>

enum {
    RNG_NONE   = 0,    /* no source — crypto must refuse to start */
    RNG_VIRTIO = 1,    /* virtio-rng: real host entropy           */
    RNG_CPU    = 2     /* RDSEED (x86_64 only)                    */
};

/* Probe the available sources. Returns 0 if any real source was found. */
int      rng_init(void);

/* Fill `out` with `len` bytes. 0 on success, <0 when there is no source.
 * Never returns success without having filled the buffer from a real source —
 * there is no path that emits pseudo-random or constant bytes. */
int      rng_bytes(void *out, uint32_t len);

/* RNG_NONE | RNG_VIRTIO | RNG_CPU. Exists so the failure is VISIBLE: the boot
 * log names the active source and the DDR-812 lockbox records it. A machine
 * running without entropy must say so on every boot, not discover it when a key
 * is needed. */
unsigned rng_source(void);

/* Two draws compared byte-for-byte: proves the source is not a fixed buffer,
 * which is the likeliest real implementation bug. Prints PRADYOS_RNG_OK, or
 * PRADYOS_RNG_STUB when the draws are identical. */
void     rng_selftest(void);

/* virtio-rng probe, called from the PCI scan in main.c. */
void     virtio_rng_init(uint8_t bus, uint8_t dev, uint8_t func);
