/* kernel/drivers/rtc/rtc.h — CMOS/RTC wall-clock (Phase 4j).
 *
 * Reads the battery-backed real-time clock (CMOS ports 0x70/0x71) so the kernel
 * has a wall-clock for filesystem timestamps now and CLOCK_REALTIME later
 * (Layer 5/7). Handles BCD vs binary and 12/24-hour per CMOS status register B.
 */
#pragma once
#include <stdint.h>

struct rtc_time {
    uint16_t year;     /* full year, e.g. 2026 */
    uint8_t  month;    /* 1..12 */
    uint8_t  day;      /* 1..31 */
    uint8_t  hour;     /* 0..23 */
    uint8_t  minute;   /* 0..59 */
    uint8_t  second;   /* 0..59 */
};

/* Read a stable RTC reading (retries across an update-in-progress). */
void     rtc_now(struct rtc_time *out);

/* Pack the current time into a FAT directory entry's date+time fields:
 * return value high 16 bits = FAT date, low 16 bits = FAT time. */
uint32_t rtc_fat_datetime(void);
