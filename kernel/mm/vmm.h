/* kernel/vmm.h — NEXUS virtual memory manager (4-level paging), Phase 2b.
 *
 * The kernel owns its page tables: vmm_map/vmm_unmap walk the active 4-level
 * tables (rooted at CR3), allocating intermediate tables from the PMM as needed
 * and reclaiming emptied tables on unmap. Page-table frames are reached through
 * the low identity map (kept by the bootloader), so no physmap is needed yet.
 */
#pragma once
#include <stdint.h>

#define VMM_PRESENT 0x1ull
#define VMM_RW      0x2ull
#define VMM_USER    0x4ull
#define VMM_PWT     0x8ull                    /* page write-through            */
#define VMM_PCD     0x10ull                   /* page cache disable (for MMIO) */
#define VMM_NX      0x8000000000000000ull     /* no-execute; honored once EFER.NXE is on */

/* Software-available PTE bits (x86-64 bits 9-11, ignored by the MMU). Bit 10
 * marks a leaf whose physical frame is SHARED across address spaces (e.g. the
 * vDSO page): vmm_destroy_address_space must NOT free that frame, and fork must
 * share rather than copy it. (Bit 9 is reserved for the COW pass, IMP-D.) */
#define PTE_SW_COW    0x200ull                 /* bit 9: copy-on-write (IMP-D)   */
#define PTE_SW_SHARED 0x400ull                 /* bit 10 */

/* User virtual range = PML4 slot 1 = [512 GiB, 1 TiB) (ADR-021). The ELF loader
 * mirrors these as its own USER_MIN/USER_MAX; any user pointer the kernel copies
 * to/from must fall inside this range (see vmm_user_range_ok). */
#define VMM_USER_MIN  0x8000000000ull
#define VMM_USER_MAX  0x10000000000ull

/* Anonymous mmap arena: [544 GiB, 572 GiB), inside the user range and below the
 * 8 MiB user stack at 576 GiB (ADR-021). sys_mmap hands out RW+NX pages here. */
#define VMM_MMAP_BASE 0x8800000000ull
#define VMM_MMAP_TOP  0x8F00000000ull

/* Record the kernel master CR3 and enable EFER.NXE (so VMM_NX is honored).
 * Call once early in kmain, before any per-process address space is created. */
void vmm_init(void);

/* The kernel master page-table root (CR3 value for kernel-only threads). */
uint64_t vmm_kernel_cr3(void);

/* 1 if the CPU supports NX and EFER.NXE was enabled (so VMM_NX / W^X is live). */
int vmm_nx_enabled(void);

/* cap-4: EFER is per-CPU — arm NXE on the calling AP (reuses the BSP's probe). */
void vmm_enable_nxe_ap(void);

/* DDR-757: kernel-self W^X — re-stamp the kernel image mapping (text RX,
 * rodata R+NX, data/BSS RW+NX) + NX the identity alias. Call once after
 * vmm_init, before any user process. Prints the audit sentinel. */
void vmm_protect_kernel(void);

/* Map/unmap a single 4 KiB page in the ACTIVE address space. Returns 0 on
 * success, -1 on failure (huge page in the way, or out of frames). */
int vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
int vmm_unmap(uint64_t virt);

/* Create a new per-process address space: a fresh PML4 sharing the kernel's
 * top-level entries (low identity + higher-half) but with a private user range.
 * Returns the PML4 physical address (a valid CR3), or 0 on failure. */
uint64_t vmm_new_address_space(void);

/* Map a page into a SPECIFIC address space `pml4_phys` (need not be active). */
int vmm_map_in(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags);

/* ADR-038: user stack layout. Single definition — the ELF loader maps the top
 * page against these and the #PF growth path range-tests against them; two
 * copies would drift and silently disarm the guard page (DDR-822/825 class). */
#define USER_STACK_TOP   0x9000000000ull            /* 576 GiB                  */
#define USER_STACK_SIZE  (8ull * 1024 * 1024)       /* 8 MiB (ADR-021)          */
#define USER_STACK_BOT   (USER_STACK_TOP - USER_STACK_SIZE)

/* ADR-038 Option 3: pages eagerly mapped at the TOP of the stack; everything
 * below is demand-paged. This window exists because vmm_user_range_ok (ADR-022)
 * validates syscall pointers WITHOUT faulting, so a syscall buffer on an
 * untouched stack page would be rejected with -EFAULT.
 *
 * MEASURED, not guessed (ADR-038 Step 1-A, N=10 per gate per arm):
 *   Arm A (eager):     30/30 pass.  Arm B (map 1 page only): 0/30 pass.
 *   [stkdepth] fired on all 30 Arm-B runs: deepest not-present off=16384 = 4 pages.
 * W = 8 = the measured 4 pages doubled, as margin for syscall paths the three
 * co-gates do not exercise. Cost is 8 frames per process against the 2048 the
 * eager map used — the recovered-frame win is essentially unchanged. */
#define USER_STACK_EAGER_PAGES 8ull

/* ADR-038: resolve a not-present ring-3 fault inside the stack range by mapping
 * a zeroed page. Returns 0 if handled, -1 if the caller must treat it as a real
 * fault (out of range, or OOM). The guard page at [BOT-PAGE, BOT) is BELOW
 * USER_STACK_BOT and therefore never satisfied here — that is what keeps it a
 * guard rather than decoration. */
int vmm_stack_fault(uint64_t cr3, uint64_t fault_va);

/* Free a per-process address space: its private user page tables and the PML4.
 * The shared kernel entries are left intact. */
void vmm_destroy_address_space(uint64_t pml4_phys);

/* Validate a user range against the page tables rooted at `cr3` WITHOUT touching
 * the user memory: returns 1 iff every page spanned by [vaddr, vaddr+len) is
 * present, user-accessible, inside the user range, and (when `writable`) RW;
 * else 0. Never allocates and never faults — the foundation of copyin/copyout
 * (ADR-022). User mappings are 4 KiB; a huge-page (PS) entry on the path is
 * treated as not-walkable (fail-closed). */
int vmm_user_range_ok(uint64_t cr3, uint64_t vaddr, uint64_t len, int writable);

/* Resolve a virtual address to its mapped physical page base in the tables rooted
 * at `cr3` (4 KiB leaf), or 0 if not present. Used by sys_munmap to find the
 * frame to free after clearing the PTE. */
uint64_t vmm_resolve(uint64_t cr3, uint64_t virt);
