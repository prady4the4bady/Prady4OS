/* kernel/boot_info.h
 * The boot -> kernel handoff ABI. The bootloader (boot/stage2/stage2.asm) fills
 * this struct at a fixed physical address and passes its pointer to kmain in RDI.
 * Keep this layout byte-for-byte in sync with the BOOT_INFO offsets in stage2.
 */
#pragma once
#include <stdint.h>

#define BOOT_INFO_MAGIC 0x59445250u   /* 'PRDY' */

/* One BIOS INT 15h E820 entry (we always request 24-byte entries). */
struct e820_entry {
    uint64_t base;
    uint64_t len;
    uint32_t type;     /* 1 = usable, 2 = reserved, 3 = ACPI reclaim, ... */
    uint32_t acpi;     /* ACPI 3.0 extended attributes */
} __attribute__((packed));

/* ---- NUMA topology (DDR-882, item 17) ---------------------------------
 *
 * This struct is DECLARED here because boot_info.h is where machine topology
 * lives, but it is NOT part of the boot handoff and the bootloader never writes
 * it. Two reasons, both structural:
 *
 *   1. `struct boot_info` ends in a flexible array (`e820[]`). Nothing can be
 *      appended after it — a trailing flexible array must be last, and a block
 *      placed there would overlap the E820 entries.
 *   2. Topology comes from ACPI SRAT, which needs RSDP discovery, checksum
 *      validation and sub-table walking. stage2.asm is 1294 bytes of 16-bit
 *      assembly; the kernel reads SRAT itself in numa_init().
 *
 * The boot protocol is therefore unchanged by this feature.
 */
#define NUMA_MAX_NODES  8
#define NUMA_MAX_RANGES 32
#define NUMA_MAX_CPUS   64

struct numa_range {
    uint64_t base;             /* page-aligned; unaligned ranges are REJECTED */
    uint64_t len;
    uint32_t node;
    uint32_t _pad;
};

struct numa_topology {
    uint32_t valid;            /* 0 = no SRAT; treat the machine as one node   */
    uint32_t node_count;
    uint32_t range_count;
    uint32_t rejected;         /* sub-tables refused, not absorbed (DDR-882 §4) */
    struct numa_range range[NUMA_MAX_RANGES];
    uint8_t  cpu_node[NUMA_MAX_CPUS];   /* APIC id -> node; 0xFF = unknown     */
};

struct boot_info {
    uint32_t magic;            /* BOOT_INFO_MAGIC */
    uint32_t e820_count;       /* number of valid e820[] entries */
    char     cpu_vendor[16];   /* CPUID leaf 0 vendor string, NUL-terminated */
    uint32_t long_mode;        /* 1 if CPUID 80000001h EDX.29 set */
    /* DDR-978: physical address of the ACPI RSDP, or 0 if the loader did not
     * find one. Was `reserved`; same offset (28), same size, header stays 32 B.
     *
     * WHY IT IS NEEDED. The kernel's only other discovery path scans the legacy
     * BIOS window 0xE0000..0xFFFFF (acpi.c). UEFI firmware is not obliged to put
     * the RSDP there and OVMF does not -- it publishes it via the EFI
     * Configuration Table. Without this field a UEFI boot finds no RSDP, hence
     * no MCFG (no PCIe -> NO DEVICES AT ALL), no MADT (no APs) and no FADT (no
     * S5 poweroff). Measured in DDR-978 §3.
     *
     * BIOS path is unaffected: stage2.asm zeroes the whole 32-byte header
     * (stage2.asm:108-115) and never writes this, so it stays 0 and the kernel
     * falls back to the legacy scan exactly as before.
     *
     * 32 bits is enough on x86_64 -- firmware ACPI tables live well below 4 GiB
     * -- and the UEFI loader REFUSES to store an address >= 4 GiB rather than
     * truncating it. The kernel validates the signature and checksum before
     * trusting whatever arrives here, so a bad value degrades to the scan. */
    uint32_t acpi_rsdp;
    struct e820_entry e820[];  /* e820_count entries follow the header */
} __attribute__((packed));

/* ---- GOP framebuffer handoff (DDR-1142) --------------------------------
 *
 * The UEFI loader reads the firmware's already-set Graphics Output Protocol
 * mode and writes it here, in the LAST 32 bytes of the boot_info page (the
 * pinned header cannot grow: it ends in a flexible e820[]). The loader's E820
 * cap is 167 so the entries can never reach it. stage2 zeroes it, so the BIOS
 * path reads magic 0 = "no framebuffer".
 *
 * Accepted only when magic AND check match -- a block nobody wrote does not
 * become a framebuffer at a random physical address. */
#define BOOT_FB_PHYS  0x4FE0ull
#define BOOT_FB_MAGIC 0x31424647u     /* 'GFB1' */
#define BOOT_FB_FMT_BGRX 1u           /* PixelBlueGreenRedReserved8BitPerColor */

struct boot_fb {
    uint32_t magic;
    uint32_t format;          /* EFI_GRAPHICS_PIXEL_FORMAT, as the firmware reported */
    uint64_t base;            /* physical */
    uint32_t width, height;
    uint32_t stride_px;       /* PixelsPerScanLine, NOT width */
    uint32_t check;           /* xor of every other 32-bit word */
} __attribute__((packed));
_Static_assert(sizeof(struct boot_fb) == 32, "boot_fb must stay 32 bytes");

/* ---- pristine kernel image handoff (DDR-1143 §10.4) --------------------
 *
 * The installer must write the kernel's own bytes, and once kmain runs nothing
 * in memory IS those bytes (.data is live, .bss sits over the load window). So
 * each loader keeps a second copy at KIMG_PHYS, taken BEFORE it jumps, and
 * describes it here -- 32 bytes directly below boot_fb, because the header
 * cannot grow (it ends in e820[]). stage2 caps E820 at 32 entries (0x320) and
 * the UEFI loader at 165 (32 + 165*24 = 0xF98), both clear of 0xFC0.
 *
 * KIMG_PHYS is below PMM_MIN_PHYS (16 MiB) so the PMM never hands it out, and
 * a separate 2 MiB page from the DDR-1046 alias of the running image.
 *
 * `size` is the READ WINDOW on the BIOS path (stage2 never learns the file
 * size) and the EXACT FILE SIZE on the UEFI path. The kernel's own image length
 * comes from __data_end, and the block is accepted only when it covers that. */
#define BOOT_KIMG_PHYS   0x4FC0ull
#define BOOT_KIMG_MAGIC  0x474D494Bu     /* 'KIMG' */
#define KIMG_PHYS        0x800000ull     /* 8 MiB */
#define KIMG_WINDOW      0x180000ull     /* 48 x 32 KiB, stage2's read window */
#define KIMG_SRC_BIOS    1u
#define KIMG_SRC_UEFI    2u

struct boot_kimg {
    uint32_t magic;
    uint32_t source;          /* KIMG_SRC_BIOS / KIMG_SRC_UEFI */
    uint64_t base;            /* physical */
    uint64_t size;            /* bytes copied (window on BIOS, file on UEFI) */
    uint32_t reserved;
    uint32_t check;           /* xor of every other 32-bit word */
} __attribute__((packed));
_Static_assert(sizeof(struct boot_kimg) == 32, "boot_kimg must stay 32 bytes");
