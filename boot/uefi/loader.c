/* boot/uefi/loader.c — PRADYOS UEFI loader (Group 4 item 22, DDR-886).
 *
 * A second implementation of the SAME handoff contract as boot/stage2/stage2.asm:
 *
 *   RDI        = physical &boot_info  (0x4000)
 *   kernel.bin loaded at physical 0x400000
 *   paging     0xFFFFFFFF80000000 -> 0x400000, plus a low identity map
 *   entry      0xFFFFFFFF80000000, long mode, interrupts off
 *
 * Anything that differs is a divergence between boot paths, and a kernel that
 * boots one way but not the other fails far from the loader that caused it.
 */
#include "efi.h"

#define BOOT_INFO_PHYS 0x4000ull
#define BOOT_MAGIC     0x59445250u          /* 'PRDY' */
#define KERNEL_LMA     0x400000ull
#define KERNEL_VBASE   0xFFFFFFFF80000000ull
#define PAGE_SIZE      4096ull

/* Mirror of struct e820_entry / struct boot_info (kernel/boot_info.h). Declared
 * again rather than included: this file is compiled for a different target
 * (PE32+/Windows ABI) and must not pull the kernel's headers. The layouts are
 * pinned by _Static_assert below so a drift is a build failure, not a silent
 * handoff corruption. */
struct e820_entry { uint64_t base, len; uint32_t type, acpi; };
struct boot_info {
    uint32_t magic, e820_count;
    char     cpu_vendor[16];
    uint32_t long_mode, reserved;
    struct e820_entry e820[];
};
_Static_assert(sizeof(struct e820_entry) == 24, "e820 entry must stay 24 bytes");
_Static_assert(sizeof(struct boot_info) == 32,  "boot_info header must stay 32 bytes");

static EFI_SYSTEM_TABLE *ST;

static void print(CHAR16 *s) {
    /* Never called after ExitBootServices — see DDR-886 §3.2. */
    if (ST && ST->con_out)
        ST->con_out->output_string(ST->con_out, s);
}

static void die(CHAR16 *msg) {
    print(u"[uefi] FATAL ");
    print(msg);
    print(u"\r\n");
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* ---- page tables -------------------------------------------------------
 * UEFI's own identity map cannot be reused: the kernel runs at
 * 0xFFFFFFFF80000000, which the firmware does not map (DDR-886 §3.3).
 */
#define PTE_P  (1ull << 0)
#define PTE_RW (1ull << 1)
#define PTE_PS (1ull << 7)

static uint64_t alloc_page_zeroed(EFI_BOOT_SERVICES *bs) {
    uint64_t p = 0;
    if (bs->allocate_pages(AllocateAnyPages, EfiLoaderData, 1, &p) != EFI_SUCCESS)
        die(u"page alloc");
    for (uint64_t i = 0; i < PAGE_SIZE / 8; i++)
        ((uint64_t *)p)[i] = 0;
    return p;
}

static uint64_t build_page_tables(EFI_BOOT_SERVICES *bs) {
    uint64_t pml4 = alloc_page_zeroed(bs);

    /* Low identity map, 0..1 GiB with 2 MiB pages. The kernel keeps a low
     * identity map so the PMM can dereference physical frames directly. */
    uint64_t pdpt_lo = alloc_page_zeroed(bs);
    uint64_t pd_lo   = alloc_page_zeroed(bs);
    ((uint64_t *)pml4)[0]    = pdpt_lo | PTE_P | PTE_RW;
    ((uint64_t *)pdpt_lo)[0] = pd_lo   | PTE_P | PTE_RW;
    for (uint64_t i = 0; i < 512; i++)
        ((uint64_t *)pd_lo)[i] = (i * 0x200000ull) | PTE_P | PTE_RW | PTE_PS;

    /* Higher half: 0xFFFFFFFF80000000 -> 0x400000, a 2 MiB span in 4 KiB pages,
     * matching stage2's PT_HI exactly. 4 KiB (not 2 MiB) because the kernel's
     * own VMM later re-maps sub-ranges with different permissions (W^X). */
    uint64_t pml4i = (KERNEL_VBASE >> 39) & 0x1FF;
    uint64_t pdpti = (KERNEL_VBASE >> 30) & 0x1FF;
    uint64_t pdi   = (KERNEL_VBASE >> 21) & 0x1FF;
    uint64_t pdpt_hi = alloc_page_zeroed(bs);
    uint64_t pd_hi   = alloc_page_zeroed(bs);
    uint64_t pt_hi   = alloc_page_zeroed(bs);
    ((uint64_t *)pml4)[pml4i]      = pdpt_hi | PTE_P | PTE_RW;
    ((uint64_t *)pdpt_hi)[pdpti]   = pd_hi   | PTE_P | PTE_RW;
    ((uint64_t *)pd_hi)[pdi]       = pt_hi   | PTE_P | PTE_RW;
    for (uint64_t i = 0; i < 512; i++)
        ((uint64_t *)pt_hi)[i] = (KERNEL_LMA + i * PAGE_SIZE) | PTE_P | PTE_RW;

    return pml4;
}

/* ---- kernel image ------------------------------------------------------- */
static void load_kernel(EFI_BOOT_SERVICES *bs, EFI_HANDLE image) {
    EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID fi_guid = EFI_FILE_INFO_GUID;

    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (bs->handle_protocol(image, &li_guid, (void **)&li) != EFI_SUCCESS || !li)
        die(u"loaded-image");

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    if (bs->handle_protocol(li->device_handle, &fs_guid, (void **)&fs) != EFI_SUCCESS || !fs)
        die(u"filesystem");

    EFI_FILE_PROTOCOL *root = 0, *f = 0;
    if (fs->open_volume(fs, &root) != EFI_SUCCESS || !root)
        die(u"open volume");
    if (root->open(root, &f, u"KERNEL.BIN", EFI_FILE_MODE_READ, 0) != EFI_SUCCESS || !f)
        die(u"open KERNEL.BIN");

    uint8_t info_buf[512];
    uint64_t info_size = sizeof info_buf;
    if (f->get_info(f, &fi_guid, &info_size, info_buf) != EFI_SUCCESS)
        die(u"file info");
    uint64_t size = ((EFI_FILE_INFO *)info_buf)->file_size;
    if (size == 0 || size > 8ull * 1024 * 1024)
        die(u"implausible kernel size");

    /* AllocateAddress at exactly 0x400000: the flat image is linked for that
     * LMA, so a "convenient" address chosen by the firmware would relocate the
     * kernel without relocating the higher-half mapping that points at it. */
    uint64_t at = KERNEL_LMA;
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (bs->allocate_pages(AllocateAddress, EfiLoaderData, pages, &at) != EFI_SUCCESS)
        die(u"cannot claim 0x400000");

    uint64_t got = size;
    if (f->read(f, &got, (void *)KERNEL_LMA) != EFI_SUCCESS || got != size)
        die(u"short read");
    f->close(f);
    root->close(root);
}

/* ---- boot_info ---------------------------------------------------------
 * The UEFI memory map is converted to E820-shaped entries. Only Conventional
 * and the two BootServices types are free once boot services are gone; treating
 * anything else as free hands the PMM memory the firmware still owns
 * (DDR-886 §3.4).
 */
static uint32_t fill_boot_info(EFI_MEMORY_DESCRIPTOR *map, uint64_t map_size,
                               uint64_t desc_size) {
    struct boot_info *bi = (struct boot_info *)BOOT_INFO_PHYS;
    bi->magic = BOOT_MAGIC;
    bi->long_mode = 1;
    bi->reserved = 0;
    for (int i = 0; i < 16; i++)
        bi->cpu_vendor[i] = 0;

    uint32_t n = 0;
    /* desc_size, NOT sizeof(EFI_MEMORY_DESCRIPTOR) — the firmware reports its
     * own stride and it may be larger (DDR-886 §3.5). */
    for (uint64_t off = 0; off + desc_size <= map_size; off += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + off);
        uint32_t type;
        if (d->type == EfiConventionalMemory ||
            d->type == EfiBootServicesCode ||
            d->type == EfiBootServicesData ||
            d->type == EfiLoaderCode ||
            d->type == EfiLoaderData)
            type = 1;                       /* usable RAM */
        else
            type = 2;                       /* reserved — anything not proven free */

        uint64_t base = d->physical_start;
        uint64_t len  = d->number_of_pages * PAGE_SIZE;
        if (len == 0)
            continue;

        /* MERGE adjacent same-type runs. OVMF emits well over 100 descriptors,
         * mostly tiny BootServices regions that are contiguous and convert to
         * the same E820 type. Without this the first version hit its own cap and
         * SILENTLY TRUNCATED the map — the kernel then never saw the tail of
         * RAM and simply had less memory, with nothing anywhere saying so. */
        if (n > 0 && bi->e820[n - 1].type == type &&
            bi->e820[n - 1].base + bi->e820[n - 1].len == base) {
            bi->e820[n - 1].len += len;
            continue;
        }

        /* boot_info lives at 0x4000 and the page ends at 0x5000: 32-byte header
         * plus 24 bytes an entry leaves room for 169. Overflowing it would
         * scribble past the page; truncating would lose RAM silently. Refuse. */
        if (n >= 169)
            die(u"memory map exceeds boot_info capacity");

        bi->e820[n].base = base;
        bi->e820[n].len  = len;
        bi->e820[n].type = type;
        bi->e820[n].acpi = 1;
        n++;
    }
    bi->e820_count = n;
    return n;
}

/* ---- entry -------------------------------------------------------------- */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    ST = st;
    EFI_BOOT_SERVICES *bs = st->boot_services;

    print(u"[uefi] PRADYOS loader\r\n");
    load_kernel(bs, image);
    uint64_t cr3 = build_page_tables(bs);
    print(u"[uefi] handoff\r\n");

    /* GetMemoryMap invalidates its own key on every allocation, including the
     * one that holds the map. Fetch it immediately before ExitBootServices and
     * RETRY on failure with a fresh map — reporting the failure and continuing
     * would run the kernel with boot services still live (DDR-886 §3.1). */
    EFI_MEMORY_DESCRIPTOR *map = 0;
    uint64_t map_size = 0, key = 0, desc_size = 0;
    uint32_t desc_ver = 0;

    for (int attempt = 0; attempt < 8; attempt++) {
        map_size = 0;
        bs->get_memory_map(&map_size, 0, &key, &desc_size, &desc_ver);
        map_size += 4 * desc_size;                  /* headroom for the alloc itself */
        if (bs->allocate_pool(EfiLoaderData, map_size, (void **)&map) != EFI_SUCCESS)
            die(u"map pool");
        if (bs->get_memory_map(&map_size, map, &key, &desc_size, &desc_ver) != EFI_SUCCESS) {
            bs->free_pool(map);
            continue;
        }
        fill_boot_info(map, map_size, desc_size);
        if (bs->exit_boot_services(image, key) == EFI_SUCCESS)
            goto exited;
        bs->free_pool(map);                          /* stale key — refetch */
    }
    die(u"ExitBootServices");

exited:
    /* No firmware call is legal past this point — not even a print. */
    __asm__ volatile(
        "cli\n"
        "mov %0, %%cr3\n"
        "mov %1, %%rdi\n"
        "mov %2, %%rax\n"
        "jmp *%%rax\n"
        :
        : "r"(cr3), "r"(BOOT_INFO_PHYS), "r"(KERNEL_VBASE)
        : "memory");
    for (;;)
        __asm__ volatile("hlt");
}
