/* kernel/exec/elf.c — static ELF64 loader with W^X (Phase 5a, ADR-021).
 *
 * The image is copied into freshly allocated physical frames through the kernel
 * identity map, then mapped into the new process address space with permissions
 * derived from each PT_LOAD's p_flags. The kernel writes file bytes via the
 * frame's physical (identity) address, so a text segment can be populated even
 * though its user mapping is read-execute only — no page is ever writable and
 * executable at once.
 */
#include "elf.h"
#include "vmm.h"
#include "kheap.h"
#include "pmm.h"
#include "sched.h"
#include "cap.h"
#include "syscall.h"
#include "string.h"
#include "vfs.h"        /* FS_RES_ID + vfs_default_mnt for the 5b FS grant */
#include "vdso_page.h"  /* vdso_map_user — read-only clock page (IMP-C) */
#include "metric_page.h" /* metric_page_map_user — sealed objective root (DDR-795) */

/* --- ELF64 on-disk format (System V gABI; a public spec, clean-room) -------- */

struct elf64_ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

_Static_assert(sizeof(struct elf64_ehdr) == 64, "Elf64_Ehdr must be 64 bytes");
_Static_assert(sizeof(struct elf64_phdr) == 56, "Elf64_Phdr must be 56 bytes");

#define ET_EXEC      2
#define EM_X86_64    62
#define PT_LOAD      1
#define PF_X         0x1
#define PF_W         0x2
#define PF_R         0x4

#define ELFCLASS64   2
#define ELFDATA2LSB  1
#define EV_CURRENT   1

#define AT_NULL      0
#define AT_PAGESZ    6

/* --- user address-space layout (PML4 slot 1: [512 GiB, 1 TiB), per ADR-021) - */
#define USER_MIN         0x8000000000ull            /* 512 GiB                  */
#define USER_MAX         0x10000000000ull           /* 1 TiB                    */
/* ADR-038: USER_STACK_TOP / _SIZE / _BOT now live in vmm.h, because the #PF
 * growth path needs the SAME range test this loader maps against. Defining them
 * in two places is the mirrored-definition bug class (DDR-822/825): the two
 * copies drift and the guard silently stops being a guard. One definition. */

static inline uint64_t page_down(uint64_t x) { return x & ~((uint64_t)PAGE_SIZE - 1); }
static inline uint64_t page_up(uint64_t x)   { return (x + PAGE_SIZE - 1) & ~((uint64_t)PAGE_SIZE - 1); }

static uint64_t kstrlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

/* Map one freshly zeroed user page into `as` at `va` with `flags`, copying any
 * overlapping file bytes from the image first (via the frame's identity view).
 * Returns the frame phys, or 0 on failure. */
static uint64_t load_page(uint64_t as, uint64_t va, uint64_t flags,
                          const uint8_t *image, uint64_t seg_vaddr,
                          uint64_t file_off, uint64_t file_sz) {
    void *frame = ptnode_alloc();                  /* zeroed -> BSS/padding is 0 */
    if (!frame)
        return 0;
    uint64_t phys = (uint64_t)(uintptr_t)frame;

    /* Intersect this page [va, va+PAGE) with the file-backed part of the segment
     * [seg_vaddr, seg_vaddr+file_sz). */
    uint64_t copy_lo = va > seg_vaddr ? va : seg_vaddr;
    uint64_t seg_file_end = seg_vaddr + file_sz;
    uint64_t page_end = va + PAGE_SIZE;
    uint64_t copy_hi = page_end < seg_file_end ? page_end : seg_file_end;
    if (copy_hi > copy_lo) {
        uint64_t page_off = copy_lo - va;
        uint64_t seg_off  = copy_lo - seg_vaddr;
        memcpy((uint8_t *)(uintptr_t)phys + page_off,
               image + file_off + seg_off, (size_t)(copy_hi - copy_lo));
    }

    if (vmm_map_in(as, va, phys, flags) != 0) {
        ptnode_free(frame);
        return 0;
    }
    return phys;
}

int elf_build_image(const void *image, uint64_t image_len, const char *name,
                    const struct exec_args *args,
                    uint64_t *out_as, uint64_t *out_entry, uint64_t *out_rsp) {
    if (!image || image_len < sizeof(struct elf64_ehdr) ||
        !out_as || !out_entry || !out_rsp)
        return ELF_E_ARGS;

    const uint8_t *img = (const uint8_t *)image;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)image;

    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F'  ||
        eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB ||
        eh->e_ident[6] != EV_CURRENT)
        return ELF_E_MAGIC;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64)
        return ELF_E_MACHINE;
    if (eh->e_phentsize < sizeof(struct elf64_phdr) || eh->e_phnum == 0)
        return ELF_E_PHDR;
    /* program-header table fully inside the image */
    uint64_t ph_bytes = (uint64_t)eh->e_phnum * eh->e_phentsize;
    if (eh->e_phoff > image_len || ph_bytes > image_len - eh->e_phoff)
        return ELF_E_PHDR;

    uint64_t as = vmm_new_address_space();
    if (!as)
        return ELF_E_NOMEM;

    int rc = ELF_OK;
    for (uint16_t i = 0; i < eh->e_phnum && rc == ELF_OK; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(img + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;

        /* file range valid */
        if (ph->p_filesz > ph->p_memsz ||
            ph->p_offset > image_len || ph->p_filesz > image_len - ph->p_offset) {
            rc = ELF_E_SEGMENT;
            break;
        }
        /* W^X: never both writable and executable */
        if ((ph->p_flags & PF_W) && (ph->p_flags & PF_X)) {
            rc = ELF_E_WX;
            break;
        }
        /* stay inside the user range and clear of the stack + its guard page */
        uint64_t vstart = page_down(ph->p_vaddr);
        uint64_t vend   = page_up(ph->p_vaddr + ph->p_memsz);
        if (ph->p_memsz == 0) continue;
        if (vstart < USER_MIN || vend > USER_STACK_BOT - PAGE_SIZE || vend <= vstart) {
            rc = ELF_E_RANGE;
            break;
        }

        uint64_t flags = VMM_USER;                 /* present is added by map_core */
        if (ph->p_flags & PF_W) flags |= VMM_RW;
        if (!(ph->p_flags & PF_X)) flags |= VMM_NX;   /* W^X: only X stays executable */

        for (uint64_t va = vstart; va < vend; va += PAGE_SIZE) {
            if (!load_page(as, va, flags, img, ph->p_vaddr,
                           ph->p_offset, ph->p_filesz)) {
                rc = ELF_E_NOMEM;
                break;
            }
        }
    }

    if (rc != ELF_OK) {
        vmm_destroy_address_space(as);
        return rc;
    }

    /* --- user stack: 8 MiB RW+NX, guard page below, SysV ABI initial frame --- */
    /* ADR-038: map ONLY the top page here; the rest of the 8 MiB is faulted in
     * on first touch (vmm_stack_fault, called from the #PF path). The eager loop
     * this replaces cost 2048 frames per process whether or not they were ever
     * touched — measured by DDR-943 driving pmmfree to 2096 of 28630, i.e. one
     * stack's headroom. The top page cannot be lazy: elf.c writes the SysV
     * initial frame (argc/argv/envp/auxv) into it just below, through its
     * identity view, before the thread ever runs. */
    uint64_t top_phys = 0;
    for (uint64_t i = 0; i < USER_STACK_EAGER_PAGES; i++) {
        uint64_t va = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        void *frame = ptnode_alloc();
        if (!frame) { vmm_destroy_address_space(as); return ELF_E_NOMEM; }
        uint64_t phys = (uint64_t)(uintptr_t)frame;
        if (vmm_map_in(as, va, phys, VMM_USER | VMM_RW | VMM_NX) != 0) {
            ptnode_free(frame);
            vmm_destroy_address_space(as);
            return ELF_E_NOMEM;
        }
        if (i == 0)
            top_phys = phys;              /* the SysV initial frame lives here */
    }
    /* guard page at [USER_STACK_BOT - PAGE_SIZE, USER_STACK_BOT) is left unmapped. */

    /* Build argc/argv/envp/auxv in the top stack page via its identity view.
     * Pointers stored in the frame are USER virtual addresses. */
    uint8_t *kp = (uint8_t *)(uintptr_t)top_phys;
    uint64_t top_uva = USER_STACK_TOP - PAGE_SIZE;
    const char *prog = name ? name : "prog";
    uint64_t nlen = kstrlen(prog) + 1;

    uint64_t sp = PAGE_SIZE;

    /* DDR-1032: lay the marshalled strings down first and remember each one's
     * USER virtual address, then build the pointer arrays above them. With no
     * args this reduces to exactly the previous frame -- argc=1, argv[0]=path --
     * which is why every existing probe keeps booting unchanged. */
    uint64_t uva[EXEC_ARG_MAX_ENTRIES];
    unsigned nstr = 0;
    unsigned nargc, nenvc;

    if (args && args->argc && args->blob &&
        (unsigned)(args->argc + args->envc) <= EXEC_ARG_MAX_ENTRIES &&
        args->blob_len <= EXEC_ARG_MAX_BYTES) {
        sp -= args->blob_len;
        memcpy(kp + sp, args->blob, (size_t)args->blob_len);
        /* Walk the copy, not the source: the offsets are identical and this
         * cannot run off the end, because blob_len bounds it. */
        uint64_t base = sp;
        for (uint32_t o = 0; o < args->blob_len && nstr < EXEC_ARG_MAX_ENTRIES; ) {
            uva[nstr++] = top_uva + base + o;
            while (o < args->blob_len && kp[base + o]) o++;
            o++;                                   /* step over the NUL */
        }
        nargc = args->argc;
        nenvc = args->envc;
        if (nargc + nenvc > nstr) {                /* blob short of its own counts */
            nargc = nstr; nenvc = 0;
        }
    } else {
        sp -= nlen;
        memcpy(kp + sp, prog, (size_t)nlen);
        uva[0] = top_uva + sp;
        nstr = 1; nargc = 1; nenvc = 0;
    }
    sp &= ~15ull;                                  /* 16-align the structure end */

    /* The frame is 7 fixed slots (4 auxv + 2 terminators + argc) plus one per
     * string. Each is 8 bytes, so RSP lands 16-aligned only when that TOTAL is
     * EVEN -- pad one slot when it is odd. The pad goes first, at the highest
     * address, where nothing reads it.
     *
     * The old fixed frame was 7+1+0 = 8, already even, which is why no pad ever
     * existed and why getting this backwards was easy: the first draft padded on
     * EVEN and shipped a misaligned stack for argc=3/envc=1 (11 slots). Nothing
     * in the assembly receiver can feel that, so `PRADYOS_ARGV_ALIGN` was added
     * to measure RSP at entry -- it read `bad`, which is how this was caught. */
    if (((7u + nargc + nenvc) & 1u) != 0u) { sp -= 8; *(uint64_t *)(kp + sp) = 0; }

    /* push high->low so the final (lowest) slot is argc; RSP stays 16-aligned */
    sp -= 8; *(uint64_t *)(kp + sp) = 0;           /* auxv[1].val  (AT_NULL)     */
    sp -= 8; *(uint64_t *)(kp + sp) = AT_NULL;     /* auxv[1].type               */
    sp -= 8; *(uint64_t *)(kp + sp) = PAGE_SIZE;   /* auxv[0].val                */
    sp -= 8; *(uint64_t *)(kp + sp) = AT_PAGESZ;   /* auxv[0].type               */
    sp -= 8; *(uint64_t *)(kp + sp) = 0;           /* envp terminator            */
    for (unsigned i = nenvc; i-- > 0; ) {
        sp -= 8; *(uint64_t *)(kp + sp) = uva[nargc + i];
    }
    sp -= 8; *(uint64_t *)(kp + sp) = 0;           /* argv terminator            */
    for (unsigned i = nargc; i-- > 0; ) {
        sp -= 8; *(uint64_t *)(kp + sp) = uva[i];
    }
    sp -= 8; *(uint64_t *)(kp + sp) = nargc;       /* argc                       */
    uint64_t user_rsp = top_uva + sp;

    vdso_map_user(as);          /* IMP-C: read-only vDSO clock page in every user AS */
    metric_page_map_user(as);   /* F#68/DDR-795: read-only sealed objective root */

    *out_as    = as;
    *out_entry = eh->e_entry;
    *out_rsp   = user_rsp;
    return ELF_OK;
}

int elf_load(const void *image, uint64_t image_len, const char *name,
             struct tcb **out) {
    if (!out)
        return ELF_E_ARGS;

    uint64_t as, entry, user_rsp;
    int rc = elf_build_image(image, image_len, name, 0, &as, &entry, &user_rsp);
    if (rc != ELF_OK)
        return rc;

    const char *prog = name ? name : "prog";

    /* --- ready ring-3 thread ------------------------------------------------- */
    /* sched_create_user inserts a READY thread into the run ring immediately, so
     * mask interrupts until cr3/user_arg are set — otherwise a timer tick could
     * schedule it with cr3 == 0 (kernel master AS), where its user pages are not
     * mapped, and it would fault on the first ring-3 instruction. */
    uint64_t fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl) :: "memory");
    struct tcb *t = sched_create_user(prog, entry, user_rsp);
    if (t) {
        t->cr3 = as;
        /* exactly one capability delivered in RDI: console display */
        t->user_arg = (uint64_t)cap_create(t->caps, RES_DEVICE, CONSOLE_RES_ID, CAP_DISPLAY);
        /* 5b (ADR-022): grant an FS capability + root mount so the fd-based file
         * syscalls (sys_open/read/...) can reach the VFS on this process's behalf. */
        t->fs_cap   = cap_create(t->caps, RES_FILE, FS_RES_ID, CAP_FS_READ | CAP_FS_WRITE);
        t->root_mnt = vfs_default_mnt();
    }
    __asm__ volatile("push %0; popfq" :: "r"(fl) : "memory", "cc");

    if (!t) {
        vmm_destroy_address_space(as);
        return ELF_E_THREAD;
    }
    *out = t;
    return ELF_OK;
}
