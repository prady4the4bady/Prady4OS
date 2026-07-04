/* kernel/proc/tss.c — per-CPU TSS setup + GDT TSS-descriptor patching
 * (Phase 2e; per-CPU under ADR-031 / DDR-SMP-3c-cap-1). */
#include "tss.h"
#include "string.h"
#include "percpu.h"

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* One TSS per CPU, indexed by cpu_idx (ADR-031 D2). */
static struct tss64 tss[PERCPU_MAX];

/* PERCPU_MAX back-to-back 16-byte TSS descriptors in the GDT (cpu.asm): two
 * 8-byte slots per CPU, CPU i at gdt64_tss[i*2 .. i*2+1]. */
extern uint64_t gdt64_tss[PERCPU_MAX * 2];

void tss_init_cpu(uint32_t cpu_idx, uint64_t rsp0) {
    if (cpu_idx >= PERCPU_MAX)
        return;
    struct tss64 *t = &tss[cpu_idx];
    memset(t, 0, sizeof(*t));
    t->rsp0 = rsp0;
    t->iomap_base = sizeof(struct tss64);        /* no I/O bitmap */

    uint64_t base = (uint64_t)(uintptr_t)t;
    uint32_t limit = sizeof(struct tss64) - 1;

    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= ((uint64_t)(base & 0xFFFFFF)) << 16;
    low |= (uint64_t)0x89 << 40;                 /* present, type=9 (avail 64-bit TSS) */
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;/* G=0, limit[19:16] */
    low |= ((uint64_t)((base >> 24) & 0xFF)) << 56;
    gdt64_tss[cpu_idx * 2 + 0] = low;
    gdt64_tss[cpu_idx * 2 + 1] = (base >> 32) & 0xFFFFFFFF;   /* base[63:32] */

    uint16_t sel = (uint16_t)(0x28 + cpu_idx * 0x10);
    __asm__ volatile("ltr %%ax" : : "a"(sel));
}

void tss_set_rsp0(uint64_t rsp0) {
    struct percpu *pc = this_cpu();
    tss[pc ? pc->cpu_idx : 0].rsp0 = rsp0;       /* this CPU's own TSS */
}
