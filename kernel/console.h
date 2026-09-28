/* kernel/console.h — minimal ring-0 console (COM1 serial + VGA text). */
#pragma once
#include <stdint.h>

void kputc(char c);                       /* one byte to COM1 (also captured into the log ring) */
uint32_t klog_read(char *dst, uint32_t max); /* DDR-750: copy recent kernel log bytes */
void console_rx_init(void);                /* arm COM1 RX IRQ + ring buffer (5e) */
int  kgetc_nb(void);                       /* COM1 RX: one buffered byte, or -1 (5e) */
void kputs(const char *s);                /* NUL-terminated string to COM1 */
void kwrite(const char *buf, uint64_t n); /* n bytes to COM1 as one locked unit */
void kputhex(uint64_t v);                 /* "0x" + 16 hex digits to COM1 */
void kputdec(uint64_t v);                 /* unsigned decimal to COM1 */
void kvga_line(const char *s, int row);   /* write a string to one VGA text row */

/* DDR-1055: assemble a multi-part line and emit it as ONE kwrite.
 *
 * Use this -- not several kputs/kputdec calls, and not console_line_lock() --
 * for any line a gate asserts on.  kwrite holds g_console_lock for the whole
 * buffer and every printer in the tree takes that lock, so a kline is atomic
 * against ALL of them, including the ring-3 write(2) path that the line lock
 * below does not exclude.  Overflow emits '[kline] TRUNC' (GLOBAL_FORBIDDEN)
 * rather than silently shortening the line. */
#define KLINE_MAX 256
typedef struct { char b[KLINE_MAX]; unsigned n; unsigned trunc; } kline;
void kline_init(kline *k);
void kline_c(kline *k, char c);
void kline_s(kline *k, const char *s);
void kline_d(kline *k, uint64_t v);          /* unsigned decimal          */
void kline_x(kline *k, uint64_t v);          /* "0x" + 16 hex digits      */
void kline_emit(kline *k);                   /* one kwrite; resets the buffer */

/* DDR-963 §5: line-granularity console lock.
 *
 * kputc/kputs/kputhex/kputdec are each individually atomic (g_console_lock,
 * ADR-030 stage 1), but a LOGICAL line assembled from several of those calls
 * is not: another CPU's printer can land between them. Hold this across such a
 * line. It nests strictly OUTSIDE g_console_lock — order is
 * line -> console -> klog, taken in that order at every site and never the
 * reverse — so it changes neither kputc nor the UART busy-wait.
 *
 * The lock is IRQ-saving: the [hb] heartbeat prints from the timer IRQ -- one
 * line every 500 ticks at the 100 Hz LAPIC rate, i.e. ~5 s per heartbeat -- so
 * a holder that could be preempted by its own CPU's timer would deadlock.
 * Exceptions are not maskable, which is what console_line_trylock() is for. */
uint64_t console_line_lock(void);            /* returns saved flags */
int      console_line_trylock(uint64_t *fl); /* 1 = taken (never blocks) */
void     console_line_unlock(uint64_t fl);
void     console_panic_force_release(void);  /* DDR-970/1009: panic path ONLY */
