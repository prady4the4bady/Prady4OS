/* kernel/mm/numa.c — ACPI SRAT topology parse (Group 3 item 17a, DDR-882).
 *
 * SRAT (System Resource Affinity Table) maps proximity domains to CPUs and to
 * physical memory ranges. Layout per ACPI 6.x §5.2.16: a 48-byte header (the
 * 36-byte SDT header + 4 reserved + 8 reserved) followed by variable-length
 * sub-tables, each starting with {uint8 type, uint8 length}.
 *
 * Sub-tables handled:
 *   type 0 — Processor Local APIC/SAPIC Affinity
 *   type 1 — Memory Affinity
 *   type 2 — Processor Local x2APIC Affinity
 *
 * ---------------------------------------------------------------------------
 * THE THINGS THAT MAKE SRAT SUBTLY WRONG
 *
 * 1. THE PROXIMITY DOMAIN IS SPLIT ACROSS FOUR BYTES IN TYPE 0. The low byte is
 *    at offset 2 and the high three bytes at offsets 9..11, with the APIC id in
 *    between. Reading it as one little-endian u32 at offset 2 gives a value
 *    contaminated by the APIC id — which still looks like a small plausible
 *    domain number on a two-node machine.
 *
 * 2. A SUB-TABLE WITH length == 0 IS AN INFINITE LOOP. The walk advances by the
 *    length field, so a malformed or misread zero stalls forever inside a
 *    kernel init path with interrupts on and no watchdog. Bounded explicitly.
 *
 * 3. THE ENABLED FLAG IS NOT OPTIONAL. Both affinity types carry a flags word
 *    whose bit 0 means "enabled". Firmware routinely lists disabled entries for
 *    hot-plug sockets and absent memory; counting them inflates the node count
 *    and files frames onto a node that has no memory behind it.
 */
#include "numa.h"
#include "acpi.h"
#include "console.h"
#include "string.h"

static struct numa_topology g_topo;

/* Track which proximity domains were actually seen, so node_count reflects
 * reality rather than the highest id encountered. A machine may legitimately
 * number its domains 0 and 3. */
static uint8_t g_domain_seen[256];

static uint32_t domain_slot(uint32_t domain) {
    /* Domains above the table are refused, not folded — see DDR-882 §4. */
    if (domain > 255)
        return 0xFFFFFFFFu;
    if (!g_domain_seen[domain]) {
        if (g_topo.node_count >= NUMA_MAX_NODES)
            return 0xFFFFFFFFu;
        g_domain_seen[domain] = (uint8_t)(g_topo.node_count + 1);
        g_topo.node_count++;
    }
    return (uint32_t)(g_domain_seen[domain] - 1);
}

static void reject(const char *why) {
    g_topo.rejected++;
    kputs("[numa] rejected: ");
    kputs(why);
    kputs("\r\n");
}

void numa_init(void) {
    memset(&g_topo, 0, sizeof g_topo);
    memset(g_domain_seen, 0, sizeof g_domain_seen);
    for (unsigned i = 0; i < NUMA_MAX_CPUS; i++)
        g_topo.cpu_node[i] = 0xFF;

    const struct acpi_sdt_header *h = acpi_find_table("SRAT");
    if (!h) {
        /* Not an error. A single-node machine has no SRAT, and inventing
         * topology from nothing would be worse than reporting none. */
        kputs("[numa] no SRAT; single node\r\n");
        g_topo.node_count = 1;
        return;
    }
    if (h->length < 48u) {
        reject("SRAT shorter than its own header");
        g_topo.node_count = 1;
        return;
    }

    const uint8_t *p   = (const uint8_t *)h + 48;   /* first sub-table */
    const uint8_t *end = (const uint8_t *)h + h->length;

    /* Bounded: a zero-length sub-table would otherwise spin forever (note 2).
     * The cap is the largest number of sub-tables that could fit. */
    unsigned guard = 0;
    while (p + 2 <= end && guard++ < 4096u) {
        uint8_t type = p[0];
        uint8_t len  = p[1];
        if (len < 2u || p + len > end) {
            reject("sub-table length out of range");
            break;
        }

        if (type == 0 && len >= 16u) {                  /* Local APIC affinity */
            /* ACPI 6.x §5.2.16.1 layout: 2 = domain[7:0], 3 = APIC id,
             * 4..7 = flags, 8 = SAPIC EID, 9..11 = domain[31:8]. The domain is
             * split around the APIC id — see note 1. */
            uint32_t flags = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                             ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
            if (!(flags & 1u))                          /* disabled (note 3)   */
                goto next;
            uint32_t domain = (uint32_t)p[2] |
                              ((uint32_t)p[9] << 8) |
                              ((uint32_t)p[10] << 16) |
                              ((uint32_t)p[11] << 24);
            uint32_t node = domain_slot(domain);
            if (node == 0xFFFFFFFFu) { reject("too many proximity domains (cpu)"); goto next; }
            uint8_t apic = p[3];
            if (apic < NUMA_MAX_CPUS)
                g_topo.cpu_node[apic] = (uint8_t)node;
        } else if (type == 1 && len >= 40u) {           /* Memory affinity     */
            uint32_t flags = (uint32_t)p[28] | ((uint32_t)p[29] << 8) |
                             ((uint32_t)p[30] << 16) | ((uint32_t)p[31] << 24);
            if (!(flags & 1u))                          /* disabled (note 3)   */
                goto next;
            uint32_t domain = (uint32_t)p[2] | ((uint32_t)p[3] << 8) |
                              ((uint32_t)p[4] << 16) | ((uint32_t)p[5] << 24);
            uint64_t base = 0, length = 0;
            for (int i = 0; i < 8; i++) base   |= (uint64_t)p[8 + i]  << (8 * i);
            for (int i = 0; i < 8; i++) length |= (uint64_t)p[16 + i] << (8 * i);

            if (length == 0)                     { reject("zero-length memory range");  goto next; }
            if ((base | length) & 0xFFFull)      { reject("memory range not page aligned"); goto next; }
            if (g_topo.range_count >= NUMA_MAX_RANGES) { reject("too many memory ranges"); goto next; }
            uint32_t node = domain_slot(domain);
            if (node == 0xFFFFFFFFu) { reject("too many proximity domains (mem)"); goto next; }

            g_topo.range[g_topo.range_count].base = base;
            g_topo.range[g_topo.range_count].len  = length;
            g_topo.range[g_topo.range_count].node = node;
            g_topo.range_count++;
        } else if (type == 2 && len >= 24u) {           /* x2APIC affinity     */
            uint32_t flags = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                             ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
            if (!(flags & 1u))
                goto next;
            uint32_t domain = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                              ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
            uint32_t apic = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                            ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
            uint32_t node = domain_slot(domain);
            if (node == 0xFFFFFFFFu) { reject("too many proximity domains (x2apic)"); goto next; }
            if (apic < NUMA_MAX_CPUS)
                g_topo.cpu_node[apic] = (uint8_t)node;
        }
next:
        p += len;
    }

    if (g_topo.node_count == 0)
        g_topo.node_count = 1;
    g_topo.valid = 1;

    { kline k; kline_init(&k);                       /* DDR-1055 */
      kline_s(&k, "[numa] nodes=");
      kline_d(&k, g_topo.node_count);
      kline_s(&k, " ranges=");
      kline_d(&k, g_topo.range_count);
      kline_s(&k, " rejected=");
      kline_d(&k, g_topo.rejected);
      kline_s(&k, "\r\n"); kline_emit(&k); }

    /* Per-node totals. This is the assertion with teeth in smoke-numa: a parser
     * that finds SRAT but mis-walks the sub-tables still reports a node count,
     * and only the byte totals show it read the right fields. */
    for (uint32_t n = 0; n < g_topo.node_count; n++) {
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < g_topo.range_count; i++)
            if (g_topo.range[i].node == n)
                bytes += g_topo.range[i].len;
        { kline k; kline_init(&k);                   /* DDR-1055 */
          kline_s(&k, "[numa] node");
          kline_d(&k, n);
          kline_s(&k, " mem=");
          kline_d(&k, bytes >> 20);
          kline_s(&k, "MiB\r\n"); kline_emit(&k); }
    }
}

const struct numa_topology *numa_get(void) { return &g_topo; }
uint32_t numa_node_count(void) { return g_topo.node_count ? g_topo.node_count : 1u; }

uint32_t numa_node_of(uint64_t phys) {
    for (uint32_t i = 0; i < g_topo.range_count; i++)
        if (phys >= g_topo.range[i].base &&
            phys <  g_topo.range[i].base + g_topo.range[i].len)
            return g_topo.range[i].node;
    return 0;                       /* unknown -> node 0; never fails (see .h) */
}

uint32_t numa_node_of_cpu(uint32_t apic_id) {
    if (apic_id < NUMA_MAX_CPUS && g_topo.cpu_node[apic_id] != 0xFF)
        return g_topo.cpu_node[apic_id];
    return 0;
}
