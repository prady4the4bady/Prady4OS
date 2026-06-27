/* third_party/lwip-port/arch/cc.h — PRADYOS lwIP compiler/platform abstraction.
 * Freestanding kernel toolchain: no <inttypes.h>/<errno.h>, no libc stdio. */
#ifndef PRADYOS_LWIP_ARCH_CC_H
#define PRADYOS_LWIP_ARCH_CC_H

#include <stddef.h>
#include <stdint.h>

/* lwIP provides its own errno (no freestanding <errno.h>). */
#define LWIP_PROVIDE_ERRNO          1

/* Printf-style format specifiers (LWIP_NO_INTTYPES_H=1). x86_64 LP64. */
#define X8_F   "02x"
#define U16_F  "u"
#define S16_F  "d"
#define X16_F  "x"
#define U32_F  "u"
#define S32_F  "d"
#define X32_F  "x"
#define SZT_F  "lu"

/* Diagnostics + assertions route to the kernel console (lwip-port shim). DIAG is
 * called as LWIP_PLATFORM_DIAG(("fmt", args...)); ASSERT with a message string. */
void pradyos_lwip_diag(const char *fmt, ...);
void pradyos_lwip_assert(const char *msg);
#define LWIP_PLATFORM_DIAG(x)    do { pradyos_lwip_diag x; } while (0)
#define LWIP_PLATFORM_ASSERT(x)  do { pradyos_lwip_assert(x); } while (0)

#endif /* PRADYOS_LWIP_ARCH_CC_H */
