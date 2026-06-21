/* kernel/exec/elf.h — static ELF64 loader (Phase 5a).
 *
 * Loads an ET_EXEC x86-64 image into a fresh per-process address space with
 * W^X enforced (see ADR-021) and spawns a ready ring-3 thread for it. The
 * console capability is delivered to the program in RDI at entry (5a
 * convention; POSIX I/O arrives in 5b).
 */
#pragma once
#include <stdint.h>

struct tcb;

/* Returns 0 and sets *out to the new ring-3 thread on success; a negative
 * elf_err on any validation, W^X, or resource failure (the partially built
 * address space is torn down before returning). */
int elf_load(const void *image, uint64_t image_len, const char *name,
             struct tcb **out);

/* Loader core shared by elf_load and sys_execve: validate the image, build a
 * fresh W^X address space, map its PT_LOAD segments, and lay out the 8 MiB user
 * stack with the SysV ABI initial frame — WITHOUT creating a thread. On success
 * returns ELF_OK and fills *out_as (a CR3), *out_entry (e_entry), and *out_rsp
 * (initial user RSP); on failure returns a negative elf_err with any partial
 * address space already torn down. */
int elf_build_image(const void *image, uint64_t image_len, const char *name,
                    uint64_t *out_as, uint64_t *out_entry, uint64_t *out_rsp);

enum elf_err {
    ELF_OK            =  0,
    ELF_E_ARGS        = -1,   /* null/short image                       */
    ELF_E_MAGIC       = -2,   /* not ELF / wrong class / endian / version */
    ELF_E_MACHINE     = -3,   /* not x86-64 ET_EXEC                      */
    ELF_E_PHDR        = -4,   /* program-header table out of bounds      */
    ELF_E_SEGMENT     = -5,   /* segment file/addr range invalid         */
    ELF_E_WX          = -6,   /* segment requested write+execute (W^X)   */
    ELF_E_RANGE       = -7,   /* segment outside the user address range  */
    ELF_E_NOMEM       = -8,   /* out of frames / address space           */
    ELF_E_THREAD      = -9,   /* thread creation failed                  */
};
