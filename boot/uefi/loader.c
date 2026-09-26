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
    uint32_t long_mode, acpi_rsdp;   /* DDR-978: was `reserved` */
    struct e820_entry e820[];
};
_Static_assert(sizeof(struct e820_entry) == 24, "e820 entry must stay 24 bytes");
_Static_assert(sizeof(struct boot_info) == 32,  "boot_info header must stay 32 bytes");

/* DDR-1142: the GOP framebuffer handoff, mirror of struct boot_fb
 * (kernel/boot_info.h). It sits in the LAST 32 bytes of the boot_info page,
 * which is why the E820 cap below is 167 and not 169: 32 + 167*24 = 0xFC8,
 * clear of 0xFE0. `check` lets the kernel reject a block this loader did not
 * write -- the BIOS path zeroes it, and zero never validates. */
#define BOOT_FB_PHYS   0x4FE0ull
#define BOOT_FB_MAGIC  0x31424647u          /* 'GFB1' */
#define E820_CAP       165u          /* DDR-1143 §10.4: was 167; boot_kimg sits at 0xFC0 */
struct boot_fb {
    uint32_t magic, format;
    uint64_t base;
    uint32_t width, height, stride_px, check;
};
_Static_assert(sizeof(struct boot_fb) == 32, "boot_fb must stay 32 bytes");
_Static_assert(32 + E820_CAP * 24 <= 0xFC0, "E820 entries would reach boot_kimg");

/* DDR-1143 §10.4: the pristine kernel copy, mirror of struct boot_kimg
 * (kernel/boot_info.h), 32 bytes directly below boot_fb. The copy is taken
 * from 0x400000 right after the file read -- before the kernel has run, so
 * .data is still the file's .data. `size` is the exact file size here; the
 * kernel refuses the block unless it equals its own linker length. */
#define BOOT_KIMG_PHYS  0x4FC0ull
#define BOOT_KIMG_MAGIC 0x474D494Bu         /* 'KIMG' */
#define KIMG_PMM_FLOOR  0x1000000ull       /* kernel PMM_MIN_PHYS: never allocated */
#define KIMG_SRC_UEFI   2u
struct boot_kimg {
    uint32_t magic, source;
    uint64_t base, size;
    uint32_t reserved, check;
};
_Static_assert(sizeof(struct boot_kimg) == 32, "boot_kimg must stay 32 bytes");
static struct boot_kimg g_kimg;             /* magic 0 until the copy exists */

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

    /* DDR-1143 §10.4: keep a byte copy of what was just read, BELOW the
     * kernel's 16 MiB PMM floor. That bound is load-bearing: fill_boot_info
     * reports EfiLoaderData as USABLE, so a copy above 16 MiB would be handed
     * out by the PMM. A fixed 0x800000 (stage2's choice) is NOT used here --
     * measured, OVMF owns it ("cannot claim 0x800000"). If no room exists the
     * copy is skipped and the boot continues: the kernel reports the block
     * absent and the installer refuses, which beats not booting at all. */
    uint64_t kat = KIMG_PMM_FLOOR - 1;
    if (bs->allocate_pages(AllocateMaxAddress, EfiLoaderData, pages, &kat) != EFI_SUCCESS) {
        print(u"[uefi] kimg: no room below 16 MiB, copy skipped\r\n");
        f->close(f);
        root->close(root);
        return;
    }
    const volatile uint8_t *ks = (const volatile uint8_t *)KERNEL_LMA;
    volatile uint8_t *kd = (volatile uint8_t *)kat;
    for (uint64_t i = 0; i < size; i++)
        kd[i] = ks[i];
    g_kimg.magic    = BOOT_KIMG_MAGIC;
    g_kimg.source   = KIMG_SRC_UEFI;
    g_kimg.base     = kat;
    g_kimg.size     = size;
    g_kimg.reserved = 0;
    g_kimg.check    = g_kimg.magic ^ g_kimg.source ^ (uint32_t)g_kimg.base ^
                      (uint32_t)(g_kimg.base >> 32) ^ (uint32_t)g_kimg.size ^
                      (uint32_t)(g_kimg.size >> 32) ^ g_kimg.reserved;
    f->close(f);
    root->close(root);
}

/* ---- boot_info ---------------------------------------------------------
 * The UEFI memory map is converted to E820-shaped entries. Only Conventional
 * and the two BootServices types are free once boot services are gone; treating
 * anything else as free hands the PMM memory the firmware still owns
 * (DDR-886 §3.4).
 */
/* DDR-978: the ACPI RSDP, from the EFI Configuration Table.
 *
 * The kernel's other discovery path scans 0xE0000..0xFFFFF, which is where
 * SeaBIOS puts the RSDP and where OVMF does not. Without this the kernel finds
 * no ACPI at all under UEFI: no MCFG (so PCIe enumerates NOTHING -- no disk, no
 * net, no GPU), no MADT (no APs), no FADT (no S5 poweroff). Measured in
 * DDR-978 §3.
 *
 * Prefer the ACPI 2.0 GUID (XSDT-capable) and fall back to ACPI 1.0. Returns 0
 * if absent, or if the address does not fit the 32-bit handoff field -- refusing
 * is correct there, because a truncated pointer would send the kernel to a wild
 * address whereas 0 simply restores the legacy scan. */
static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (int i = 0; i < 16; i++)
        if (x[i] != y[i]) return 0;
    return 1;
}

static uint32_t find_acpi_rsdp(void) {
    if (!ST || !ST->config_table)
        return 0;
    EFI_GUID g20 = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID g10 = EFI_ACPI_10_TABLE_GUID;
    void *best = 0;
    for (uint64_t i = 0; i < ST->num_table_entries; i++) {
        EFI_CONFIGURATION_TABLE *e = &ST->config_table[i];
        if (guid_eq(&e->vendor_guid, &g20)) { best = e->vendor_table; break; }
        if (guid_eq(&e->vendor_guid, &g10) && !best) best = e->vendor_table;
    }
    uint64_t pa = (uint64_t)(uintptr_t)best;
    if (!pa || pa > 0xFFFFFFFFull)
        return 0;
    return (uint32_t)pa;
}

/* DDR-978: the loader zeroed cpu_vendor and never filled it, so a UEFI boot
 * printed "NEXUS: boot_info OK vendor=" with an empty string while the BIOS path
 * printed AuthenticAMD. CPUID leaf 0 returns the 12-char vendor in EBX,EDX,ECX
 * -- the same order stage2.asm:338-341 stores it in. */
static void fill_cpu_vendor(char out[16]) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    (void)a;
    const uint32_t w[3] = { b, d, c };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (char)((w[i] >> (8 * j)) & 0xFF);
    out[12] = 0; out[13] = 0; out[14] = 0; out[15] = 0;
}

static uint32_t fill_boot_info(EFI_MEMORY_DESCRIPTOR *map, uint64_t map_size,
                               uint64_t desc_size) {
    struct boot_info *bi = (struct boot_info *)BOOT_INFO_PHYS;
    bi->magic = BOOT_MAGIC;
    bi->long_mode = 1;
    bi->acpi_rsdp = find_acpi_rsdp();        /* DDR-978; 0 => kernel scans */
    for (int i = 0; i < 16; i++)
        bi->cpu_vendor[i] = 0;
    fill_cpu_vendor(bi->cpu_vendor);         /* DDR-978: was left empty */

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
         * plus 24 bytes an entry left room for 169. DDR-1142 took the last 32
         * bytes for boot_fb, so the cap is 167. Overflowing it would scribble
         * over the framebuffer handoff; truncating would lose RAM silently.
         * Refuse. */
        if (n >= E820_CAP)
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

/* ---- GOP framebuffer (DDR-1142) ----------------------------------------
 * Read the mode the firmware ALREADY set on the console-out handle; never call
 * SetMode. Must run before ExitBootServices (the protocol is a boot service)
 * and is done ONCE, before the GetMemoryMap/ExitBootServices retry loop, so no
 * firmware call sits between fetching the map key and using it.
 *
 * The format is recorded as reported and NOT filtered here: the kernel decides
 * what it can draw, and a refused format should still be visible in its log. */
static struct boot_fb g_fb;

static void query_gop(EFI_BOOT_SERVICES *bs) {
    g_fb.magic = 0;
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (!ST->console_out_handle ||
        bs->handle_protocol(ST->console_out_handle, &gop_guid, (void **)&gop) != EFI_SUCCESS ||
        !gop || !gop->mode || !gop->mode->info) {
        print(u"[uefi] gop none\r\n");
        return;
    }
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->mode->info;
    g_fb.format    = mi->pixel_format;
    g_fb.base      = gop->mode->frame_buffer_base;
    g_fb.width     = mi->horizontal_resolution;
    g_fb.height    = mi->vertical_resolution;
    g_fb.stride_px = mi->pixels_per_scan_line;
    g_fb.magic     = BOOT_FB_MAGIC;
    g_fb.check     = g_fb.magic ^ g_fb.format ^ (uint32_t)g_fb.base ^
                     (uint32_t)(g_fb.base >> 32) ^ g_fb.width ^ g_fb.height ^
                     g_fb.stride_px;
    print(u"[uefi] gop found\r\n");
}

static void write_boot_fb(void) {
    struct boot_fb *dst = (struct boot_fb *)BOOT_FB_PHYS;
    *dst = g_fb;                              /* magic 0 => "no framebuffer" */
    struct boot_kimg *kdst = (struct boot_kimg *)BOOT_KIMG_PHYS;
    *kdst = g_kimg;                           /* DDR-1143 §10.4 */
}

/* ---- entry -------------------------------------------------------------- */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    ST = st;
    EFI_BOOT_SERVICES *bs = st->boot_services;

    print(u"[uefi] PRADYOS loader\r\n");
    load_kernel(bs, image);
    uint64_t cr3 = build_page_tables(bs);
    query_gop(bs);                           /* DDR-1142: before the map loop */
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
        write_boot_fb();
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
