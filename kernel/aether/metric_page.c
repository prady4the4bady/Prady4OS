/* kernel/aether/metric_page.c — F#68 sealed objective-function root (DDR-795). */
#include "metric_page.h"
#include "pmm.h"          /* pmm_alloc_page, PAGE_SIZE */
#include "vmm.h"          /* vmm_map_in, VMM_USER/VMM_NX, PTE_SW_SHARED */
#include "string.h"       /* memset */
#include "console.h"      /* kputs (panic path) */
#include "irq.h"          /* g_ticks */

volatile metric_page_t *metric_page;   /* NULL until metric_page_init */
uint64_t metric_page_phys;

void metric_page_init(void) {
    uint64_t frame = pmm_alloc_page();
    if (!frame) {
        kputs("\r\n*** NEXUS PANIC: metric_page_init out of memory ***\r\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);   /* clears KASAN poison too */
    metric_page_phys = frame;
    metric_page = (volatile metric_page_t *)(uintptr_t)frame;  /* identity = kernel RW */

    /* Stamp the header immediately. A page that is mapped but unstamped reads as
     * magic=0, which a verifier could mistake for "no objective function set"
     * rather than "the region is not initialised" — two very different states. */
    metric_page->magic   = METRIC_MAGIC;
    metric_page->version = METRIC_VERSION;
    metric_page->flags   = 0;                 /* not sealed until seal() runs */
}

void metric_page_map_user(uint64_t cr3) {
    if (!metric_page_phys)
        return;
    /* Read-only (no VMM_RW), non-executable (VMM_NX), user, SW_SHARED so the
     * teardown/fork paths share this one frame rather than freeing or copying
     * it. map_core adds PRESENT. A ring-3 store therefore takes #PF and
     * idt.c's user-fault path kills the process cleanly (ADR-021). */
    vmm_map_in(cr3, METRIC_USER_VA, metric_page_phys,
               VMM_USER | VMM_NX | PTE_SW_SHARED);
}

void metric_page_seal(const uint8_t *root, uint32_t entries) {
    if (!metric_page || !root)
        return;
    for (uint32_t i = 0; i < METRIC_ROOT_LEN; i++)
        metric_page->root[i] = root[i];
    metric_page->entries    = entries;
    metric_page->sealed_ts  = g_ticks;
    /* generation LAST, after the payload is in place: a reader that samples
     * mid-update must not see a new generation attached to an old root. */
    metric_page->generation = metric_page->generation + 1;
    metric_page->flags      = METRIC_FLAG_SEALED;
}
