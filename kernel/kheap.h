/* kernel/kheap.h — NEXUS kernel heap (slab + general kmalloc), Phase 2b.
 *
 * Built on the buddy PMM over the identity map (no new paging). Provides:
 *   - kmalloc/kfree for arbitrary sizes (size-class slabs; >2 KiB -> whole pages)
 *   - dedicated slab caches for common kernel objects (PCB, capability token,
 *     IPC message) and a page-sized page-table-node allocator
 *   - debug poisoning + double-free detection + leak accounting (KHEAP_DEBUG)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

void  kheap_init(void);

void *kmalloc(size_t size);
void  kfree(void *ptr);

/* Dedicated object pools (provisional fixed sizes until the real structs land). */
void *pcb_alloc(void);      void pcb_free(void *p);     /* process control block  */
void *cap_alloc(void);      void cap_free(void *p);     /* 128-bit capability tok  */
void *ipc_alloc(void);      void ipc_free(void *p);     /* IPC message             */
void *ptnode_alloc(void);   void ptnode_free(void *p);  /* zeroed 4 KiB page table */

/* Total outstanding allocations across all pools (0 == no leaks). */
uint64_t kheap_outstanding(void);
