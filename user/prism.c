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
#define SYS_GETDENTS 66     /* DDR-742: (path, index, name_buf) -> namelen | 0 | -errno */
#define SYS_GETPROCS 67     /* DDR-743: (index, struct procinfo*) -> 1 | 0(end) | -errno */

/* Mirrors kernel struct procinfo (sched.h) — pure-value process snapshot. */
struct procinfo {
    unsigned int pid;
    unsigned int ppid;
    unsigned int state;     /* 0 ready 1 running 2 done 3 blocked 4 zombie */
    unsigned int flags;     /* bit 0 = user */
    char name[16];
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

static void do_cat(const char *path) {
    long fd = nsi(SYS_OPEN, (long)path, 0 /*O_RDONLY*/, 0);
    if (fd < 0) {
        printf("cat: cannot open %s\n", path);
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
        printf("run: fork failed\n");
        return;
    }
    int st = 0;
    nsi(SYS_WAIT4, kid, (long)&st, 0);     /* block until the child finishes */
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
        const char *cmd = argv[0];

        if (!strcmp(cmd, "help")) {
            printf("builtins: help echo cat run ls ps mode exit\n");
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
                    printf("mode: denied (rc=%ld) — toggling requires CAP_SOVEREIGN\n", r);
            } else {
                long m = nsi(SYS_GET_MODE, 0, 0, 0);
                printf("MODE: %s\n", m ? "SOVEREIGN" : "MANUAL");
            }
        } else if (!strcmp(cmd, "echo")) {
            for (int i = 1; i < argc; i++)
                printf("%s%s", argv[i], i + 1 < argc ? " " : "");
            printf("\n");
        } else if (!strcmp(cmd, "cat")) {
            if (argc < 2) printf("cat: usage: cat <path>\n");
            else do_cat(argv[1]);
        } else if (!strcmp(cmd, "run")) {
            if (argc < 2) printf("run: usage: run <path>\n");
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
            if (!any) printf("ls: %s: empty or not a directory\n", dir);
        } else if (!strcmp(cmd, "ps")) {
            /* DDR-743: enumerate the scheduler ring via SYS_GETPROCS. */
            static const char st[] = "RrDBZ";     /* ready run done blkd zomb */
            printf("  PID  PPID S U NAME\n");
            struct procinfo pi;
            for (long i = 0; ; i++) {
                if (nsi(SYS_GETPROCS, i, (long)&pi, 0) <= 0) break;
                char s = (pi.state < sizeof st - 1) ? st[pi.state] : '?';
                printf("%5u %5u %c %c %s\n",
                       pi.pid, pi.ppid, s, (pi.flags & 1) ? 'u' : 'k', pi.name);
            }
        } else if (!strcmp(cmd, "exit")) {
            return 0;
        } else {
            printf("prism: unknown command: %s\n", cmd);
        }
        fflush(stdout);
    }
}
