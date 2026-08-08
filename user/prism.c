/* user/prism.c — PRISM, the first interactive PRADYOS shell (Layer 5 slice 5e).
 *
 * A ring-3 C program launched by PID 1 init. It reads command lines from the
 * serial console (raw SYS_READ on fd 0 — musl stdin would need SYS_READV), and
 * writes prompts/output with musl printf (fflush'd: the console is non-tty so
 * stdout is fully buffered). Commands are one line, space-separated; no pipes,
 * redirection, quoting, or scripting yet (ADR-024 §D3). Builtins dispatch in
 * process; `run` fork+execve+waits an external ELF. */
#include <stdio.h>
#include <string.h>

/* PRADYOS NSI numbers — keep in sync with kernel/syscall/syscall.h. */
#define SYS_GETPID 2
#define SYS_EXIT   4
#define SYS_READ   5
#define SYS_OPEN   7
#define SYS_CLOSE  8
#define SYS_EXECVE 14
#define SYS_FORK   15
#define SYS_WAIT4  16
#define SYS_PIPE   17     /* DDR-780: PROC-A, (int fds[2]) -> 0 — pipes */
#define SYS_DUP2   18     /* DDR-778: PROC-A, already in the kernel — output redirection */
#define REDIR_SAVE_FD 9   /* DDR-778: parking slot for the real stdout (FD_MAX=64) */
#define REDIR_SAVE_IN 10  /* DDR-781: parking slot for the real stdin */
#define REDIR_SAVE_ERR 11 /* DDR-784: parking slot for the real stderr */
#define SYS_KILL   23     /* DDR-755: (pid, signum) -> 0 | -errno */
#define SIGTERM    15
#define SYS_GETDENTS 66     /* DDR-742: (path, index, name_buf) -> namelen | 0 | -errno */
#define SYS_GETPROCS 67     /* DDR-743: (index, struct procinfo*) -> 1 | 0(end) | -errno */
#define SYS_UNLINK   68     /* DDR-744: (path) -> 0 | -errno */
#define O_WRONLY     0x1    /* DDR-745: touch open mode */
#define O_CREAT      0x40
#define O_TRUNC      0x200  /* DDR-782: kernel sys_file.h values (Linux octal 01000) */
#define O_APPEND     0x400  /* DDR-782: atomic append, honoured by the FD_VFS write path */
#define SYS_SYSINFO  71     /* DDR-748: (struct sysinfo*) -> 0 | -EFAULT */
#define SYS_TIME     72     /* DDR-749: (struct rtc_time*) -> 0 | -EFAULT */
#define SYS_DMESG    73     /* DDR-750: (buf, max) -> bytes | -EFAULT */
#define SYS_MEMINFO  74     /* DDR-752: (struct meminfo*) -> 0 | -EFAULT */
#define SYS_SETNAME  75     /* DDR-756: (const char *name) -> 0 | -EFAULT */

/* Mirror of kernel struct meminfo (sys_proc.c) — DDR-752. */
struct meminfo {
    unsigned long long total_pages, free_pages, used_pages;
    unsigned page_size, _pad;
};

/* Mirror of kernel struct sysinfo (sys_proc.c) — DDR-748. */
struct sysinfo {
    char vendor[16];
    char brand[64];
    unsigned cpu_count, feat_edx, feat_ecx, _pad;
    unsigned long long uptime_ticks, free_pages;
};
/* Mirror of kernel struct rtc_time (rtc.h) — DDR-749. */
struct rtc_time {
    unsigned short year;
    unsigned char  month, day, hour, minute, second;
};

/* Mirrors kernel struct procinfo (sched.h) — pure-value process snapshot. */
struct procinfo {
    unsigned int pid;
    unsigned int ppid;
    unsigned int state;     /* 0 ready 1 running 2 done 3 blocked 4 zombie */
    unsigned int flags;     /* bit 0 = user */
    char name[16];
    unsigned long long run_ticks;   /* DDR-754: 100 Hz ticks */
    unsigned long long dispatches;  /* DDR-754: switch-in count */
};
#define SYS_GET_MODE 29   /* L7: sovereign/manual toggle binding (DDR-701) */
#define SYS_SET_MODE 30

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

/* Read one line from the console into buf (NUL-terminated); returns length, or
 * -1 on EOF/error. CR is ignored; LF ends the line. */
static int readline(char *buf, int max) {
    int n = 0;
    for (;;) {
        char c;
        long r = nsi(SYS_READ, 0, (long)&c, 1);
        if (r <= 0)
            return -1;
        if (c == '\r')
            continue;
        if (c == '\n') {
            buf[n] = 0;
            return n;
        }
        if (n < max - 1)
            buf[n++] = c;
    }
}

/* DDR-789: exit status of the last reaped child, surfaced to the user as `$?`.
 * do_run and the DDR-786 pipeline loop already collected this and discarded it. */
static long last_status = 0;

/* DDR-789: expand tokens that are EXACTLY "$?" to the decimal last status. This
 * is deliberately not general variable expansion — no $VAR, no interpolation
 * inside a larger token, no quoting. The rotating buffers let a line mention $?
 * more than once; a token points at one until the next command overwrites it,
 * which is safe because expansion happens once per line, before dispatch. */
static void fmt_long(char *out, unsigned cap, long v) {
    /* snprintf is NOT in the musl subset (the same gap DDR-784 hit with stderr),
     * and one integer does not justify growing it. Bounded by cap (S2). */
    char tmp[24];
    unsigned n = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-(v + 1)) + 1UL : (unsigned long)v;
    do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u && n < sizeof tmp);
    unsigned o = 0;
    if (neg && o + 1 < cap) out[o++] = '-';
    while (n > 0 && o + 1 < cap) out[o++] = tmp[--n];
    out[o] = 0;
}

static void expand_status(char **argv, int argc) {
    static char sbuf[4][64];
    static int  slot = 0;
    for (int i = 0; i < argc; i++) {
        unsigned n = (unsigned)strlen(argv[i]);
        /* A token ENDING in "$?" — so both `$?` alone and the usual idiom
         * `status=$?` work. Still not general expansion: no $VAR, and nothing is
         * substituted mid-token. Bounded copy into a fixed buffer (S2). */
        if (n < 2 || argv[i][n - 2] != '$' || argv[i][n - 1] != '?')
            continue;
        char *out = sbuf[slot];
        unsigned cap = (unsigned)sizeof sbuf[0], o = 0, pre = n - 2;
        for (unsigned k = 0; k < pre && o + 1 < cap; k++)
            out[o++] = argv[i][k];
        fmt_long(out + o, cap - o, last_status);
        argv[i] = out;
        slot = (slot + 1) % 4;
    }
}

/* Split s in place on runs of spaces; fills argv (up to maxv), returns argc. */
static int tokenize(char *s, char **argv, int maxv) {
    int argc = 0;
    while (*s && argc < maxv) {
        while (*s == ' ')
            *s++ = 0;
        if (!*s)
            break;
        argv[argc++] = s;
        while (*s && *s != ' ')
            s++;
    }
    return argc;
}

/* DDR-780: `cat` with no argument copies stdin -> stdout, the standard shell
 * behaviour. Without this nothing in PRISM consumes fd 0, so a pipe could not be
 * demonstrated (or used) at all. Bounded per read (S2); a closed/empty stdin just
 * yields EOF and returns. */
static void do_cat_stdin(void) {
    char b[256];
    long r;
    while ((r = nsi(SYS_READ, 0, (long)b, (long)sizeof b)) > 0)
        fwrite(b, 1, (size_t)r, stdout);
    fflush(stdout);
}

static void do_cat(const char *path) {
    long fd = nsi(SYS_OPEN, (long)path, 0 /*O_RDONLY*/, 0);
    if (fd < 0) {
        fprintf(stderr, "cat: cannot open %s\n", path);
        return;
    }
    char b[256];
    long r;
    while ((r = nsi(SYS_READ, fd, (long)b, (long)sizeof b)) > 0)
        fwrite(b, 1, (size_t)r, stdout);
    fflush(stdout);
    nsi(SYS_CLOSE, fd, 0, 0);
}

static void do_run(const char *path) {
    long kid = nsi(SYS_FORK, 0, 0, 0);
    if (kid == 0) {
        nsi(SYS_EXECVE, (long)path, 0, 0);
        nsi(SYS_EXIT, 127, 0, 0);          /* execve failed: child gives up */
    }
    if (kid < 0) {
        fprintf(stderr, "run: fork failed\n");
        return;
    }
    int st = 0;
    nsi(SYS_WAIT4, kid, (long)&st, 0);     /* block until the child finishes */
    last_status = st;                      /* DDR-789: surfaced as `$?` */
}

int main(void) {
    printf("PRISM_READY\n");
    fflush(stdout);

    char line[256];
    char *argv[16];
    for (;;) {
        printf("prism> ");
        fflush(stdout);

        int len = readline(line, sizeof line);
        if (len < 0)                       /* EOF: controlled exit, init won't respawn */
            return 0;
        int argc = tokenize(line, argv, 16);
        if (argc == 0)
            continue;
        expand_status(argv, argc);         /* DDR-789: `$?` -> last exit status */
        /* DDR-780: pipes `cmd1 | cmd2`. PRISM builtins are internal functions,
         * not execs, so the fork must wrap the builtin DISPATCH itself. Rather
         * than hoist the 120-line if/else chain into a function, each child sets
         * up its fds here and FALLS THROUGH to the existing dispatch, then exits
         * at the bottom of the loop (see `pipe_child`). Exactly one pipe and two
         * children — bounded (S2); a fault in a builtin now kills only the child,
         * not the shell (S6). */
        /* DDR-786 generalises this to N stages. DDR-780 scanned for the FIRST `|`
         * only, and its right-hand child fell through having already passed this
         * block — so a second `|` reached the builtin as a literal token (`cat`
         * would try to open a file named "|"). The shell now splits the line into
         * all its stages up front and forks each one itself, threading a pipe
         * between neighbours. That keeps the correct topology — N processes, one
         * waiter — instead of leaving an intermediate shell at each boundary
         * holding the previous read end open (which, with non-blocking 4 KiB
         * pipes, silently drops output rather than hanging). */
        int pipe_child = 0;
        {
            int cut[8];                                /* start index of each stage */
            int nstage = 1, bad = 0;
            cut[0] = 0;
            for (int i = 0; i < argc; i++) {
                if (strcmp(argv[i], "|") != 0)
                    continue;
                /* `|` first, last, or doubled is malformed — reject before any
                 * fork, so a bad line costs no processes (S2). */
                if (i == 0 || i + 1 >= argc || !strcmp(argv[i + 1], "|")) { bad = 1; break; }
                if (nstage >= (int)(sizeof cut / sizeof cut[0])) { bad = 1; break; }
                argv[i] = 0;                           /* terminate the previous stage */
                cut[nstage++] = i + 1;
            }
            if (bad) {
                fprintf(stderr, "pipe: missing command\n"); fflush(stderr); continue;
            }
            if (nstage > 1) {
                long pids[8];
                int prev_read = -1, forked = 0, am_child = 0;
                for (int s = 0; s < nstage; s++) {
                    int fds[2] = { -1, -1 };
                    if (s + 1 < nstage && nsi(SYS_PIPE, (long)fds, 0, 0) != 0) {
                        fprintf(stderr, "pipe: unavailable\n"); fflush(stderr);
                        break;                         /* reap what we already forked */
                    }
                    long pid = nsi(SYS_FORK, 0, 0, 0);
                    if (pid == 0) {                    /* this stage's process */
                        if (prev_read >= 0) {
                            nsi(SYS_DUP2, prev_read, 0, 0);
                            nsi(SYS_CLOSE, prev_read, 0, 0);
                        }
                        if (fds[1] >= 0) {
                            nsi(SYS_DUP2, fds[1], 1, 0);
                            nsi(SYS_CLOSE, fds[0], 0, 0);
                            nsi(SYS_CLOSE, fds[1], 0, 0);
                        }
                        int n = 0;                     /* shift this stage down to argv[0] */
                        for (int i = cut[s]; i < argc && argv[i]; i++) argv[n++] = argv[i];
                        argc = n;
                        pipe_child = 1;
                        am_child = 1;
                        break;                         /* fall through to the dispatch */
                    }
                    pids[forked++] = pid;
                    /* The parent must hold NO pipe end open, or the reader never
                     * sees EOF and the shell wedges (the DDR-780 lesson, applied
                     * once per stage boundary). */
                    if (prev_read >= 0) nsi(SYS_CLOSE, prev_read, 0, 0);
                    if (fds[1] >= 0)    nsi(SYS_CLOSE, fds[1], 0, 0);
                    prev_read = fds[0];
                }
                if (!am_child) {
                    if (prev_read >= 0) nsi(SYS_CLOSE, prev_read, 0, 0);
                    /* int, not long: sys_wait4 copies out sizeof(int), so a long
                     * would leave its upper bytes uninitialised (DDR-789). */
                    int st = 0;
                    for (int i = 0; i < forked; i++) {
                        nsi(SYS_WAIT4, pids[i], (long)&st, 0);
                        /* DDR-789: a pipeline reports its LAST stage's status,
                         * as a real shell does; pids[] is in stage order. */
                        if (i == forked - 1)
                            last_status = st;
                    }
                    continue;                          /* parent runs no builtin */
                }
            }
        }

        const char *cmd = argv[0];

        /* DDR-778: output redirection `cmd ... > file`. Scan from index 1 so a
         * leading "> file" can never leave argv[0] undefined; truncating argc at
         * the '>' hides the redirect tokens from the builtin. Ring-3 only —
         * SYS_DUP2 (PROC-A) and O_CREAT|O_WRONLY already exist. */
        /* DDR-781 extends this to `<` and `>>`. argc is truncated at the FIRST
         * redirect token so the builtin sees only its own arguments. */
        /* DDR-784 adds `2>`. Diagnostics now go to stderr, so redirecting fd 2 is
         * observable; `2>` truncates like `>` (DDR-782 flags). */
        /* DDR-868 (item 33) adds `2>>` and `2>&1`.
         *
         * `2>&1` takes NO filename, so it must be matched before the
         * "next token is the target" logic — treating it as `2>` followed by a
         * file named "&1" would create a junk file and leave stderr pointing at
         * it, which looks like it worked. */
        const char *redir = 0, *redir_in = 0, *redir_e = 0;
        int redir_err = 0, redir_append = 0, redir_e_append = 0;
        int redir_e_to_out = 0, cut = argc;
        for (int i = 1; i < argc; i++) {
            int is_out  = !strcmp(argv[i], ">");
            int is_app  = !strcmp(argv[i], ">>");
            int is_in   = !strcmp(argv[i], "<");
            int is_e    = !strcmp(argv[i], "2>");
            int is_eapp = !strcmp(argv[i], "2>>");
            int is_edup = !strcmp(argv[i], "2>&1");
            if (!is_out && !is_app && !is_in && !is_e && !is_eapp && !is_edup)
                continue;
            if (i < cut) cut = i;
            if (is_edup) {               /* no operand — do not consume a token */
                redir_e_to_out = 1;
                redir_e = 0;             /* last one wins, as a real shell does */
                continue;
            }
            if (i + 1 >= argc) { redir_err = 1; break; }
            if (is_in)      redir_in = argv[i + 1];
            else if (is_e || is_eapp) {
                redir_e = argv[i + 1];
                redir_e_append = is_eapp;
                redir_e_to_out = 0;      /* an explicit file overrides an earlier 2>&1 */
            }
            else          { redir = argv[i + 1]; redir_append = is_app; }
            i++;                         /* skip the filename token */
        }
        argc = cut;
        if (redir_err) {
            fprintf(stderr, "redirect: missing filename\n");
            fflush(stderr);
            continue;                    /* fds untouched — nothing to restore */
        }
        long redir_save = -1, redir_save_in = -1, redir_save_err = -1;
        if (redir_in) {                  /* DDR-781: `< file` */
            long ifd = nsi(SYS_OPEN, (long)redir_in, 0 /*O_RDONLY*/, 0);
            if (ifd < 0) {
                fprintf(stderr, "redirect: cannot open %s\n", redir_in);
                fflush(stderr);
                continue;
            }
            nsi(SYS_DUP2, 0, REDIR_SAVE_IN, 0);   /* stash the real stdin */
            nsi(SYS_DUP2, ifd, 0, 0);
            nsi(SYS_CLOSE, ifd, 0, 0);
            redir_save_in = REDIR_SAVE_IN;
        }
        if (redir_e) {                   /* DDR-784 `2> file`, DDR-868 `2>> file` */
            long eflags = O_CREAT | O_WRONLY | (redir_e_append ? O_APPEND : O_TRUNC);
            long efd = nsi(SYS_OPEN, (long)redir_e, eflags, 0);
            if (efd < 0) {
                fprintf(stderr, "redirect: cannot open %s\n", redir_e);
                fflush(stderr);
                if (redir_save_in >= 0) {         /* undo the stdin swap */
                    nsi(SYS_DUP2, redir_save_in, 0, 0);
                    nsi(SYS_CLOSE, redir_save_in, 0, 0);
                }
                continue;                /* still before the stdout swap — safe */
            }
            fflush(stderr);              /* musl leaves stderr unbuffered, but be explicit */
            nsi(SYS_DUP2, 2, REDIR_SAVE_ERR, 0);  /* stash the real stderr */
            nsi(SYS_DUP2, efd, 2, 0);
            nsi(SYS_CLOSE, efd, 0, 0);
            redir_save_err = REDIR_SAVE_ERR;
        }
        if (redir) {
            /* DDR-782: the kernel now honours O_TRUNC and O_APPEND, so `>` really
             * replaces the file and `>>` appends atomically per write(). */
            long oflags = O_CREAT | O_WRONLY | (redir_append ? O_APPEND : O_TRUNC);
            long rfd = nsi(SYS_OPEN, (long)redir, oflags, 0);
            if (rfd < 0) {
                /* stderr may already be redirected — that is where a real shell
                 * puts this message too. Unwind exactly the swaps made so far. */
                fprintf(stderr, "redirect: cannot open %s\n", redir);
                fflush(stderr);
                if (redir_save_err >= 0) {
                    nsi(SYS_DUP2, redir_save_err, 2, 0);
                    nsi(SYS_CLOSE, redir_save_err, 0, 0);
                }
                if (redir_save_in >= 0) {
                    nsi(SYS_DUP2, redir_save_in, 0, 0);
                    nsi(SYS_CLOSE, redir_save_in, 0, 0);
                }
                continue;                /* still before the stdout swap — safe */
            }
            fflush(stdout);              /* don't spill buffered console text into the file */
            nsi(SYS_DUP2, 1, REDIR_SAVE_FD, 0);   /* stash the real stdout */
            nsi(SYS_DUP2, rfd, 1, 0);
            nsi(SYS_CLOSE, rfd, 0, 0);
            redir_save = REDIR_SAVE_FD;
        }

        /* DDR-868 (item 33): `2>&1` — point stderr at WHATEVER fd 1 IS NOW.
         *
         * ORDER IS THE WHOLE POINT, and it is why this sits after the `>` block
         * rather than beside the `2>` one. `cmd > f 2>&1` must send both streams
         * to f, which only works if fd 1 already refers to f when fd 2 is
         * duplicated from it. Doing it earlier would capture the console, and
         * `cmd > f 2>&1` would silently leave errors on the terminal — the exact
         * bug this syntax exists to avoid, and one that looks like it worked.
         *
         * Note this is a SNAPSHOT, not an alias: a later change to fd 1 does not
         * follow. That matches the shell semantics being implemented. */
        if (redir_e_to_out && redir_save_err < 0) {
            fflush(stderr);
            nsi(SYS_DUP2, 2, REDIR_SAVE_ERR, 0);  /* stash the real stderr */
            nsi(SYS_DUP2, 1, 2, 0);               /* fd 2 := current fd 1      */
            redir_save_err = REDIR_SAVE_ERR;
        }

        if (!strcmp(cmd, "help")) {
            printf("builtins: help echo cat run ls ps kill setname touch rm uname date uptime dmesg free mode exit\n");
        } else if (!strcmp(cmd, "mode")) {
            /* L7 (DDR-701): the Sovereign/Manual toggle binding. `mode [get]`
             * reads SYS_GET_MODE; `mode set sovereign|manual` attempts
             * SYS_SET_MODE — denied here (no CAP_SOVEREIGN), proving the gate. */
            if (argc >= 3 && !strcmp(argv[1], "set")) {
                int want = !strcmp(argv[2], "sovereign") ? 1 : 0;
                long r = nsi(SYS_SET_MODE, want, 0, 0);
                if (r == 0)
                    printf("mode: set %s\n", want ? "SOVEREIGN" : "MANUAL");
                else
                    fprintf(stderr, "mode: denied (rc=%ld) — toggling requires CAP_SOVEREIGN\n", r);
            } else {
                long m = nsi(SYS_GET_MODE, 0, 0, 0);
                printf("MODE: %s\n", m ? "SOVEREIGN" : "MANUAL");
            }
        } else if (!strcmp(cmd, "echo")) {
            for (int i = 1; i < argc; i++)
                printf("%s%s", argv[i], i + 1 < argc ? " " : "");
            printf("\n");
        } else if (!strcmp(cmd, "cat")) {
            if (argc < 2) do_cat_stdin();        /* DDR-780: `... | cat` */
            else do_cat(argv[1]);
        } else if (!strcmp(cmd, "run")) {
            if (argc < 2) fprintf(stderr, "run: usage: run <path>\n");
            else do_run(argv[1]);
        } else if (!strcmp(cmd, "ls")) {
            const char *dir = (argc > 1) ? argv[1] : "/";   /* DDR-742 */
            char nm[256];
            int any = 0;
            for (long i = 0; ; i++) {
                long len = nsi(SYS_GETDENTS, (long)dir, i, (long)nm);
                if (len <= 0) break;
                printf("%s\n", nm);
                any = 1;
            }
            if (!any) fprintf(stderr, "ls: %s: empty or not a directory\n", dir);
        } else if (!strcmp(cmd, "ps")) {
            /* DDR-743: enumerate the scheduler ring via SYS_GETPROCS. */
            static const char st[] = "RrDBZ";     /* ready run done blkd zomb */
            printf("  PID  PPID S U    CPUms   DISP NAME\n");
            struct procinfo pi;
            for (long i = 0; ; i++) {
                if (nsi(SYS_GETPROCS, i, (long)&pi, 0) <= 0) break;
                char s = (pi.state < sizeof st - 1) ? st[pi.state] : '?';
                printf("%5u %5u %c %c %8llu %6llu %s\n",
                       pi.pid, pi.ppid, s, (pi.flags & 1) ? 'u' : 'k',
                       pi.run_ticks * 10ULL, pi.dispatches, pi.name);
            }
        } else if (!strcmp(cmd, "touch")) {
            /* DDR-745: create-if-absent an empty file on the (FAT) root. */
            if (argc < 2) printf("touch: usage: touch <path>\n");
            else {
                long fd = nsi(SYS_OPEN, (long)argv[1], O_CREAT | O_WRONLY, 0);
                if (fd < 0) printf("touch: cannot create %s\n", argv[1]);
                else { nsi(SYS_CLOSE, fd, 0, 0); printf("touch: %s\n", argv[1]); }
            }
        } else if (!strcmp(cmd, "rm")) {
            /* DDR-745: remove a file (or empty dir) from the root. */
            if (argc < 2) printf("rm: usage: rm <path>\n");
            else if (nsi(SYS_UNLINK, (long)argv[1], 0, 0) == 0)
                printf("rm: removed %s\n", argv[1]);
            else
                printf("rm: cannot remove %s\n", argv[1]);
        } else if (!strcmp(cmd, "uname")) {                  /* DDR-751 */
            struct sysinfo si;
            if (nsi(SYS_SYSINFO, (long)&si, 0, 0) == 0)
                printf("uname: %s \"%s\" cpus=%u\n",
                       si.vendor, si.brand[0] ? si.brand : "(none)", si.cpu_count);
            else
                printf("uname: unavailable\n");
        } else if (!strcmp(cmd, "date")) {                   /* DDR-751 */
            struct rtc_time t;
            if (nsi(SYS_TIME, (long)&t, 0, 0) == 0)
                printf("date: %04u-%02u-%02u %02u:%02u:%02u\n",
                       t.year, t.month, t.day, t.hour, t.minute, t.second);
            else
                printf("date: unavailable\n");
        } else if (!strcmp(cmd, "uptime")) {                 /* DDR-751 */
            struct sysinfo si;
            if (nsi(SYS_SYSINFO, (long)&si, 0, 0) == 0)
                printf("uptime: %llus\n", si.uptime_ticks / 100ULL);  /* 100 Hz tick */
            else
                printf("uptime: unavailable\n");
        } else if (!strcmp(cmd, "dmesg")) {                  /* DDR-751 */
            char b[4096];
            long n = nsi(SYS_DMESG, (long)b, (long)sizeof b, 0);
            printf("dmesg: %ld bytes\n", n > 0 ? n : 0);
            if (n > 0) fwrite(b, 1, (size_t)n, stdout);
        } else if (!strcmp(cmd, "kill")) {                   /* DDR-755 */
            if (argc < 2) { printf("kill: usage: kill <pid> [signum]\n"); }
            else {
                long pid = 0; const char *s = argv[1];
                while (*s >= '0' && *s <= '9') pid = pid * 10 + (*s++ - '0');
                int sig = SIGTERM;
                if (argc >= 3) {
                    sig = 0; const char *t = argv[2];
                    while (*t >= '0' && *t <= '9') sig = sig * 10 + (*t++ - '0');
                }
                long r = nsi(SYS_KILL, pid, sig, 0);
                if (r == 0) printf("kill: sent %d to %ld\n", sig, pid);
                else        printf("kill: pid %ld not found (rc=%ld)\n", pid, r);
            }
        } else if (!strcmp(cmd, "setname")) {                /* DDR-756 */
            if (argc < 2) printf("setname: usage: setname <name>\n");
            else if (nsi(SYS_SETNAME, (long)argv[1], 0, 0) == 0)
                printf("setname: now %s\n", argv[1]);
            else
                printf("setname: failed\n");
        } else if (!strcmp(cmd, "free")) {                   /* DDR-752 */
            struct meminfo mi;
            if (nsi(SYS_MEMINFO, (long)&mi, 0, 0) == 0)
                printf("mem: total=%lluK free=%lluK used=%lluK\n",
                       mi.total_pages * 4ULL, mi.free_pages * 4ULL, mi.used_pages * 4ULL);
            else
                printf("free: unavailable\n");
        } else if (!strcmp(cmd, "exit")) {
            return 0;
        } else {
            fprintf(stderr, "prism: unknown command: %s\n", cmd);
        }
        fflush(stdout);                  /* DDR-778: MUST flush INTO the file before
                                          * restoring fd 1 — musl fully buffers a
                                          * non-tty stdout, so a late flush would
                                          * land on the console instead. */
        if (redir_save >= 0) {
            nsi(SYS_DUP2, redir_save, 1, 0);
            nsi(SYS_CLOSE, redir_save, 0, 0);
        }
        if (redir_save_in >= 0) {        /* DDR-781: restore the real stdin */
            nsi(SYS_DUP2, redir_save_in, 0, 0);
            nsi(SYS_CLOSE, redir_save_in, 0, 0);
        }
        if (redir_save_err >= 0) {       /* DDR-784: restore the real stderr */
            fflush(stderr);              /* flush INTO the file, as for fd 1 above */
            nsi(SYS_DUP2, redir_save_err, 2, 0);
            nsi(SYS_CLOSE, redir_save_err, 0, 0);
        }
        if (pipe_child)                  /* DDR-780: a pipe half never loops —
                                          * output already flushed above. */
            nsi(SYS_EXIT, 0, 0, 0);
    }
}
