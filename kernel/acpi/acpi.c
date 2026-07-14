/* kernel/acpi/acpi.c — RSDP/RSDT/XSDT discovery + FADT S5 poweroff (DDR-746). */
#include "acpi.h"
#include "console.h"
#include "string.h"
#include "io.h"

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

/* ---- DDR-746: FADT (FACP) parse + ACPI S5 (soft-off) poweroff ------------- */

/* PM1x_CNT SLP_EN bit (ACPI 6.x §4.8.3.2.1). SLP_TYP occupies bits 10..12. */
#define ACPI_SLP_EN (1u << 13)

static uint16_t g_pm1a_cnt, g_pm1b_cnt;   /* PM1{a,b} control I/O ports        */
static uint8_t  g_slp_typa, g_slp_typb;   /* \_S5_ sleep-type values (3 bits)  */
static int      g_s5_ok;                  /* set once the S5 path is resolved  */
static uint16_t g_reset_port;             /* FADT RESET_REG I/O port (0 = none) */
static uint8_t  g_reset_value;            /* FADT RESET_VALUE                   */

/* Little-endian u32 at a byte offset into a table (FADT fields are unaligned). */
static uint32_t acpi_rd32(const void *base, uint32_t off) {
    const uint8_t *b = (const uint8_t *)base + off;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

void acpi_power_init(void) {
    const struct acpi_sdt_header *fadt = acpi_find_table("FACP");
    if (!fadt) { kputs("ACPI: no FADT (no S5)\r\n"); return; }

    /* FADT field offsets (ACPI 6.x §5.2.9): DSDT=40, PM1a_CNT_BLK=64,
     * PM1b_CNT_BLK=66. The 32-bit fields are always populated by QEMU/SeaBIOS
     * and the tables live in identity-mapped low RAM (see acpi_find_table). */
    uint32_t dsdt = acpi_rd32(fadt, 40);
    g_pm1a_cnt = (uint16_t)acpi_rd32(fadt, 64);
    g_pm1b_cnt = (uint16_t)acpi_rd32(fadt, 66);
    if (!g_pm1a_cnt || !dsdt) { kputs("ACPI: FADT lacks PM1a/DSDT (no S5)\r\n"); return; }

    /* Minimal \_S5_ scan (ACPI §7.4.2): find the "_S5_" NameOp and read the
     * first two integer elements of the following Package — SLP_TYPa/b. This is
     * NOT a full AML interpreter; if _S5_ is hidden behind AML control flow the
     * scan fails and poweroff becomes a no-op. */
    const struct acpi_sdt_header *d = (const struct acpi_sdt_header *)(uintptr_t)dsdt;
    const uint8_t *aml = (const uint8_t *)d + sizeof *d;
    uint32_t n = d->length - (uint32_t)sizeof *d;
    for (uint32_t i = 0; i + 6 < n; i++) {
        if (aml[i] != '_' || aml[i+1] != 'S' || aml[i+2] != '5' || aml[i+3] != '_')
            continue;
        /* Valid form: NameOp(0x08) ["\\"] "_S5_" PackageOp(0x12). */
        int name_ok = (i >= 1 && aml[i-1] == 0x08) ||
                      (i >= 2 && aml[i-2] == 0x08 && aml[i-1] == '\\');
        if (!name_ok || aml[i+4] != 0x12)
            continue;
        const uint8_t *p = aml + i + 5;         /* past "_S5_" + PackageOp        */
        p += ((*p & 0xC0u) >> 6) + 2;           /* skip PkgLength(+extra) + NumElem */
        if (*p == 0x0A) p++;                    /* optional BytePrefix            */
        g_slp_typa = *p++;
        if (*p == 0x0A) p++;
        g_slp_typb = *p;
        g_s5_ok = 1;
        break;
    }

    /* DDR-747: FADT reset register (ACPI §5.2.9). Flags@112 bit10 =
     * RESET_REG_SUPPORTED; RESET_REG is a GAS@116 (byte0 = address_space_id,
     * bytes4..11 = address); RESET_VALUE@128. Only a System-I/O (id==1) register
     * is used — memory/PCI-config reset spaces need a different access path and
     * the 0xCF9/8042 fallbacks in acpi_reboot() cover QEMU regardless. */
    uint32_t flags = acpi_rd32(fadt, 112);
    if ((flags & (1u << 10)) && ((const uint8_t *)fadt)[116] == 1) {
        uint64_t addr = 0;
        for (int k = 0; k < 8; k++)
            addr |= (uint64_t)((const uint8_t *)fadt)[116 + 4 + k] << (8 * k);
        if (addr && addr <= 0xFFFFu) {
            g_reset_port  = (uint16_t)addr;
            g_reset_value = ((const uint8_t *)fadt)[128];
        }
    }

    kputs("ACPI: FADT ok, S5 ");
    kputs(g_s5_ok ? "found" : "NOT found");
    kputs(g_reset_port ? ", reset-reg\r\n" : "\r\n");
}

int acpi_power_available(void) { return g_s5_ok; }

__attribute__((noreturn)) void acpi_poweroff(void) {
    /* Sentinel BEFORE the port write — the machine may power off immediately. */
    kputs("PRADYOS_POWEROFF\r\n");
    if (g_s5_ok && g_pm1a_cnt) {
        outw(g_pm1a_cnt, (uint16_t)(((uint16_t)(g_slp_typa & 7u) << 10) | ACPI_SLP_EN));
        if (g_pm1b_cnt)
            outw(g_pm1b_cnt, (uint16_t)(((uint16_t)(g_slp_typb & 7u) << 10) | ACPI_SLP_EN));
    }
    /* Still running -> S5 unavailable or refused. Halt this CPU forever. */
    for (;;)
        __asm__ volatile("cli; hlt");
}

__attribute__((noreturn)) void acpi_reboot(void) {
    kputs("PRADYOS_REBOOT\r\n");
    /* 1. FADT reset register (if a System-I/O one was parsed). */
    if (g_reset_port)
        outb(g_reset_port, g_reset_value);
    /* 2. PCI reset-control port 0xCF9: SYS_RST|RST_CPU|FULL_RST. */
    outb(0xCF9, 0x0E);
    /* 3. 8042 keyboard controller: pulse the CPU reset line. */
    outb(0x64, 0xFE);
    /* All reset paths failed (or are disabled) — halt this CPU forever. */
    for (;;)
        __asm__ volatile("cli; hlt");
}
