/* third_party/lwip-port/stdio.h — minimal freestanding <stdio.h> for lwIP.
 * Some lwIP files include <stdio.h> unconditionally; with LWIP_DEBUG=0,
 * LWIP_STATS_DISPLAY=0 and LWIP_PLATFORM_DIAG/ASSERT overridden (arch/cc.h),
 * lwIP does not actually call these at runtime — they are declared only so the
 * third-party sources compile under the freestanding kernel toolchain. */
#ifndef PRADYOS_LWIP_STDIO_H
#define PRADYOS_LWIP_STDIO_H
#include <stddef.h>
#include <stdarg.h>
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
#endif
