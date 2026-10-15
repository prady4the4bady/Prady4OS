/* kernel/drivers/usb/xhci.c — xHCI controller bring-up and port detection.
 *
 * DDR-1161, USB piece 1. Finds one xHCI controller (PCI 0C/03/30), maps BAR0,
 * reads the capability registers, halts and resets the controller with bounded
 * waits, and reports which root-hub ports have a device connected. No rings, no
 * slots, no enumeration: nothing here makes a USB device usable yet.
 * DDR-1162 adds the command/event rings and one polled Enable Slot.
 * DDR-1163 addresses the first connected device and reads its device descriptor.
 */
#include "xhci.h"
#include "console.h"
#include "pcie.h"
#include "vmm.h"
#include "pmm.h"
#include "../input/ps2kbd.h"

#define XHCI_BAR_VBASE 0xFFFFD60000000000ull   /* clear of ECAM/virtio/NVMe/GOP */
#define XHCI_BAR_MAP   0x10000u                 /* 64 KiB covers QEMU's BAR0 */

/* Capability registers (offsets from BAR0). */
#define CAP_CAPLENGTH  0x00                     /* byte */
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF      0x14
#define CAP_RTSOFF     0x18
/* Operational registers (offsets from BAR0 + CAPLENGTH). */
#define OP_USBCMD      0x00
#define OP_USBSTS      0x04
#define OP_CRCR        0x18                     /* 64-bit */
#define OP_DCBAAP      0x30                     /* 64-bit */
#define OP_CONFIG      0x38
#define OP_PORTSC(n)   (0x400u + 0x10u * ((n) - 1u))   /* n is 1-based */

#define USBCMD_RS      (1u << 0)
#define USBCMD_HCRST   (1u << 1)
#define USBSTS_HCH     (1u << 0)
#define USBSTS_CNR     (1u << 11)
#define PORTSC_CCS     (1u << 0)

static int g_xhci_up;

/* DDR-1162: one TRB = 16 bytes. */
struct trb { uint64_t param; uint32_t status; uint32_t control; };
#define TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3F)
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_ADDRESS_DEV 11
#define TRB_XFER_EVENT  32
#define TRB_CMD_DONE    33
#define TRB_CONFIG_EP   12
#define TRB_NORMAL      1
#define TRB_IOC         (1u << 5)
#define TRB_IDT         (1u << 6)
#define RING_TRBS       (PAGE_SIZE / sizeof(struct trb))   /* 256 */

static inline void wr64(volatile uint8_t *b, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(b + off)     = (uint32_t)v;
    *(volatile uint32_t *)(b + off + 4) = (uint32_t)(v >> 32);
}
static void *zpage(uint64_t *phys) {          /* identity-mapped low memory */
    uint64_t p = pmm_alloc_page();
    if (!p) return 0;
    uint8_t *v = (uint8_t *)(uintptr_t)p;
    for (unsigned i = 0; i < PAGE_SIZE; i++) v[i] = 0;
    *phys = p;
    return v;
}

static inline uint32_t rd32(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint32_t *)(b + off);
}
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(b + off) = v;
}

/* Bounded wait for (reg & mask) == want. 0 on success, -1 on timeout. */
static int wait_reg(volatile uint8_t *b, uint32_t off, uint32_t mask, uint32_t want) {
    for (uint64_t i = 0; i < 50000000ull; i++) {
        if ((rd32(b, off) & mask) == want)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

/* DDR-1162/1163: controller state for the polled command/event path. */
static struct {
    volatile uint8_t *op, *db, *ir;
    struct trb *cmdr, *evtr;
    uint64_t cmd_p, evt_p;
    uint32_t cmd_i, cmd_cyc, evt_i, evt_cyc;
    uint64_t *dcbaa;
    uint32_t csz;                               /* context size, 32 or 64 */
    struct trb *tr;                             /* DDR-1164: EP0 transfer ring */
    uint32_t tr_i, tr_cyc;
} X;

/* Put one TRB on a ring at *idx with cycle *cyc; follow the Link TRB at the end. */
static void ring_put(struct trb *r, uint32_t *idx, uint32_t *cyc,
                     uint64_t param, uint32_t status, uint32_t control) {
    r[*idx].param  = param;
    r[*idx].status = status;
    r[*idx].control = (control & ~1u) | *cyc;
    if (++*idx == RING_TRBS - 1) {
        r[*idx].control = (r[*idx].control & ~1u) | *cyc;   /* hand over the Link */
        *idx = 0;
        *cyc ^= 1u;
    }
}

/* Wait for the next event of `type`. Returns the event (copied) or 0 on timeout. */
/* DDR-1167: the one boot keyboard's interrupt-IN state. */
static struct {
    struct trb *itr; uint32_t itr_i, itr_cyc;
    volatile uint8_t *buf; uint64_t buf_p;
    uint32_t slot; uint8_t prev[8];
    int live, reported;
    uint32_t nrep;                  /* successful transfer events */
} H;

static void hid_queue(void) {
    for (int i = 0; i < 8; i++) H.buf[i] = 0;
    ring_put(H.itr, &H.itr_i, &H.itr_cyc, H.buf_p, 8, TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    wr32(X.db, 4 * H.slot, 3);
}

static int wait_event(uint32_t type, struct trb *out) {
    for (uint64_t i = 0; i < 50000000ull; i++) {
        volatile struct trb *e = &X.evtr[X.evt_i];
        if ((e->control & 1u) != X.evt_cyc) { __asm__ volatile("pause"); continue; }
        out->param = e->param; out->status = e->status; out->control = e->control;
        if (++X.evt_i == RING_TRBS) { X.evt_i = 0; X.evt_cyc ^= 1u; }
        wr64(X.ir, 0x18, (X.evt_p + X.evt_i * sizeof(struct trb)) | (1u << 3));
        if (TRB_GET_TYPE(out->control) == type)
            return 1;
        /* Port status change and other events are skipped. */
    }
    return 0;
}

static int command(uint64_t param, uint32_t control, struct trb *ev) {
    ring_put(X.cmdr, &X.cmd_i, &X.cmd_cyc, param, 0, control);
    wr32(X.db, 0, 0);
    return wait_event(TRB_CMD_DONE, ev);
}

/* DDR-1162: DCBAA, command ring, event ring, run, then one Enable Slot. */
static int xhci_rings_up(volatile uint8_t *cap, volatile uint8_t *op, uint32_t slots) {
    uint64_t dcbaa_p, cmd_p, evt_p, erst_p;
    uint64_t *dcbaa = zpage(&dcbaa_p);
    struct trb *cmdr = zpage(&cmd_p);
    struct trb *evtr = zpage(&evt_p);
    uint64_t *erst = zpage(&erst_p);
    if (!dcbaa || !cmdr || !evtr || !erst) { kputs("[xhci] FAIL alloc\r\n"); return -1; }

    uint32_t hcs2 = rd32(cap, CAP_HCSPARAMS2);
    uint32_t nsp  = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (nsp) {
        uint64_t sp_p; uint64_t *sp = zpage(&sp_p);
        if (!sp || nsp > PAGE_SIZE / 8) { kputs("[xhci] FAIL scratch\r\n"); return -1; }
        for (uint32_t i = 0; i < nsp; i++) {
            uint64_t pg; if (!zpage(&pg)) { kputs("[xhci] FAIL scratch\r\n"); return -1; }
            sp[i] = pg;
        }
        dcbaa[0] = sp_p;
    }
    wr32(op, OP_CONFIG, slots & 0xFF);
    wr64(op, OP_DCBAAP, dcbaa_p);

    /* Command ring: producer cycle 1, Link TRB at the end toggles the cycle. */
    cmdr[RING_TRBS - 1].param   = cmd_p;
    cmdr[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | (1u << 1);
    wr64(op, OP_CRCR, cmd_p | 1u);

    /* Event ring, interrupter 0: ERSTSZ, ERDP, then ERSTBA. */
    volatile uint8_t *ir = cap + (rd32(cap, CAP_RTSOFF) & ~0x1Fu) + 0x20;
    erst[0] = evt_p;
    erst[1] = RING_TRBS;
    wr32(ir, 0x08, 1);
    wr64(ir, 0x18, evt_p);
    wr64(ir, 0x10, erst_p);

    X.op = op; X.ir = ir; X.db = cap + (rd32(cap, CAP_DBOFF) & ~0x3u);
    X.cmdr = cmdr; X.evtr = evtr; X.cmd_p = cmd_p; X.evt_p = evt_p;
    X.cmd_i = 0; X.cmd_cyc = 1; X.evt_i = 0; X.evt_cyc = 1;
    X.dcbaa = dcbaa;
    X.csz = (rd32(cap, CAP_HCCPARAMS1) & (1u << 2)) ? 64 : 32;

    wr32(op, OP_USBCMD, rd32(op, OP_USBCMD) | USBCMD_RS);
    if (wait_reg(op, OP_USBSTS, USBSTS_HCH, 0) < 0) { kputs("[xhci] FAIL run\r\n"); return -1; }
    kputs("[xhci] running\r\n");

    struct trb ev;
    if (!command(0, TRB_TYPE(TRB_ENABLE_SLOT), &ev)) {
        kputs("[xhci] FAIL enable-slot\r\n");
        return -1;
    }
    uint32_t cc = ev.status >> 24, slot = ev.control >> 24;
    kputs("[xhci] enable-slot cc="); kputdec(cc);
    kputs(" slot=");                 kputdec(slot);
    kputs("\r\n");
    return cc == 1 ? (int)slot : -1;
}

/* DDR-1164: root-hub port reset. PORTSC change bits are RW1C and PED is
 * write-1-to-disable, so never write back what was read: write PP|PR only. */
#define PORTSC_PED  (1u << 1)
#define PORTSC_PR   (1u << 4)
#define PORTSC_PP   (1u << 9)
#define PORTSC_PRC  (1u << 21)
static int xhci_port_reset(uint32_t port) {
    volatile uint8_t *op = X.op;
    uint32_t ped0 = (rd32(op, OP_PORTSC(port)) & PORTSC_PED) ? 1 : 0;
    wr32(op, OP_PORTSC(port), PORTSC_PP | PORTSC_PR);
    if (wait_reg(op, OP_PORTSC(port), PORTSC_PRC, PORTSC_PRC) < 0) {
        kputs("[xhci] FAIL port-reset\r\n");
        return -1;
    }
    wr32(op, OP_PORTSC(port), PORTSC_PP | PORTSC_PRC);
    uint32_t ped = (rd32(op, OP_PORTSC(port)) & PORTSC_PED) ? 1 : 0;
    kputs("[xhci] port-reset port="); kputdec(port);
    kputs(" ped0=");                  kputdec(ped0);
    kputs(" ped=");                   kputdec(ped);
    kputs("\r\n");
    return ped ? 0 : -1;
}

/* DDR-1164: one EP0 control transfer. len == 0 is a no-data request. Returns
 * the completion code; *got receives the transferred byte count. */
static uint32_t control(uint32_t slot, uint64_t setup, uint64_t buf_p,
                        uint32_t len, uint32_t *got) {
    struct trb ev;
    uint32_t trt = len ? (3u << 16) : 0;               /* IN data stage or none */
    ring_put(X.tr, &X.tr_i, &X.tr_cyc, setup, 8,
             TRB_TYPE(TRB_SETUP) | TRB_IDT | trt);
    if (len)
        ring_put(X.tr, &X.tr_i, &X.tr_cyc, buf_p, len, TRB_TYPE(TRB_DATA) | (1u << 16));
    /* Status stage direction is opposite the data stage; IN when there is none. */
    ring_put(X.tr, &X.tr_i, &X.tr_cyc, 0, 0,
             TRB_TYPE(TRB_STATUS) | TRB_IOC | (len ? 0 : (1u << 16)));
    wr32(X.db, 4 * slot, 1);                           /* doorbell slot, target EP0 */
    if (!wait_event(TRB_XFER_EVENT, &ev))
        return 0;
    if (got)
        *got = len - (ev.status & 0xFFFFFF);
    return ev.status >> 24;
}

#define SETUP(rt, rq, val, idx, len) \
    ((uint64_t)(rt) | ((uint64_t)(rq) << 8) | ((uint64_t)(val) << 16) | \
     ((uint64_t)(idx) << 32) | ((uint64_t)(len) << 48))

/* DDR-1163: Address Device on `port`, then GET_DESCRIPTOR(DEVICE) on EP0. */
static int xhci_address(uint32_t slot, uint32_t port, uint32_t speed) {
    uint64_t in_p, out_p, tr_p, buf_p;
    uint8_t *in = zpage(&in_p), *out = zpage(&out_p), *buf = zpage(&buf_p);
    struct trb *tr = zpage(&tr_p);
    if (!in || !out || !tr || !buf) { kputs("[xhci] FAIL alloc\r\n"); return -1; }
    X.dcbaa[slot] = out_p;

    tr[RING_TRBS - 1].param   = tr_p;
    tr[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | (1u << 1);
    X.tr = tr; X.tr_i = 0; X.tr_cyc = 1;

    uint32_t mps = speed == 4 ? 512 : speed == 3 ? 64 : 8;
    uint32_t *icc = (uint32_t *)in;                   /* input control context */
    uint32_t *sc  = (uint32_t *)(in + X.csz);         /* slot context */
    uint32_t *ep0 = (uint32_t *)(in + 2 * X.csz);     /* EP0 context */
    icc[1] = 3u;                                      /* A0 | A1 */
    sc[0]  = (speed << 20) | (1u << 27);              /* speed, context entries 1 */
    sc[1]  = port << 16;                              /* root hub port number */
    ep0[1] = (3u << 1) | (4u << 3) | (mps << 16);     /* CErr 3, Control, MPS */
    ep0[2] = (uint32_t)tr_p | 1u;                     /* TR dequeue, DCS 1 */
    ep0[3] = (uint32_t)(tr_p >> 32);
    ep0[4] = 8;                                       /* average TRB length */

    struct trb ev;
    if (!command(in_p, TRB_TYPE(TRB_ADDRESS_DEV) | (slot << 24), &ev) || (ev.status >> 24) != 1) {
        kputs("[xhci] FAIL address\r\n");
        return -1;
    }
    kputs("[xhci] address port="); kputdec(port);
    kputs(" slot=");               kputdec(slot);
    kputs(" cc=");                 kputdec(ev.status >> 24);
    kputs("\r\n");

    uint32_t len = 0;
    uint32_t cc = control(slot, SETUP(0x80, 6, 0x0100, 0, 18), buf_p, 18, &len);
    if (!cc) { kputs("[xhci] FAIL desc\r\n"); return -1; }
    kputs("[xhci] device-desc cc="); kputdec(cc);
    kputs(" len=");   kputdec(len);
    kputs(" type=");  kputdec(buf[1]);
    kputs(" usb=");   kputhex(buf[2] | (buf[3] << 8));
    kputs(" mps0=");  kputdec(buf[7]);
    kputs(" vid=");   kputhex(buf[8] | (buf[9] << 8));
    kputs(" pid=");   kputhex(buf[10] | (buf[11] << 8));
    kputs("\r\n");
    if (cc != 1 || len != 18 || buf[1] != 1)
        return -1;

    /* DDR-1164: configuration descriptor, header first for wTotalLength. */
    cc = control(slot, SETUP(0x80, 6, 0x0200, 0, 9), buf_p, 9, &len);
    uint32_t total = buf[2] | (buf[3] << 8);
    if (cc != 1 || len != 9 || buf[1] != 2 || total < 9) {
        kputs("[xhci] FAIL config-desc\r\n");
        return -1;
    }
    if (total > 256)
        total = 256;
    cc = control(slot, SETUP(0x80, 6, 0x0200, 0, total), buf_p, total, &len);
    if (cc != 1) { kputs("[xhci] FAIL config-desc\r\n"); return -1; }
    uint32_t cfg = buf[5], ifaces = buf[4];
    uint32_t cls = 0, sub = 0, proto = 0, epa = 0, attr = 0, emps = 0, ival = 0, ifn = 0;
    int have_if = 0;
    for (uint32_t o = 0; o + 2 <= len && buf[o] >= 2; o += buf[o]) {
        if (o + buf[o] > len) break;
        if (buf[o + 1] == 4 && !have_if) {            /* interface */
            ifn = buf[o + 2];
            cls = buf[o + 5]; sub = buf[o + 6]; proto = buf[o + 7]; have_if = 1;
        } else if (buf[o + 1] == 5 && have_if && !epa) {  /* endpoint */
            epa = buf[o + 2]; attr = buf[o + 3];
            emps = buf[o + 4] | (buf[o + 5] << 8); ival = buf[o + 6];
        }
    }
    kputs("[xhci] config-desc cc="); kputdec(cc);
    kputs(" total=");  kputdec(len);
    kputs(" cfg=");    kputdec(cfg);
    kputs(" ifaces="); kputdec(ifaces);
    kputs(" class=");  kputdec(cls);
    kputs(" sub=");    kputdec(sub);
    kputs(" proto=");  kputdec(proto);
    kputs(" ep=");     kputhex(epa);
    kputs(" attr=");   kputdec(attr);
    kputs(" mps=");    kputdec(emps);
    kputs(" ival=");   kputdec(ival);
    kputs("\r\n");

    /* SET_CONFIGURATION, then read it back: got= comes from the device. */
    uint32_t scc = control(slot, SETUP(0x00, 9, cfg, 0, 0), 0, 0, 0);
    buf[0] = 0xEE;
    uint32_t gcc = control(slot, SETUP(0x80, 8, 0, 0, 1), buf_p, 1, &len);
    kputs("[xhci] set-config cfg="); kputdec(cfg);
    kputs(" cc=");  kputdec(scc);
    kputs(" got="); kputdec(gcc == 1 && len == 1 ? buf[0] : 255);
    kputs("\r\n");
    if (scc != 1 || gcc != 1 || buf[0] != cfg)
        return -1;
    if (cls != 3 || sub != 1 || epa != 0x81 || (attr & 3) != 3)
        return 0;                                 /* not a boot HID device */

    /* DDR-1165: Configure Endpoint for interrupt IN 0x81 (DCI 3). */
    uint64_t itr_p; struct trb *itr = zpage(&itr_p);
    if (!itr) { kputs("[xhci] FAIL alloc\r\n"); return -1; }
    itr[RING_TRBS - 1].param   = itr_p;
    itr[RING_TRBS - 1].control = TRB_TYPE(TRB_LINK) | (1u << 1);
    uint32_t itr_i = 0, itr_cyc = 1;
    for (uint32_t i = 0; i < 6 * X.csz; i++) in[i] = 0;
    icc[1] = 1u | (1u << 3);                          /* A0 | A3 */
    sc[0]  = (speed << 20) | (3u << 27);              /* context entries 3 */
    sc[1]  = port << 16;
    uint32_t *ep = (uint32_t *)(in + 4 * X.csz);      /* DCI 3 */
    uint32_t exp = ival ? ival - 1 : 0;               /* HS: 2^(bInterval-1) uframes */
    ep[0] = exp << 16;
    ep[1] = (3u << 1) | (7u << 3) | (emps << 16);     /* CErr 3, Interrupt IN */
    ep[2] = (uint32_t)itr_p | 1u;
    ep[3] = (uint32_t)(itr_p >> 32);
    ep[4] = 8u | (emps << 16);                        /* avg TRB len, max ESIT */
    if (!command(in_p, TRB_TYPE(TRB_CONFIG_EP) | (slot << 24), &ev)) {
        kputs("[xhci] FAIL configure-ep\r\n");
        return -1;
    }
    kputs("[xhci] configure-ep ep="); kputhex(epa);
    kputs(" dci=3 cc=");              kputdec(ev.status >> 24);
    kputs("\r\n");
    if ((ev.status >> 24) != 1)
        return -1;

    uint32_t pcc = control(slot, SETUP(0x21, 0x0B, 0, ifn, 0), 0, 0, 0);
    kputs("[xhci] set-protocol cc="); kputdec(pcc); kputs("\r\n");
    if (pcc != 1)
        return -1;

    /* DDR-1167: arm ONE Normal TRB and hand the ring to xhci_poll(). Every
     * command has completed by now, so from here the event ring has a single
     * consumer: the BSP timer tick. hid_live is set LAST. */
    H.itr = itr; H.itr_i = itr_i; H.itr_cyc = itr_cyc;
    H.buf = buf; H.buf_p = buf_p; H.slot = slot;
    for (int i = 0; i < 8; i++) H.prev[i] = 0;
    hid_queue();
    kputs("[xhci] hid-armed\r\n");
    __atomic_store_n(&H.live, 1, __ATOMIC_RELEASE);
    return 0;
}

/* DDR-1167 §3: HID boot-protocol usage -> set-1 make code. 0 = unmapped.
 * Bit 0x100 marks an E0-prefixed (extended) key. */
static const uint16_t g_usage_sc[0x53] = {
    [4]=0x1E,[5]=0x30,[6]=0x2E,[7]=0x20,[8]=0x12,[9]=0x21,[10]=0x22,[11]=0x23,
    [12]=0x17,[13]=0x24,[14]=0x25,[15]=0x26,[16]=0x32,[17]=0x31,[18]=0x18,
    [19]=0x19,[20]=0x10,[21]=0x13,[22]=0x1F,[23]=0x14,[24]=0x16,[25]=0x2F,
    [26]=0x11,[27]=0x2D,[28]=0x15,[29]=0x2C,
    [30]=0x02,[31]=0x03,[32]=0x04,[33]=0x05,[34]=0x06,[35]=0x07,[36]=0x08,
    [37]=0x09,[38]=0x0A,[39]=0x0B,
    [40]=0x1C,[41]=0x01,[42]=0x0E,[43]=0x0F,[44]=0x39,[45]=0x0C,[46]=0x0D,
    [47]=0x1A,[48]=0x1B,[49]=0x2B,[51]=0x27,[52]=0x28,[53]=0x29,[54]=0x33,
    [55]=0x34,[56]=0x35,[57]=0x3A,
    [58]=0x3B,[59]=0x3C,[60]=0x3D,[61]=0x3E,[62]=0x3F,[63]=0x40,[64]=0x41,
    [65]=0x42,[66]=0x43,[67]=0x44,[68]=0x57,[69]=0x58,
    [73]=0x152,[74]=0x147,[75]=0x149,[76]=0x153,[77]=0x14F,[78]=0x151,
    [79]=0x14D,[80]=0x14B,[81]=0x150,[82]=0x148,
};
static const uint16_t g_mod_sc[8] = {
    0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C,
};

static void hid_emit(uint16_t code, int brk) {
    if (!code) return;
    if (code & 0x100) ps2kbd_feed(0xE0);
    ps2kbd_feed((uint8_t)((code & 0xFF) | (brk ? 0x80 : 0)));
}

static int hid_has(const uint8_t *r, uint8_t u) {
    for (int i = 2; i < 8; i++) if (r[i] == u) return 1;
    return 0;
}

static void hid_report(const uint8_t *r) {
    for (int i = 2; i < 8; i++)
        if (r[i] == 1) return;              /* ErrorRollOver: no key state */
    uint8_t dm = r[0] ^ H.prev[0];
    for (int b = 0; b < 8; b++)
        if (dm & (1u << b)) hid_emit(g_mod_sc[b], !(r[0] & (1u << b)));
    for (int i = 2; i < 8; i++) {
        uint8_t u = H.prev[i];
        if (u && u < sizeof g_usage_sc / sizeof g_usage_sc[0] && !hid_has(r, u))
            hid_emit(g_usage_sc[u], 1);
    }
    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u && u < sizeof g_usage_sc / sizeof g_usage_sc[0] && !hid_has(H.prev, u))
            hid_emit(g_usage_sc[u], 0);
    }
    for (int i = 0; i < 8; i++) H.prev[i] = r[i];
}

/* DDR-1167 §4: called from timer_tick(), which runs ONLY on the BSP, and
 * IRQ1 (the other ps2kbd_feed writer) is PIC-routed to the BSP. Both enter via
 * interrupt gates, so the unlocked decoder stays single-CPU and serialised.
 * Moving either off the BSP must add a lock in the same commit. */
void xhci_poll(void) {
    if (!__atomic_load_n(&H.live, __ATOMIC_ACQUIRE))
        return;
    for (;;) {
        volatile struct trb *e = &X.evtr[X.evt_i];
        if ((e->control & 1u) != X.evt_cyc)
            return;
        struct trb ev = { e->param, e->status, e->control };
        if (++X.evt_i == RING_TRBS) { X.evt_i = 0; X.evt_cyc ^= 1u; }
        wr64(X.ir, 0x18, (X.evt_p + X.evt_i * sizeof(struct trb)) | (1u << 3));
        if (((ev.control >> 10) & 0x3F) != TRB_XFER_EVENT ||
            ((ev.control >> 16) & 0x1F) != 3)
            continue;                       /* port change etc.: ignored */
        uint8_t r[8];
        for (int i = 0; i < 8; i++) r[i] = H.buf[i];
        if (!H.reported && r[2]) {
            H.reported = 1;
            kputs("[xhci] hid-report cc="); kputdec(ev.status >> 24);
            kputs(" mod=");                  kputdec(r[0]);
            kputs(" k0=");                   kputdec(r[2]);
            kputs("\r\n");
        }
        if ((ev.status >> 24) == 1) {
            hid_report(r);
            /* DDR-1167 §6: a re-queue that never happens yields exactly one
             * report, so the 4th says the ring is being re-armed. */
            if (++H.nrep == 4)
                kputs("[xhci] hid-reports n=4\r\n");
        }
        hid_queue();
    }
}

void xhci_init(uint8_t bus, uint8_t dev, uint8_t func) {
    if (g_xhci_up)
        return;                                 /* one controller this piece */

    uint32_t cmd = pcie_read32(bus, dev, func, 0x04);
    cmd |= (1u << 1) | (1u << 2);               /* memory space + bus master */
    pcie_write32(bus, dev, func, 0x04, cmd);

    uint32_t lo = pcie_read32(bus, dev, func, 0x10);
    uint64_t base = lo & 0xFFFFFFF0u;
    if (((lo >> 1) & 3) == 2)                   /* 64-bit memory BAR */
        base |= (uint64_t)pcie_read32(bus, dev, func, 0x14) << 32;
    if (!base) {
        kputs("[xhci] FAIL bar\r\n");
        return;
    }
    for (uint32_t off = 0; off < XHCI_BAR_MAP; off += PAGE_SIZE)
        vmm_map(XHCI_BAR_VBASE + off, base + off, VMM_RW | VMM_PCD);
    volatile uint8_t *cap = (volatile uint8_t *)XHCI_BAR_VBASE;

    /* CAPLENGTH and HCIVERSION share dword 0. Read it as a dword: QEMU's
     * capability region returns 0 for a 16-bit read at offset 2 (measured). */
    uint32_t cap0   = rd32(cap, CAP_CAPLENGTH);
    uint8_t  caplen = (uint8_t)(cap0 & 0xFF);
    uint32_t hciver = cap0 >> 16;
    uint32_t hcs1   = rd32(cap, CAP_HCSPARAMS1);
    uint32_t slots  = hcs1 & 0xFF;
    uint32_t ports  = (hcs1 >> 24) & 0xFF;
    volatile uint8_t *op = cap + caplen;

    /* Halt, then reset. The spec requires HCH=1 before HCRST is set. */
    wr32(op, OP_USBCMD, rd32(op, OP_USBCMD) & ~USBCMD_RS);
    if (wait_reg(op, OP_USBSTS, USBSTS_HCH, USBSTS_HCH) < 0) {
        kputs("[xhci] FAIL halt\r\n");
        return;
    }
    wr32(op, OP_USBCMD, USBCMD_HCRST);
    if (wait_reg(op, OP_USBCMD, USBCMD_HCRST, 0) < 0 ||
        wait_reg(op, OP_USBSTS, USBSTS_CNR, 0) < 0) {
        kputs("[xhci] FAIL reset\r\n");
        return;
    }
    g_xhci_up = 1;

    kputs("[xhci] up hciver="); kputhex(hciver);
    kputs(" slots=");           kputdec(slots);
    kputs(" ports=");           kputdec(ports);
    kputs("\r\n");

    unsigned connected = 0;
    uint32_t first_port = 0, first_speed = 0;
    for (uint32_t p = 1; p <= ports && p <= 255; p++) {
        uint32_t sc = rd32(op, OP_PORTSC(p));
        if (!(sc & PORTSC_CCS))
            continue;
        connected++;
        if (!first_port) { first_port = p; first_speed = (sc >> 10) & 0xF; }
        kputs("[xhci] port="); kputdec(p);
        kputs(" ccs=1 speed="); kputdec((sc >> 10) & 0xF);
        kputs("\r\n");
    }
    kputs("[xhci] connected="); kputdec(connected); kputs("\r\n");

    int slot = xhci_rings_up(cap, op, slots);
    if (slot < 0 || !first_port)
        return;
    if (xhci_port_reset(first_port) < 0)
        return;
    xhci_address((uint32_t)slot, first_port, first_speed);
}
