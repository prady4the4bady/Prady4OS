/* kernel/vdso/vdso_page.c — vDSO shared timekeeping frame (IMP-C). */
#include "vdso_page.h"
#include "pmm.h"          /* pmm_alloc_page, PAGE_SIZE */
#include "vmm.h"          /* vmm_map_in, VMM_USER/VMM_NX, PTE_SW_SHARED */
#include "string.h"       /* memset */
#include "console.h"      /* kputs (panic path) */

volatile vdso_data_t *vdso_data;     /* NULL until vdso_init (PIT handler guards on it) */
uint64_t vdso_page_phys;

void vdso_init(void) {
    uint64_t frame = pmm_alloc_page();
    if (!frame) {
        kputs("\r\n*** NEXUS PANIC: vdso_init out of memory ***\r\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }
    memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);   /* clears the KASAN poison too */
    vdso_page_phys = frame;
    vdso_data = (volatile vdso_data_t *)(uintptr_t)frame;   /* identity map = kernel RW */
}

void vdso_map_user(uint64_t cr3) {
    if (!vdso_page_phys)
        return;
    /* Read-only (no VMM_RW), non-executable (VMM_NX), user, and SW_SHARED so the
     * address-space teardown / fork paths share this one frame instead of freeing
     * or copying it. map_core adds PRESENT. */
    vmm_map_in(cr3, VDSO_USER_VA, vdso_page_phys,
               VMM_USER | VMM_NX | PTE_SW_SHARED);
}
