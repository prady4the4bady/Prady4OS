/* user/renametest.c — SYS_RENAME probe on the SFS root (DDR-956, gated by DDR-962).
 *
 * DDR-958 shipped fat32_rename and gated it through PRISM's `mv`, which reaches
 * only the FAT root. sfs_rename has been implemented and callable since DDR-956
 * and has never had a gate: PRISM is FAT-rooted, so a shell-driven gate cannot
 * reach SFS, and an embedded probe did not fit until DDR-960 raised the stage-2
 * read window. This is that probe.
 *
 * Three arms, each chosen so a STUB `sfs_rename` returning 0 fails it (R8):
 *
 *   arm 1  PRADYOS_SFS_RENAME_OK      rename moves the DATA: the destination
 *                                     reads back the source's exact bytes and
 *                                     the source stops opening.
 *   arm 2  PRADYOS_SFS_RENAME_ENOENT  renaming an absent path FAILS.
 *   arm 3  PRADYOS_SFS_RENAME_LFN     a long source name is retired: after the
 *                                     rename the old name no longer resolves
 *                                     and the new one holds the payload.
 *
 * Why arm 1 reads the payload back rather than just opening the destination: a
 * rename that created an empty entry and dropped the inode would pass an
 * open-only check. The bytes are what prove the SAME inode was re-keyed, which
 * is exactly what sfs_rename claims to do (two bt_inserts, one commit, inode
 * reused not copied).
 *
 * Arm 3 mirrors smoke-rename arm 6 in intent, not mechanism. SFS has no VFAT
 * fragment chain to corrupt -- it stores the whole name in the B+tree leaf slot
 * -- so what a long name exercises here is the name_len field and the name copy
 * in bt_insert, and the FNV1a32 keying of a name that does not fit the 8.3 shape
 * a FAT-shaped implementation would assume. Unlike FAT, the probe can create its
 * own long-named file, so this arm needs no image fixture.
 *
 * Freestanding (no libc): raw syscalls, NO writable globals (user.ld / DDR-826).
 */

#define SYS_EXIT    4
#define SYS_READ    5
#define SYS_WRITE   6
#define SYS_OPEN    7
#define SYS_CLOSE   8
#define SYS_RENAME  95

#define O_RDONLY    0x0
#define O_WRONLY    0x1
#define O_CREAT     0x40

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void wr(const char *s) { nsi(SYS_WRITE, 1, (long)s, slen(s)); }

__attribute__((noreturn)) static void fail(const char *why) {
    wr("SFS RENAME FAIL: ");
    wr(why);
    wr("\n");
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}

/* Create `path` holding `payload`. Returns nothing; fails the probe on error. */
static void make_file(const char *path, const char *payload) {
    long fd = nsi(SYS_OPEN, (long)path, O_CREAT | O_WRONLY, 0);
    if (fd < 0)
        fail("could not create a source file");
    long n = slen(payload);
    if (nsi(SYS_WRITE, fd, (long)payload, n) != n)
        fail("short write creating a source file");
    nsi(SYS_CLOSE, fd, 0, 0);
}

/* 1 if `path` opens and its first slen(want) bytes equal `want`; 0 if it does
 * not open. Fails the probe if it opens but the bytes differ -- that is a
 * corrupted rename, not an absent file, and the two must not be conflated. */
static int holds(const char *path, const char *want) {
    char buf[64];
    long n = slen(want);
    long fd = nsi(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (fd < 0)
        return 0;
    long got = nsi(SYS_READ, fd, (long)buf, n);
    nsi(SYS_CLOSE, fd, 0, 0);
    if (got != n)
        fail("destination opened but read back a different length");
    for (long i = 0; i < n; i++)
        if (buf[i] != want[i])
            fail("destination opened but its bytes are not the source's");
    return 1;
}

__attribute__((noreturn, force_align_arg_pointer)) void _start(void) {
    /* ---- arm 1: the data moves with the name ---------------------------- */
    const char *src = "/RENSRC.TXT";
    const char *dst = "/RENDST.TXT";
    const char *pay = "sfs-rename-payload-b4k";

    make_file(src, pay);
    if (nsi(SYS_RENAME, (long)src, (long)dst, 0) != 0)
        fail("rename of an existing file did not return 0");
    if (!holds(dst, pay))
        fail("destination does not exist after rename");
    if (holds(src, pay))
        fail("source still readable after rename");
    wr("PRADYOS_SFS_RENAME_OK\n");

    /* ---- arm 2: an absent path must FAIL -------------------------------- */
    /* The stub-catcher. A handler returning 0 unconditionally passes nothing
     * else here, but this is the arm that cannot be passed by accident. */
    if (nsi(SYS_RENAME, (long)"/NOSUCH/DEEP.TXT", (long)"/RENX.TXT", 0) == 0)
        fail("rename of an absent path returned success");
    wr("PRADYOS_SFS_RENAME_ENOENT\n");

    /* ---- arm 3: a long name is retired, not aliased --------------------- */
    /* SFS keys directory entries by (parent_ino << 32) | FNV1a32(name) and
     * stores the name in the leaf slot with an explicit length, so a long name
     * exercises the name copy and name_len that a short one does not. */
    const char *lsrc = "/AVeryLongSfsFileNameForRenameTesting.txt";
    const char *ldst = "/RENLFN.TXT";
    const char *lpay = "sfs-longname-payload-c7m";

    make_file(lsrc, lpay);
    if (!holds(lsrc, lpay))
        fail("long-named source not readable before rename");
    if (nsi(SYS_RENAME, (long)lsrc, (long)ldst, 0) != 0)
        fail("rename of a long-named file did not return 0");
    if (!holds(ldst, lpay))
        fail("long-named file not readable under its new name");
    if (holds(lsrc, lpay))
        fail("old long name still resolves after rename");
    wr("PRADYOS_SFS_RENAME_LFN\n");

    nsi(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}
