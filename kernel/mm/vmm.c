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
        if (!(e & PTE_PRESENT) || !(e & VMM_USER)) return 0;
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
