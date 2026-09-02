/* kernel/vmm.c — kernel-owned 4-level paging (Phase 2b).
 *
 * Tables are reached via the low identity map (physical addr == virtual addr for
 * the low 1 GiB), so a table's physical address doubles as a usable pointer.
 * Intermediate tables are allocated as zeroed pages via ptnode_alloc and freed
 * via ptnode_free, keeping the heap's leak accounting consistent.
 */
#include "vmm.h"
#include "pmm.h"
#include "kheap.h"
#include "console.h"           /* kputs for the DDR-757 W^X audit sentinel */

#define PTE_PRESENT  0x1ull
#define PTE_PS       0x80ull
#define PTE_ADDR     0x000FFFFFFFFFF000ull
#define MSR_EFER     0xC0000080u

static uint64_t g_kernel_pml4;          /* master kernel page-table root */
static int      g_nx_ok;                /* CPU supports NX (CPUID 8000_0001h:EDX[20]) */

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void invlpg(uint64_t v) {
    __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
}

void vmm_init(void) {
    g_kernel_pml4 = read_cr3() & PTE_ADDR;

    /* W^X relies on the NX bit, which is only architectural when CPUID advertises
     * it (AMD64 CPUID 8000_0001h, EDX[20]). Enabling EFER.NXE on a CPU without NX
     * would #GP, and setting PTE bit 63 while NXE is clear faults with a
     * reserved-bit error — so gate both on this probe. The long-mode entry
     * guarantees leaf 8000_0001h exists; QEMU's default qemu64 advertises NX. */
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001u));
    (void)eax; (void)ebx; (void)ecx;           /* cpuid writes all four; only EDX read */
    g_nx_ok = (int)((edx >> 20) & 1u);

    if (g_nx_ok) {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_EFER));
        uint64_t efer = ((uint64_t)hi << 32) | lo;
        efer |= (1ull << 11);                  /* EFER.NXE */
        __asm__ volatile("wrmsr" : : "c"(MSR_EFER),
                         "a"((uint32_t)efer), "d"((uint32_t)(efer >> 32)));
    }
}

int vmm_nx_enabled(void) { return g_nx_ok; }

/* ADR-031 cap-4: EFER is PER-CPU — an AP without NXE treats PTE bit 63 as
 * reserved and every W^X-marked (NX) user page faults with a RSVD-bit #PF.
 * Reuses the BSP's CPUID probe (g_nx_ok; identical cores). Call on each AP. */
void vmm_enable_nxe_ap(void) {
    if (!g_nx_ok)
        return;
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_EFER));
    uint64_t efer = ((uint64_t)hi << 32) | lo;
    efer |= (1ull << 11);                  /* EFER.NXE */
    __asm__ volatile("wrmsr" : : "c"(MSR_EFER),
                     "a"((uint32_t)efer), "d"((uint32_t)(efer >> 32)));
}

uint64_t vmm_kernel_cr3(void) {
    return g_kernel_pml4 ? g_kernel_pml4 : (read_cr3() & PTE_ADDR);
}

/* ---- DDR-757: kernel-self W^X ---------------------------------------------
 * Stage2 maps the kernel image (KERNEL_VBASE -> 0x400000, 4 KiB PT_HI pages,
 * 2 MiB span) present+RW with no NX. Re-stamp each PTE by section: text RX,
 * rodata R+NX, data/BSS/spare RW+NX; and NX the 2 MiB identity-map alias PDE at
 * 0x400000 (execute-via-alias). Per-process ASes share the kernel top-level
 * entries, so this single pass hardens every AS. NX bits gate on g_nx_ok
 * (identical to user W^X); text RW-clearing is unconditional. */
#define KERNEL_VBASE_WX 0xFFFFFFFF80000000ull
extern char __text_end[], __rodata_end[];      /* page-aligned (kernel.ld) */
static uint64_t *table_at(uint64_t phys);      /* defined below */

void vmm_protect_kernel(void) {
    /* Master walk: PML4[511] -> PDPT[510] -> PD[0] -> PT (the PT_HI page). */
    uint64_t *pml4 = table_at(g_kernel_pml4);
    uint64_t pdpt = pml4[511] & PTE_ADDR;
    if (!pdpt) { kputs("[wx] kernel W^X FAIL: no PML4[511]\r\n"); return; }
    uint64_t pd = table_at(pdpt)[510] & PTE_ADDR;
    if (!pd)   { kputs("[wx] kernel W^X FAIL: no PDPT[510]\r\n"); return; }
    uint64_t pt = table_at(pd)[0] & PTE_ADDR;
    if (!pt)   { kputs("[wx] kernel W^X FAIL: no PD[0]\r\n"); return; }

    uint64_t alias_pte  = 0;   /* DDR-1046: the identity-alias PD entry, read back */
    uint64_t *ptes      = table_at(pt);
    uint64_t text_end   = (uint64_t)(uintptr_t)__text_end;
    uint64_t rodata_end = (uint64_t)(uintptr_t)__rodata_end;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = ptes[i];
        if (!(e & PTE_PRESENT))
            continue;
        uint64_t va = KERNEL_VBASE_WX + (uint64_t)i * 4096ull;
        if (va < text_end) {
            e &= ~VMM_RW;                       /* text: RX */
        } else if (va < rodata_end) {
            e &= ~VMM_RW;                       /* rodata: R, no-exec */
            if (g_nx_ok) e |= VMM_NX;
        } else {
            if (g_nx_ok) e |= VMM_NX;           /* data/BSS/spare: RW, no-exec */
        }
        ptes[i] = e;
    }

    /* DDR-1046 — THE IDENTITY ALIAS, and the residue DDR-757 documented.
     *
     * The kernel image is mapped TWICE: in the higher half (protected above,
     * text RX / rodata R+NX / data RW+NX) and identity-mapped at 0x400000 by a
     * single 2 MiB PD entry that stage2.asm builds as 0x83 = PRESENT|RW|PS.
     * DDR-757 set NX here, killing execute-via-alias, and left RW — its own
     * comment said so: "RW kept (documented residue)".
     *
     * SO KERNEL TEXT WAS WRITABLE THROUGH THE ALIAS. W^X was half-enforced: a
     * stray write through a physical address could patch kernel code, and the
     * audit below could not see it, because it only walks the higher-half PTEs.
     *
     * MEASURED BEFORE CHANGING IT, not assumed safe: with RW cleared the boot
     * is line-for-line normal (423 lines, steady state at t=14500, no fault),
     * so nothing writes the kernel image through a physical address. Two facts
     * make that unsurprising and both were checked rather than reasoned:
     * PMM_MIN_PHYS is 16 MiB so no allocated frame lives in this 2 MiB page,
     * and the page tables sit at 0x300000 — PD entry 1, a different page — so
     * table_at()'s identity access is untouched.
     *
     * The readback is not decoration. "I cleared the bit" and "the bit is
     * clear" are different claims, and a measurement showing only that nothing
     * crashed cannot tell them apart. */
    {
        uint64_t lo_pdpt = pml4[0] & PTE_ADDR;
        if (lo_pdpt) {
            uint64_t lo_pd = table_at(lo_pdpt)[0] & PTE_ADDR;
            if (lo_pd && (table_at(lo_pd)[2] & PTE_PRESENT)) {
                if (g_nx_ok)
                    table_at(lo_pd)[2] |= VMM_NX;      /* no execute-via-alias */
                table_at(lo_pd)[2] &= ~VMM_RW;         /* no write-via-alias   */
                alias_pte = table_at(lo_pd)[2];        /* read BACK, audited below */
            }
        }
    }

    /* Full TLB flush (non-global entries), then audit what is actually live. */
    __asm__ volatile("mov %0, %%cr3" : : "r"(g_kernel_pml4) : "memory");

    kputs("PRADYOS_WX_ALIAS present=");
    kputdec((alias_pte & PTE_PRESENT) ? 1u : 0u);
    kputs(" rw=");
    kputdec((alias_pte & VMM_RW) ? 1u : 0u);
    kputs(" nx=");
    kputdec((alias_pte & VMM_NX) ? 1u : 0u);
    kputs("\r\n");

    int ok = 1;
    /* DDR-1046: the alias is part of W^X and is audited with everything else.
     * Without this the verdict said OK on a kernel whose text was writable
     * through the alias — which is exactly how the residue survived. */
    if (!(alias_pte & PTE_PRESENT))      ok = 0;   /* the alias must still exist */
    if (alias_pte & VMM_RW)              ok = 0;   /* writable kernel image      */
    if (g_nx_ok && !(alias_pte & VMM_NX)) ok = 0;  /* executable kernel image    */
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = ptes[i];
        if (!(e & PTE_PRESENT))
            continue;
        uint64_t va = KERNEL_VBASE_WX + (uint64_t)i * 4096ull;
        if (va < text_end) {
            if (e & VMM_RW) ok = 0;             /* writable text = FAIL */
        } else {
            if (g_nx_ok && !(e & VMM_NX)) ok = 0;  /* executable non-text = FAIL */
            if (va < rodata_end && (e & VMM_RW)) ok = 0;   /* writable rodata */
        }
    }
    kputs(ok ? "[wx] kernel W^X OK\r\n" : "[wx] kernel W^X FAIL\r\n");
}

/* A table's physical address is directly usable through the identity map. */
static uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)(uintptr_t)phys;
}

static unsigned idx(uint64_t virt, unsigned level) {  /* level: 4=PML4 .. 1=PT */
    return (unsigned)((virt >> (12 + 9 * (level - 1))) & 0x1FF);
}

/* Return the next-level table's physical address, creating it if `create`.
 * When `user`, ensure this intermediate entry (existing OR new) is user-walkable:
 * the CPU ANDs the user bit down the whole walk, so even a bootloader-made
 * top-level entry must carry it. Actual access is still gated by the leaf's
 * user bit, so a kernel-only leaf under a user-walkable path stays kernel-only. */
static uint64_t descend(uint64_t *table, unsigned i, int create, int user) {
    uint64_t e = table[i];
    if (e & PTE_PRESENT) {
        if (e & PTE_PS)
            return 0;                 /* huge page here; cannot descend */
        if (user && !(e & VMM_USER))
            table[i] = e | VMM_USER;  /* promote existing entry to user-walkable */
        return e & PTE_ADDR;
    }
    if (!create)
        return 0;
    void *frame = ptnode_alloc();     /* zeroed page from the PMM */
    if (!frame)
        return 0;
    uint64_t fphys = (uint64_t)(uintptr_t)frame;
    table[i] = fphys | PTE_PRESENT | VMM_RW | (user ? VMM_USER : 0);
    return fphys;
}

static int table_empty(const uint64_t *t) {
    for (unsigned i = 0; i < 512; i++) {
        if (t[i] != 0)
            return 0;
    }
    return 1;
}

/* Core mapper: install a leaf PTE into the tables rooted at `pml4_phys`.
 * Low 12 flag bits (RW/USER/PWT/PCD/...) and the NX bit (63) are both honored;
 * everything else is derived. Intermediate tables never carry NX, so an X leaf
 * under a user-walkable path can still execute. */
static int map_core(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = table_at(pml4_phys);
    int user = (flags & VMM_USER) ? 1 : 0;

    uint64_t pdpt = descend(pml4, idx(virt, 4), 1, user);
    if (!pdpt) return -1;
    uint64_t pd = descend(table_at(pdpt), idx(virt, 3), 1, user);
    if (!pd) return -1;
    uint64_t pt = descend(table_at(pd), idx(virt, 2), 1, user);
    if (!pt) return -1;

    uint64_t nx = g_nx_ok ? (flags & VMM_NX) : 0;   /* drop NX if unsupported */
    table_at(pt)[idx(virt, 1)] =
        (phys & PTE_ADDR) | (flags & 0xFFF) | nx | PTE_PRESENT;
    invlpg(virt);
    return 0;
}

int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    return map_core(read_cr3() & PTE_ADDR, virt, phys, flags);
}

int vmm_map_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags) {
    return map_core(pml4_phys & PTE_ADDR, virt, phys, flags);
}

/* DDR-1031: locate the leaf PTE for `virt` without creating anything. Returns a
 * pointer into the page table, or 0 if any level is absent or is a large page. */
static uint64_t *leaf_pte(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pml4 = table_at(pml4_phys);
    uint64_t pdpt_p = descend(pml4, idx(virt, 4), 0, 0);
    if (!pdpt_p) return 0;
    uint64_t pd_p = descend(table_at(pdpt_p), idx(virt, 3), 0, 0);
    if (!pd_p) return 0;
    uint64_t pt_p = descend(table_at(pd_p), idx(virt, 2), 0, 0);
    if (!pt_p) return 0;
    return &table_at(pt_p)[idx(virt, 1)];
}

int vmm_protect_range(uint64_t pml4_phys, uint64_t va, uint64_t len, uint64_t flags) {
    pml4_phys &= PTE_ADDR;
    uint64_t start = va & ~0xFFFull;
    uint64_t end   = (va + len + 0xFFFull) & ~0xFFFull;

    /* PASS 1 -- validate the WHOLE range before touching anything. A half-applied
     * protection change is worse than a rejected one: the caller has no way to
     * discover where it stopped, and POSIX callers do not expect to unwind it. */
    for (uint64_t p = start; p < end; p += 0x1000ull) {
        uint64_t *pte = leaf_pte(pml4_phys, p);
        if (!pte) return -1;
        uint64_t e = *pte;
        if (!(e & PTE_PRESENT) || !(e & VMM_USER))
            return -1;
        /* DDR-1031 §3b: the hardware RO bit IS copy-on-write's trigger. Granting
         * write on a COW page would not make it writable -- it would let this
         * process write a frame another still shares, with no copy and no fault.
         * Removing write from a COW page is harmless and stays allowed. */
        if ((flags & VMM_RW) && (e & PTE_SW_COW))
            return -2;
    }

    /* PASS 2 -- apply. The frame, both SOFTWARE bits and the cache attributes are
     * carried over verbatim; only the permission bits are replaced. Rebuilding
     * the PTE as `frame | flags` would clear PTE_SW_SHARED (breaking DDR-1003's
     * invariant) and PTE_SW_COW (making vmm_cow_fault return early at
     * vmm_cow.c:115, so the page is never copied). */
    uint64_t nx = g_nx_ok ? (flags & VMM_NX) : 0;
    for (uint64_t p = start; p < end; p += 0x1000ull) {
        uint64_t *pte = leaf_pte(pml4_phys, p);
        uint64_t e = *pte;
        *pte = (e & PTE_ADDR)
             | (e & (PTE_SW_COW | PTE_SW_SHARED | VMM_PWT | VMM_PCD))
             | PTE_PRESENT | VMM_USER
             | (flags & VMM_RW)
             | nx;
        invlpg(p);
    }
    return 0;
}

int vmm_unmap(uint64_t virt) {
    uint64_t *pml4 = table_at(read_cr3() & PTE_ADDR);

    uint64_t pdpt_p = descend(pml4, idx(virt, 4), 0, 0);
    if (!pdpt_p) return -1;
    uint64_t *pdpt = table_at(pdpt_p);
    uint64_t pd_p = descend(pdpt, idx(virt, 3), 0, 0);
    if (!pd_p) return -1;
    uint64_t *pd = table_at(pd_p);
    uint64_t pt_p = descend(pd, idx(virt, 2), 0, 0);
    if (!pt_p) return -1;
    uint64_t *pt = table_at(pt_p);

    pt[idx(virt, 1)] = 0;
    invlpg(virt);

    /* Reclaim now-empty tables bottom-up: each parent's entry is at the index
     * for the child's level (PD entry -> level 2, PDPT entry -> 3, PML4 -> 4). */
    if (table_empty(pt)) {
        ptnode_free(pt);
        pd[idx(virt, 2)] = 0;
        if (table_empty(pd)) {
            ptnode_free(pd);
            pdpt[idx(virt, 3)] = 0;
            if (table_empty(pdpt)) {
                ptnode_free(pdpt);
                pml4[idx(virt, 4)] = 0;
            }
        }
    }
    return 0;
}

uint64_t vmm_new_address_space(void) {
    void *frame = ptnode_alloc();         /* zeroed PML4 */
    if (!frame)
        return 0;
    uint64_t pml4 = (uint64_t)(uintptr_t)frame;
    uint64_t *np = table_at(pml4);
    uint64_t *kp = table_at(vmm_kernel_cr3());
    /* Share every existing kernel top-level entry (low identity, MMIO, higher
     * half). They point at the kernel's own PDPTs, so later kernel mappings stay
     * coherent. The user range (PML4 slot 1) is empty in the kernel master, so
     * each process gets a private subtree there. */
    for (unsigned i = 0; i < 512; i++)
        np[i] = kp[i];
    return pml4;
}

/* Recursively free a private page-table subtree. level: 3=PDPT,2=PD,1=PT.
 * Leaf (PT) entries point at user data pages (also ptnode-allocated). */
static void free_subtree(uint64_t table_phys, int level) {
    uint64_t *t = table_at(table_phys);
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_PRESENT) || (e & PTE_PS))
            continue;
        if (level > 1)
            free_subtree(e & PTE_ADDR, level - 1);
        else if (!(e & PTE_SW_SHARED))
            ptnode_free((void *)(uintptr_t)(e & PTE_ADDR));   /* user data page (skip shared, e.g. vDSO) */
    }
    ptnode_free((void *)(uintptr_t)table_phys);
}

void vmm_destroy_address_space(uint64_t pml4_phys) {
    pml4_phys &= PTE_ADDR;
    uint64_t kcr3 = vmm_kernel_cr3();
    if (!pml4_phys || pml4_phys == kcr3)
        return;
    uint64_t *np = table_at(pml4_phys);
    uint64_t *kp = table_at(kcr3);
    /* Free only the slots that differ from the kernel master — the private user
     * range. Shared kernel entries (np[i] == kp[i]) are left intact. */
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = np[i];
        if ((e & PTE_PRESENT) && e != kp[i])
            free_subtree(e & PTE_ADDR, 3);
    }
    ptnode_free((void *)(uintptr_t)pml4_phys);
}

/* Walk the tables rooted at `cr3` and confirm every page spanned by
 * [vaddr, vaddr+len) is mapped USER (and RW when `writable`), inside the user
 * range. Reads PTEs through the identity map; never dereferences the user
 * address, so a bad pointer yields 0 instead of a #PF (ADR-022). USER is checked
 * at every level (the user subtree lives entirely in PML4 slot 1, where the
 * kernel master has nothing, so all intermediates are user-promoted) — defense
 * in depth against a kernel-only page reachable under a user-walkable path. */
/* ADR-038 Step 1-A: how deep into the user stack do syscall buffers actually
 * reach? vmm_user_range_ok's contract is "never allocates and never faults"
 * (ADR-022), so a demand-paged stack makes it reject pointers into untouched
 * stack pages — that is the regression ADR-038's Option 1 caused. This records
 * the DEEPEST offset below USER_STACK_TOP at which a not-present page was seen,
 * which sizes the eagerly-mapped window W for Option 3.
 *
 * Prints only when a NEW maximum is seen — a handful of lines per boot. Volume
 * discipline is deliberate: an earlier instrument (cur= in the timer ISR) was
 * heavy enough to move the failure rate it was measuring. */
static volatile uint64_t g_stk_np_deepest;   /* bytes below USER_STACK_TOP */

static void stk_note_not_present(uint64_t va) {
    if (va < USER_STACK_BOT || va >= USER_STACK_TOP)
        return;                              /* not a stack address */
    uint64_t off = USER_STACK_TOP - va;
    if (off <= __atomic_load_n(&g_stk_np_deepest, __ATOMIC_RELAXED))
        return;                              /* not a new maximum */
    __atomic_store_n(&g_stk_np_deepest, off, __ATOMIC_RELAXED);
    kputs("[stkdepth] not-present at off=");
    kputdec(off);
    kputs(" pages=");
    kputdec((off + PAGE_SIZE - 1) / PAGE_SIZE);
    kputs("\r\n");
}

uint64_t vmm_stack_np_deepest(void) {
    return __atomic_load_n(&g_stk_np_deepest, __ATOMIC_RELAXED);
}

int vmm_user_range_ok(uint64_t cr3, uint64_t vaddr, uint64_t len, int writable) {
    if (len == 0)
        return 1;
    uint64_t end = vaddr + len;
    if (end < vaddr)                                  /* address wrap */
        return 0;
    if (vaddr < VMM_USER_MIN || end > VMM_USER_MAX)
        return 0;

    uint64_t *pml4 = table_at(cr3 & PTE_ADDR);
    for (uint64_t va = vaddr & ~((uint64_t)PAGE_SIZE - 1); va < end; va += PAGE_SIZE) {
        uint64_t e = pml4[idx(va, 4)];
        if (!(e & PTE_PRESENT) || (e & PTE_PS) || !(e & VMM_USER)) return 0;
        e = table_at(e & PTE_ADDR)[idx(va, 3)];
        if (!(e & PTE_PRESENT) || (e & PTE_PS) || !(e & VMM_USER)) return 0;
        e = table_at(e & PTE_ADDR)[idx(va, 2)];
        if (!(e & PTE_PRESENT) || (e & PTE_PS) || !(e & VMM_USER)) return 0;
        e = table_at(e & PTE_ADDR)[idx(va, 1)];       /* leaf PTE (4 KiB) */
        if (!(e & PTE_PRESENT) || !(e & VMM_USER)) {
            if (!(e & PTE_PRESENT))
                stk_note_not_present(va);             /* ADR-038 Step 1-A */
            return 0;
        }
        if (writable && !(e & VMM_RW)) return 0;
    }
    return 1;
}

uint64_t vmm_resolve(uint64_t cr3, uint64_t virt) {
    uint64_t *pml4 = table_at(cr3 & PTE_ADDR);
    uint64_t e = pml4[idx(virt, 4)];
    if (!(e & PTE_PRESENT) || (e & PTE_PS)) return 0;
    e = table_at(e & PTE_ADDR)[idx(virt, 3)];
    if (!(e & PTE_PRESENT) || (e & PTE_PS)) return 0;
    e = table_at(e & PTE_ADDR)[idx(virt, 2)];
    if (!(e & PTE_PRESENT) || (e & PTE_PS)) return 0;
    e = table_at(e & PTE_ADDR)[idx(virt, 1)];
    if (!(e & PTE_PRESENT)) return 0;
    return e & PTE_ADDR;
}
