/* kernel/acpi/acpi.c — RSDP/RSDT/XSDT discovery. */
#include "acpi.h"
#include "console.h"
#include "string.h"

struct rsdp {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;          /* covers the first 20 bytes (ACPI 1.0) */
    char     oemid[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, >=2 = 2.0+ (XSDT present) */
    uint32_t rsdt_address;
    uint32_t length;            /* ACPI 2.0+ */
    uint64_t xsdt_address;      /* ACPI 2.0+ */
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

static const struct acpi_sdt_header *g_root;   /* RSDT or XSDT */
static int      g_use_xsdt;
static uint32_t g_count;                        /* number of table pointers */

static uint8_t sum8(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++)
        s += b[i];
    return s;
}

/* Scan the BIOS area for the RSDP (QEMU/SeaBIOS place it in 0xE0000..0xFFFFF). */
static const struct rsdp *find_rsdp(void) {
    for (uint64_t a = 0xE0000; a < 0x100000; a += 16) {
        const char *p = (const char *)(uintptr_t)a;
        if (memcmp(p, "RSD PTR ", 8) == 0 && sum8(p, 20) == 0)
            return (const struct rsdp *)p;
    }
    return 0;
}

void acpi_init(void) {
    const struct rsdp *r = find_rsdp();
    if (!r) {
        kputs("ACPI: RSDP not found\r\n");
        return;
    }
    if (r->revision >= 2 && r->xsdt_address) {
        g_root = (const struct acpi_sdt_header *)(uintptr_t)r->xsdt_address;
        g_use_xsdt = 1;
    } else {
        g_root = (const struct acpi_sdt_header *)(uintptr_t)(uint64_t)r->rsdt_address;
        g_use_xsdt = 0;
    }
    g_count = (g_root->length - (uint32_t)sizeof(struct acpi_sdt_header)) /
              (g_use_xsdt ? 8u : 4u);

    kputs("ACPI: ");
    kputs(g_use_xsdt ? "XSDT" : "RSDT");
    kputs(", ");
    kputdec(g_count);
    kputs(" tables (rev ");
    kputdec(r->revision);
    kputs(")\r\n");
}

const struct acpi_sdt_header *acpi_find_table(const char sig[4]) {
    if (!g_root)
        return 0;
    const uint8_t *ents = (const uint8_t *)g_root + sizeof(struct acpi_sdt_header);
    for (uint32_t i = 0; i < g_count; i++) {
        uint64_t addr = g_use_xsdt
            ? ((const uint64_t *)ents)[i]
            : (uint64_t)((const uint32_t *)ents)[i];
        const struct acpi_sdt_header *h = (const struct acpi_sdt_header *)(uintptr_t)addr;
        if (memcmp(h->signature, sig, 4) == 0)
            return h;
    }
    return 0;
}
