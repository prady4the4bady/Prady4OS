/* kernel/drivers/rtc/rtc.c — CMOS/RTC wall-clock (see rtc.h). */
#include "rtc.h"
#include "io.h"
#include "spinlock.h"     /* DDR-796: CMOS index/data is global chipset state */

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

/* DDR-796 (BUG-1). CMOS access is a TWO-PORT sequence over state that belongs to
 * the chipset, not to the calling CPU: 0x70 latches a register index, 0x71 then
 * reads whatever that index currently selects. Under -smp 4 two CPUs interleave
 *
 *     CPU A: outb(0x70, 0x00)    select seconds
 *     CPU B: outb(0x70, 0x04)    select hours
 *     CPU A: inb(0x71)           reads HOURS as if it were seconds
 *
 * and the wall clock can then jump in either direction. Every consumer that
 * compares two readings breaks: the DDR-730 metrics probe collapsed its whole
 * 120-second window to zero this way, which is what BUG-1 actually was.
 *
 * rtc_now's "read until two readings agree" loop does NOT cover this — that loop
 * guards against an RTC update-in-progress, a different hazard. Two racing CPUs
 * can agree twice on the same corrupt value while the interleave persists.
 */
static spinlock_t g_rtc_lock;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}
static int rtc_updating(void) {
    outb(CMOS_INDEX, 0x0A);
    return inb(CMOS_DATA) & 0x80;          /* status A bit 7: update in progress */
}
static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + ((v >> 4) * 10)); }

void rtc_now(struct rtc_time *out) {
    uint8_t s, m, h, d, mo, y, ls, lm, lh, ld, lmo, ly;
    /* DDR-796: the lock spans the WHOLE reading, not each cmos_read.
     *
     * Locking per-cmos_read would make one index/data pair atomic while still
     * letting another CPU move the index between the PAIRED readings the
     * consistency loop below compares — so the loop could still agree on two
     * corrupt values. The invariant that has to hold is "no other CPU touches
     * port 0x70 for the duration of one coherent reading", which is a property
     * of rtc_now, not of cmos_read.
     *
     * IRQ-saving so this CPU cannot re-enter through a handler mid-sequence and
     * leave the index pointing elsewhere. The hold spans the rtc_updating()
     * spin, which can reach ~2 ms during an update tick; that is accepted
     * deliberately — rtc_now is a rare call, and a bounded 2 ms hold beats a
     * wall clock that can run backwards. */
    uint64_t flags = spin_lock_irqsave(&g_rtc_lock);
    /* Read repeatedly until two back-to-back reads agree (no update midway). */
    while (rtc_updating()) { }
    s = cmos_read(0x00); m = cmos_read(0x02); h = cmos_read(0x04);
    d = cmos_read(0x07); mo = cmos_read(0x08); y = cmos_read(0x09);
    do {
        ls = s; lm = m; lh = h; ld = d; lmo = mo; ly = y;
        while (rtc_updating()) { }
        s = cmos_read(0x00); m = cmos_read(0x02); h = cmos_read(0x04);
        d = cmos_read(0x07); mo = cmos_read(0x08); y = cmos_read(0x09);
    } while (s != ls || m != lm || h != lh || d != ld || mo != lmo || y != ly);

    uint8_t regB = cmos_read(0x0B);
    /* Every CMOS port access is done; the decoding below touches no I/O port, so
     * the lock is released here rather than held across arithmetic. */
    spin_unlock_irqrestore(&g_rtc_lock, flags);

    int binary = regB & 0x04;               /* bit 2: 1 = binary, 0 = BCD */
    int h24    = regB & 0x02;               /* bit 1: 1 = 24-hour          */

    int pm = (!h24) && (h & 0x80);
    uint8_t hr = h & 0x7F;                   /* strip the PM flag before decode */
    if (!binary) {
        s = bcd2bin(s); m = bcd2bin(m); d = bcd2bin(d);
        mo = bcd2bin(mo); y = bcd2bin(y); hr = bcd2bin(hr);
    }
    if (!h24) {                              /* 12-hour -> 24-hour */
        if (hr == 12) hr = 0;
        if (pm) hr = (uint8_t)(hr + 12);
    }

    out->second = s;
    out->minute = m;
    out->hour   = hr;
    out->day    = d;
    out->month  = mo;
    out->year   = (uint16_t)(2000 + y);      /* 2-digit century register; assume 21st */
}

uint32_t rtc_fat_datetime(void) {
    struct rtc_time t;
    rtc_now(&t);
    uint16_t yr = (t.year >= 1980) ? t.year : 1980;
    uint8_t  mo = (t.month >= 1 && t.month <= 12) ? t.month : 1;
    uint8_t  dy = (t.day >= 1 && t.day <= 31) ? t.day : 1;
    uint16_t date = (uint16_t)(((yr - 1980) << 9) | (mo << 5) | dy);
    uint16_t time = (uint16_t)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
    return ((uint32_t)date << 16) | time;
}
