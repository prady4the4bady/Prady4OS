/* kernel/mm/swap.c — anonymous-memory swap, C2.1 P1a (DDR-1190 V1, A1.5,
 * A1.10, A1.11, A1.12).
 *
 * A swapped-out arena page is a NOT-present leaf that keeps every flag bit of
 * the page (PTE_SW_EVERW included, A1.5), carries PTE_SW_SWAP (bit 54), and
 * holds a slot index in its address field. The slot's sealed copy is the page
 * under ChaCha20-Poly1305 (aead_seal) with a per-boot key:
 *   nonce = 4 zero bytes || ctr (LE64), ctr from one global per-boot counter
 *           that is never reused;
 *   AAD   = slot (LE32) || ctr (LE64);
 *   ctr   is read back from the TRUSTED swap_ctr[slot], never from the store.
 * P1a's store is RAM (A1.11): one PMM frame per sealed copy. P1b puts a disk
 * behind the same calls (A1.12 point 10).
 *
 * Every entry point below runs with the mm lock of the address space held.
 * The store's own spinlock covers only the slot table and the counter;
 * sealing and opening run outside it.
 */
#include "swap.h"
#include "vmm.h"
#include "mm.h"
#include "pmm.h"
#include "tlb.h"
#include "sched.h"
#include "signal.h"
#include "spinlock.h"
#include "string.h"
#include "errno.h"
#include "console.h"
#include "aead.h"
#include "rng.h"
#include "syscall.h"
#include "uaccess.h"

#define SW_PRESENT  0x1ull
#define SW_PS       0x80ull
#define SW_ADDR     0x000FFFFFFFFFF000ull
#define SWAP_SLOTS  32u
#define SWAP_PG     4096u
#define SWAP_NCOUNT 32u

static spinlock_t g_swap_lk = SPINLOCK_INIT;
static uint8_t    g_key[AEAD_KEY_LEN];
static int        g_key_ok;
static uint64_t   g_wctr;                        /* last counter issued */
static uint64_t   swap_ctr[SWAP_SLOTS];          /* trusted; 0 = slot free */
static uint64_t   g_sealed[SWAP_SLOTS];          /* frame of the sealed copy */
static uint8_t    g_tag[SWAP_SLOTS][AEAD_TAG_LEN];
static uint8_t    g_failed[SWAP_SLOTS];          /* FAIL line printed once */
/* The store's copy of V1's on-disk metadata: the counter each sealed copy was
 * written under. Swap-in never reads it (it uses swap_ctr[]); it exists so a
 * replay restores it with the old copy (SNAP/RESTORE, A1.12 point 9). */
static uint64_t   g_dctr[SWAP_SLOTS];
static struct { uint64_t frame, dctr; uint32_t slot; uint8_t tag[AEAD_TAG_LEN]; int ok; } g_snap;
static uint32_t   g_live;                        /* swap entries in all page tables */

/* The probe's per-page swap-out counts (A1.12 point 9). */
static struct { uint64_t cr3, va; uint32_t n; } g_cnt[SWAP_NCOUNT];

static inline void sw_invlpg(uint64_t va) {
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

static uint64_t *sw_leaf(uint64_t cr3, uint64_t va) {
    uint64_t *t = (uint64_t *)(uintptr_t)(cr3 & SW_ADDR);
    for (int lvl = 4; lvl > 1; lvl--) {
        uint64_t e = t[(va >> (12 + 9 * (lvl - 1))) & 0x1FF];
        if (!(e & SW_PRESENT) || (e & SW_PS))
            return 0;
        t = (uint64_t *)(uintptr_t)(e & SW_ADDR);
    }
    return &t[(va >> 12) & 0x1FF];
}

static void swap_nonce(uint64_t ctr, uint8_t n[AEAD_NONCE_LEN]) {
    for (int i = 0; i < 4; i++) n[i] = 0;
    for (int i = 0; i < 8; i++) n[4 + i] = (uint8_t)(ctr >> (8 * i));
}

static void swap_aad(uint32_t slot, uint64_t ctr, uint8_t a[12]) {
    for (int i = 0; i < 4; i++) a[i]     = (uint8_t)(slot >> (8 * i));
    for (int i = 0; i < 8; i++) a[4 + i] = (uint8_t)(ctr >> (8 * i));
}

static int swap_key_init(void) {
    if (__atomic_load_n(&g_key_ok, __ATOMIC_ACQUIRE))
        return 0;
    uint8_t k[AEAD_KEY_LEN];
    if (rng_bytes(k, sizeof k) != 0)
        return -EIO;                             /* fails closed (DDR-816) */
    uint64_t fl = spin_lock_irqsave(&g_swap_lk);
    if (!g_key_ok) {
        memcpy(g_key, k, sizeof k);
        __atomic_store_n(&g_key_ok, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_swap_lk, fl);
    memset(k, 0, sizeof k);
    return 0;
}

static void slot_free(uint32_t slot) {
    if (slot >= SWAP_SLOTS)
        return;
    uint64_t fl = spin_lock_irqsave(&g_swap_lk);
    uint64_t f = g_sealed[slot];
    int was = swap_ctr[slot] != 0;
    swap_ctr[slot] = 0;                          /* V1: cleared when freed */
    g_sealed[slot] = 0;
    if (was && g_live)
        g_live--;
    spin_unlock_irqrestore(&g_swap_lk, fl);
    if (f)
        pmm_free_page(f);
}

static void count_note(uint64_t cr3, uint64_t va) {
    uint64_t fl = spin_lock_irqsave(&g_swap_lk);
    int free_i = -1;
    for (unsigned i = 0; i < SWAP_NCOUNT; i++) {
        if (g_cnt[i].cr3 == cr3 && g_cnt[i].va == va) {
            g_cnt[i].n++;
            spin_unlock_irqrestore(&g_swap_lk, fl);
            return;
        }
        if (free_i < 0 && !g_cnt[i].cr3)
            free_i = (int)i;
    }
    if (free_i >= 0) {
        g_cnt[free_i].cr3 = cr3;
        g_cnt[free_i].va  = va;
        g_cnt[free_i].n   = 1;
    }
    spin_unlock_irqrestore(&g_swap_lk, fl);
}

static uint32_t count_get(uint64_t cr3, uint64_t va) {
    uint32_t n = 0;
    uint64_t fl = spin_lock_irqsave(&g_swap_lk);
    for (unsigned i = 0; i < SWAP_NCOUNT; i++)
        if (g_cnt[i].cr3 == cr3 && g_cnt[i].va == va)
            n = g_cnt[i].n;
    spin_unlock_irqrestore(&g_swap_lk, fl);
    return n;
}

void swap_as_gone(uint64_t cr3) {
    cr3 &= SW_ADDR;
    uint64_t fl = spin_lock_irqsave(&g_swap_lk);
    for (unsigned i = 0; i < SWAP_NCOUNT; i++)
        if (g_cnt[i].cr3 == cr3)
            g_cnt[i].cr3 = g_cnt[i].va = g_cnt[i].n = 0;
    spin_unlock_irqrestore(&g_swap_lk, fl);
}

void swap_entry_release(uint64_t e) {
    if ((e & SW_PRESENT) || !(e & PTE_SW_SWAP))
        return;
    slot_free((uint32_t)((e & SW_ADDR) >> 12));
}

/* A1.12 point 2: candidate rule, pin check, slot + counter, PTE, shootdown,
 * seal, free. */
int swap_evict_locked(struct mm *mm, uint64_t va) {
    va &= ~0xFFFull;
    if (!mm || va < VMM_MMAP_BASE || va >= VMM_MMAP_TOP)
        return -EINVAL;                          /* A1.3: the arena only */
    uint64_t cr3 = mm->cr3 & SW_ADDR;
    uint64_t *pte = sw_leaf(cr3, va);
    if (!pte)
        return -EINVAL;
    uint64_t e = *pte;
    if (!(e & SW_PRESENT) || !(e & VMM_USER) ||
        (e & (PTE_SW_SHARED | PTE_SW_SHMEM | PTE_SW_COW | PTE_SW_DYING)))
        return -EINVAL;                          /* V2: never swapped */
    uint64_t frame = e & SW_ADDR;
    if (pmm_refcount_get(frame) != 1)
        return -EINVAL;                          /* shared after fork */
    if (__atomic_load_n(&mm->uaccess_active, __ATOMIC_ACQUIRE))
        return SWAP_SKIPPED_PINNED;              /* A1.10 point 2 */
    if (swap_key_init() < 0)
        return -EIO;
    uint64_t sealed = pmm_alloc_page();
    if (!sealed)
        return -ENOMEM;

    uint64_t fl = spin_lock_irqsave(&g_swap_lk);
    if (g_wctr == ~0ull) {                       /* V1: no wrap, ever */
        spin_unlock_irqrestore(&g_swap_lk, fl);
        pmm_free_page(sealed);
        return -EOVERFLOW;
    }
    uint32_t slot = SWAP_SLOTS;
    for (uint32_t i = 0; i < SWAP_SLOTS; i++)
        if (!swap_ctr[i]) { slot = i; break; }
    if (slot == SWAP_SLOTS) {
        spin_unlock_irqrestore(&g_swap_lk, fl);
        pmm_free_page(sealed);
        return -ENOSPC;
    }
    uint64_t ctr = ++g_wctr;
    swap_ctr[slot] = ctr;
    g_dctr[slot]   = ctr;
    g_sealed[slot] = sealed;
    g_failed[slot] = 0;
    g_live++;
    spin_unlock_irqrestore(&g_swap_lk, fl);

    /* The entry first: from here no CPU can write the page (A1.12 point 2). */
    *pte = (e & ~(SW_ADDR | SW_PRESENT)) | ((uint64_t)slot << 12) | PTE_SW_SWAP;
    sw_invlpg(va);
    tlb_shoot(cr3, va, 1);

    uint8_t nonce[AEAD_NONCE_LEN], aad[12];
    swap_nonce(ctr, nonce);
    swap_aad(slot, ctr, aad);
    aead_seal(g_key, nonce, aad, sizeof aad,
              (const void *)(uintptr_t)frame, SWAP_PG,
              (uint8_t *)(uintptr_t)sealed, g_tag[slot]);
    pmm_free_page(frame);
    count_note(cr3, va);
    return 0;
}

static void swap_fail_line(uint32_t slot) {
    if (slot < SWAP_SLOTS) {
        if (g_failed[slot])
            return;
        g_failed[slot] = 1;
    }
    struct tcb *t = current_thread;
    kputs("[swap] integrity FAIL pid=");
    kputdec(t ? t->pid : 0);
    kputs(" slot=");
    kputdec(slot);
    kputs("\r\n");
}

/* 1 swapped in, 0 not a swap entry, -EIO tag/slot failure, -ENOMEM no frame. */
int swap_in_locked(struct mm *mm, uint64_t va) {
    if (!mm)
        return 0;
    va &= ~0xFFFull;
    uint64_t *pte = sw_leaf(mm->cr3, va);
    if (!pte)
        return 0;
    uint64_t e = *pte;
    if ((e & SW_PRESENT) || !(e & PTE_SW_SWAP))
        return 0;
    uint32_t slot = (uint32_t)((e & SW_ADDR) >> 12);
    if (slot >= SWAP_SLOTS) {
        swap_fail_line(slot);
        return -EIO;
    }
    uint8_t tag[AEAD_TAG_LEN];
    uint64_t fl = spin_lock_irqsave(&g_swap_lk);
    uint64_t ctr = swap_ctr[slot];               /* the TRUSTED counter */
    uint64_t sealed = g_sealed[slot];
    memcpy(tag, g_tag[slot], sizeof tag);
    spin_unlock_irqrestore(&g_swap_lk, fl);
    if (!ctr || !sealed) {
        swap_fail_line(slot);
        return -EIO;
    }
    uint64_t frame = pmm_alloc_page();
    if (!frame)
        return -ENOMEM;                          /* V4: the OOM kill */
    uint8_t nonce[AEAD_NONCE_LEN], aad[12];
    swap_nonce(ctr, nonce);
    swap_aad(slot, ctr, aad);
    if (aead_open(g_key, nonce, aad, sizeof aad,
                  (const void *)(uintptr_t)sealed, SWAP_PG, tag,
                  (uint8_t *)(uintptr_t)frame) != 0) {
        pmm_free_page(frame);                    /* never written: tag first */
        swap_fail_line(slot);
        return -EIO;
    }
    /* A1.5: clear the marker, set present, the new frame; nothing else. */
    *pte = (e & ~(SW_ADDR | PTE_SW_SWAP)) | (frame & SW_ADDR) | SW_PRESENT;
    sw_invlpg(va);
    slot_free(slot);
    return 1;
}

int swap_prefault_locked(struct mm *mm, uint64_t va, uint64_t len, int writable) {
    (void)writable;
    if (!mm || !len || !__atomic_load_n(&g_live, __ATOMIC_ACQUIRE))
        return 0;
    if (va + len < va)
        return 0;                                /* the range check refuses it */
    for (uint64_t p = va & ~0xFFFull; p < va + len; p += SWAP_PG) {
        int r = swap_in_locked(mm, p);
        if (r < 0) {
            /* A1.10 point 4 / A1.12 point 7: uncatchable; the syscall
             * returns -EFAULT and the process ends at syscall exit. */
            sig_raise(current_thread, SIGKILL, 0, SI_KERNEL, 0);
            return r;
        }
    }
    return 0;
}

/* A1.6: fork's pre-pass. Every swap entry of the arena comes back in. */
int swap_in_all_locked(struct mm *mm) {
    if (!mm || !__atomic_load_n(&g_live, __ATOMIC_ACQUIRE))
        return 0;
    uint64_t cr3 = mm->cr3 & SW_ADDR;
    for (uint64_t va = VMM_MMAP_BASE; va < VMM_MMAP_TOP; ) {
        uint64_t *t4 = (uint64_t *)(uintptr_t)cr3;
        uint64_t e4 = t4[(va >> 39) & 0x1FF];
        if (!(e4 & SW_PRESENT) || (e4 & SW_PS)) { va = (va | ((1ull << 39) - 1)) + 1; continue; }
        uint64_t e3 = ((uint64_t *)(uintptr_t)(e4 & SW_ADDR))[(va >> 30) & 0x1FF];
        if (!(e3 & SW_PRESENT) || (e3 & SW_PS)) { va = (va | ((1ull << 30) - 1)) + 1; continue; }
        uint64_t e2 = ((uint64_t *)(uintptr_t)(e3 & SW_ADDR))[(va >> 21) & 0x1FF];
        if (!(e2 & SW_PRESENT) || (e2 & SW_PS)) { va = (va | ((1ull << 21) - 1)) + 1; continue; }
        int r = swap_in_locked(mm, va);
        if (r < 0) {
            if (r == -EIO)
                sig_raise(current_thread, SIGKILL, 0, SI_KERNEL, 0);
            return r;
        }
        va += SWAP_PG;
    }
    return 0;
}

/* ---- SYS_SWAP_PROBE (NSI 125, A1.12 point 9) ------------------------------ */
static long sys_swap_probe(long a_op, long a_va, long a_len, long a4, long a5, long a6) {
    (void)a4; (void)a5; (void)a6;
    struct tcb *t = current_thread;
    if (!t || !t->mm)
        return -EPERM;
    uint64_t va = (uint64_t)a_va & ~0xFFFull;
    long r = 0;
    switch (a_op) {
    case SWAP_PROBE_EVICT:
        mm_lock(t);
        r = swap_evict_locked(t->mm, va);
        mm_unlock(t);
        if (r == SWAP_SKIPPED_PINNED)
            kputs("[swap] evict skipped=pinned\r\n");
        return r;
    case SWAP_PROBE_PIN:
        return uaccess_pin((const void __user *)(uintptr_t)a_va, (size_t)a_len, 1);
    case SWAP_PROBE_UNPIN:
        uaccess_unpin();
        return 0;
    case SWAP_PROBE_CORRUPT: {
        mm_lock(t);
        uint64_t *pte = (va >= VMM_MMAP_BASE && va < VMM_MMAP_TOP) ? sw_leaf(t->mm->cr3, va) : 0;
        uint64_t e = pte ? *pte : 0;
        r = -EINVAL;
        if (!(e & SW_PRESENT) && (e & PTE_SW_SWAP)) {
            uint32_t slot = (uint32_t)((e & SW_ADDR) >> 12);
            uint64_t fl = spin_lock_irqsave(&g_swap_lk);
            if (slot < SWAP_SLOTS && g_sealed[slot]) {
                ((volatile uint8_t *)(uintptr_t)g_sealed[slot])[0] ^= 0x01;
                r = 0;
            }
            spin_unlock_irqrestore(&g_swap_lk, fl);
        }
        mm_unlock(t);
        return r;
    }
    case SWAP_PROBE_SNAP:
    case SWAP_PROBE_RESTORE: {
        uint64_t spare = 0;
        if (a_op == SWAP_PROBE_SNAP && !g_snap.frame && !(spare = pmm_alloc_page()))
            return -ENOMEM;
        mm_lock(t);
        uint64_t *pte = (va >= VMM_MMAP_BASE && va < VMM_MMAP_TOP) ? sw_leaf(t->mm->cr3, va) : 0;
        uint64_t e = pte ? *pte : 0;
        r = -EINVAL;
        if (!(e & SW_PRESENT) && (e & PTE_SW_SWAP)) {
            uint32_t slot = (uint32_t)((e & SW_ADDR) >> 12);
            uint64_t fl = spin_lock_irqsave(&g_swap_lk);
            if (spare && !g_snap.frame) { g_snap.frame = spare; spare = 0; }
            if (slot < SWAP_SLOTS && g_sealed[slot]) {
                void *live = (void *)(uintptr_t)g_sealed[slot];
                void *keep = (void *)(uintptr_t)g_snap.frame;
                if (a_op == SWAP_PROBE_SNAP) {
                    memcpy(keep, live, SWAP_PG);
                    memcpy(g_snap.tag, g_tag[slot], AEAD_TAG_LEN);
                    g_snap.dctr = g_dctr[slot];
                    g_snap.slot = slot;
                    g_snap.ok = 1;
                    r = 0;
                } else if (g_snap.ok && g_snap.slot != slot) {
                    r = -EAGAIN;                 /* a replay is into the SAME slot */
                } else if (g_snap.ok) {
                    memcpy(live, keep, SWAP_PG);
                    memcpy(g_tag[slot], g_snap.tag, AEAD_TAG_LEN);
                    g_dctr[slot] = g_snap.dctr;
                    r = 0;
                }
            }
            spin_unlock_irqrestore(&g_swap_lk, fl);
        }
        mm_unlock(t);
        if (spare)
            pmm_free_page(spare);
        return r;
    }
    case SWAP_PROBE_COUNT:
        return (long)count_get(t->mm->cr3 & SW_ADDR, va);
    case SWAP_PROBE_STATE: {
        mm_lock(t);
        uint64_t *pte = sw_leaf(t->mm->cr3, va);
        r = (pte && !(*pte & SW_PRESENT) && (*pte & PTE_SW_SWAP)) ? 1 : 0;
        mm_unlock(t);
        return r;
    }
    default:
        return -EINVAL;
    }
}

void swap_register(void) {
    syscall_register(SYS_SWAP_PROBE, sys_swap_probe);
}
