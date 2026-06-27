/* third_party/lwip-port/stdlib.h — minimal freestanding <stdlib.h> for lwIP.
 * lwIP's mem.c includes it when MEM_LIBC_MALLOC=1; PRADYOS routes allocation
 * through the mem_clib_* macros (-> pradyos_lwip_* in lwipopts.h), so the libc
 * malloc/free/calloc symbols are not actually called — declared only so the
 * third-party sources compile under the freestanding kernel toolchain. */
#ifndef PRADYOS_LWIP_STDLIB_H
#define PRADYOS_LWIP_STDLIB_H
#include <stddef.h>
void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void  abort(void);
int   atoi(const char *nptr);   /* used by netif name parsing; impl in lwip_port.c */
#endif
