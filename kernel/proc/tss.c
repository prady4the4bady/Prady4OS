/* kernel/proc/tss.c — TSS setup + GDT TSS-descriptor patching (Phase 2e). */
#include "tss.h"
#include "string.h"

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static struct tss64 tss;

/* The two 8-byte slots reserved for the TSS descriptor in the GDT (cpu.asm). */
extern uint64_t gdt64_tss[2];

void tss_init(uint64_t rsp0) {
    memset(&tss, 0, sizeof(tss));
    tss.rsp0 = rsp0;
    tss.iomap_base = sizeof(struct tss64);   /* no I/O bitmap */

    uint64_t base = (uint64_t)(uintptr_t)&tss;
    uint32_t limit = sizeof(struct tss64) - 1;

    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= ((uint64_t)(base & 0xFFFFFF)) << 16;
    low |= (uint64_t)0x89 << 40;                 /* present, type=9 (avail 64-bit TSS) */
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;/* G=0, limit[19:16] */
    low |= ((uint64_t)((base >> 24) & 0xFF)) << 56;
    gdt64_tss[0] = low;
    gdt64_tss[1] = (base >> 32) & 0xFFFFFFFF;     /* base[63:32] */

    __asm__ volatile("ltr %%ax" : : "a"((uint16_t)0x28));
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
