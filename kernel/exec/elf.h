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
 * address space is torn down before returning).
 * CONTRACT (DDR-boot-authority-race): the thread is returned BLOCKED — the
 * caller sets any authority flags (is_sovereign/is_agent) and then
 * sched_unblock()s it, so no process can run before its authority lands. */
int elf_load(const void *image, uint64_t image_len, const char *name,
             struct tcb **out);

/* DDR-1085: elf_load with a marshalled argv/envp frame (struct exec_args, below).
 * `elf_load` is exactly this with args == 0, so every one of its call sites keeps
 * the DDR-1032 "behave exactly as before" frame -- argc=1, argv[0]=name. Declared
 * after struct exec_args at the bottom of this header. */

/* Loader core shared by elf_load and sys_execve: validate the image, build a
 * fresh W^X address space, map its PT_LOAD segments, and lay out the 8 MiB user
 * stack with the SysV ABI initial frame — WITHOUT creating a thread. On success
 * returns ELF_OK and fills *out_as (a CR3), *out_entry (e_entry), and *out_rsp
 * (initial user RSP); on failure returns a negative elf_err with any partial
 * address space already torn down. */
/* DDR-1032: marshalled argv/envp, FLATTENED. `blob` holds argc+envc
 * NUL-terminated strings back to back -- the first `argc` are argv, the rest
 * envp. Flat rather than a pointer array because the strings must be copied out
 * of the CALLER's address space before execve replaces it, and a kernel-side
 * array of pointers into a kernel blob is the same information with an extra
 * indirection to get wrong. NULL (or argc==0) means "behave exactly as before":
 * argc=1, argv[0]=the path. Every existing caller passes NULL. */
#define EXEC_ARG_MAX_ENTRIES 32
#define EXEC_ARG_MAX_BYTES 1024
struct exec_args {
    const char *blob;
    uint32_t    blob_len;
    uint16_t    argc;
    uint16_t    envc;
};

/* DDR-1189 Amendment 1 (P1-2): the layout elf_build_image drew for the image.
 * The caller stores it in the image's mm (struct mm stack_top, mmap_base). */
struct user_layout {
    uint64_t stack_top;   /* USER_STACK_TOP - r16 pages, r16 < 2^16 */
    uint64_t mmap_base;   /* VMM_MMAP_BASE + r18 pages, r18 < 2^18  */
};

int elf_build_image(const void *image, uint64_t image_len, const char *name,
                    const struct exec_args *args,
                    uint64_t *out_as, uint64_t *out_entry, uint64_t *out_rsp,
                    struct user_layout *out_lay);

/* DDR-1085: see the note beside elf_load. args == 0 is identical to elf_load. */
int elf_load_args(const void *image, uint64_t image_len, const char *name,
                  const struct exec_args *args, struct tcb **out);

enum elf_err {
    ELF_OK            =  0,
    ELF_E_ARGS        = -1,   /* null/short image                       */
    ELF_E_MAGIC       = -2,   /* not ELF / wrong class / endian / version */
    ELF_E_MACHINE     = -3,   /* not x86-64 ET_EXEC or ET_DYN            */
    ELF_E_PHDR        = -4,   /* program-header table out of bounds      */
    ELF_E_SEGMENT     = -5,   /* segment file/addr range invalid         */
    ELF_E_WX          = -6,   /* segment requested write+execute (W^X)   */
    ELF_E_RANGE       = -7,   /* segment outside the user address range  */
    ELF_E_NOMEM       = -8,   /* out of frames / address space           */
    ELF_E_THREAD      = -9,   /* thread creation failed                  */
    ELF_E_INTERP      = -10,  /* ET_DYN with PT_INTERP (P3; DDR-1189 A2)  */
    ELF_E_DYN         = -11,  /* PT_DYNAMIC refused (DDR-1189 A2 P2-2)    */
    ELF_E_RELOC       = -12,  /* relocation refused (DDR-1189 A2 P2-3)    */
    ELF_E_ENTRY       = -13,  /* ET_DYN entry not in an X PT_LOAD (P2-2)  */
};
