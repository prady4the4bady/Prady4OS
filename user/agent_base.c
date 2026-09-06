/* user/agent_base.c — AETHER agent template, ring 3 (Layer 6, ADR-026 / ADR-027).
 *
 * An agent is an ordinary musl process holding only CAP_AGENT. It turns a model
 * "response" into an action, PROPOSES it to the kernel (SYS_SUBMIT_ACTION), waits
 * for the policy verdict (SYS_POLL_RESULT), and only then executes — it never
 * holds the authority to act, the kernel does.
 *
 * Test mode (AETHER_TEST_MODE=1, the CI path): the response is a fixed string, so
 * the full submit -> approve -> execute pipeline runs with no external dependency.
 * Live mode (AETHER_TEST_MODE=0): the response is fetched from an Ollama endpoint
 * over the ring-3 proxy-socket NSI (ADR-027) — HTTP/1.1 POST /api/generate, with a
 * hand-written JSON "response" extractor — and the agent prints PRADYOS_AGENT_LIVE_OK.
 */
#include <stdio.h>
#include <string.h>
#include "uline.h"                 /* DDR-1056: one write(2) per measured line */

#ifndef AETHER_TEST_MODE
#define AETHER_TEST_MODE 1
#endif

/* NSI numbers — keep in sync with kernel/syscall/syscall.h. */
#define SYS_EXIT          4
#define SYS_YIELD         3
#define SYS_READ          5
#define SYS_WRITE         6
#define SYS_OPEN          7
#define SYS_CLOSE         8
#define SYS_SUBMIT_ACTION 31
#define SYS_POLL_RESULT   32
#define SYS_SOCK_CONNECT  39
#define SYS_SOCK_WRITE    40
#define SYS_SOCK_READ     41
#define SYS_SOCK_CLOSE    42

/* Must match kernel/aether/aether.h. */
#define ACTION_WRITE_FILE 1
#define AE_PENDING        1
#define AE_APPROVED       2

/* Open flags — same three fsrmtest.c uses, and the same values sys_open reads. */
#define O_RDONLY          0x0
#define O_WRONLY          0x1
#define O_CREAT           0x40

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

#if !AETHER_TEST_MODE
/* ---- live mode: fetch a completion from Ollama via the proxy-socket NSI ----- */
#ifndef OLLAMA_HOST_BE
#define OLLAMA_HOST_BE 0x0A000202u   /* 10.0.2.2 = the QEMU SLIRP gateway (host) */
#endif
#ifndef OLLAMA_PORT
#define OLLAMA_PORT 11434
#endif
#ifndef OLLAMA_MODEL
#define OLLAMA_MODEL "llama3.2:1b"
#endif

/* 5-arg form for SYS_SOCK_READ (timeout in r10, the SysV syscall 4th arg). */
static inline long nsi4(long n, long a1, long a2, long a3, long a4) {
    long r;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}

/* Hand-rolled string builders (the musl subset omits strstr/snprintf). */
static int put_str(char *d, const char *s) { int n = 0; while (s[n]) { d[n] = s[n]; n++; } return n; }
static int put_uint(char *d, unsigned v) {
    char t[12]; int i = 0;
    if (!v) { d[0] = '0'; return 1; }
    while (v) { t[i++] = (char)('0' + v % 10); v /= 10; }
    for (int j = 0; j < i; j++) d[j] = t[i - 1 - j];
    return i;
}
static const char *find_sub(const char *hay, int haylen, const char *needle) {
    int nl = (int)strlen(needle);
    for (int i = 0; i + nl <= haylen; i++) {
        int j = 0;
        while (j < nl && hay[i + j] == needle[j]) j++;
        if (j == nl) return hay + i;
    }
    return 0;
}

/* POST a tiny prompt and copy the JSON "response":"..." value into out.
 * Returns the response length, or -1 on failure. */
static int ollama_generate(char *out, int outsz) {
    int fd = (int)nsi(SYS_SOCK_CONNECT, (long)OLLAMA_HOST_BE, OLLAMA_PORT, 0);
    if (fd < 0) return -1;

    /* Let the TCP handshake complete (a read with timeout parks on PIT/RX IRQs). */
    char scratch[8];
    nsi4(SYS_SOCK_READ, fd, (long)scratch, 1, 600);

    char body[160]; int blen = 0;
    blen += put_str(body + blen, "{\"model\":\"");
    blen += put_str(body + blen, OLLAMA_MODEL);
    blen += put_str(body + blen, "\",\"prompt\":\"Reply with exactly: PRADYOS\",\"stream\":false}");
    body[blen] = 0;

    char req[400]; int rlen = 0;
    rlen += put_str(req + rlen, "POST /api/generate HTTP/1.1\r\nHost: 10.0.2.2\r\n"
                                "Content-Type: application/json\r\nContent-Length: ");
    rlen += put_uint(req + rlen, (unsigned)blen);
    rlen += put_str(req + rlen, "\r\nConnection: close\r\n\r\n");
    memcpy(req + rlen, body, (size_t)blen); rlen += blen;

    if (nsi(SYS_SOCK_WRITE, fd, (long)req, rlen) <= 0) { nsi(SYS_SOCK_CLOSE, fd, 0, 0); return -1; }

    /* Read the whole response (Connection: close -> EOF) into a buffer. */
    static char resp[4096];
    int total = 0;
    while (total < (int)sizeof resp - 1) {
        long n = nsi4(SYS_SOCK_READ, fd, (long)(resp + total), (int)sizeof resp - 1 - total, 4000);
        if (n <= 0) break;                 /* EOF or timeout */
        total += (int)n;
    }
    resp[total] = 0;
    nsi(SYS_SOCK_CLOSE, fd, 0, 0);
    if (total <= 0) return -1;

    /* Extract the "response":"..." value (no escape handling beyond a stop at "). */
    const char *p = find_sub(resp, total, "\"response\":\"");
    if (!p) return -1;
    p += strlen("\"response\":\"");
    int n = 0;
    while (*p && *p != '"' && n < outsz - 1) {
        if (*p == '\\' && p[1]) p++;       /* unescape: copy the escaped char */
        out[n++] = *p++;
    }
    out[n] = 0;
    return n;
}
#endif /* !AETHER_TEST_MODE */

#if AETHER_TEST_MODE
/* A failed round trip must NOT look like a successful one: this prints an
 * explicit error naming the step and the errno, does NOT print `data`, and
 * exits non-zero — so PRADYOS_AGENT_VERIFIED is absent and smoke-aether reddens
 * rather than passing on a narration. */
static void agent_exec_fail(const char *step, long rc) {
    uline u; ul_init(&u);
    ul_s(&u, "AETHER_AGENT_EXEC_FAIL step="); ul_s(&u, step);
    ul_s(&u, " rc=");                         ul_d(&u, rc);
    ul_s(&u, "\n");
    printf("%s", ul_end(&u));
    printf("PRADYOS_AGENT_DONE\n");
    fflush(stdout);
    nsi(SYS_EXIT, 1, 0, 0);
    for (;;) { }
}
#endif

int main(int argc, char **argv) {
    const char *task = (argc > 2) ? argv[2] : "test";
    printf("PRADYOS_AGENT_START task=%s mode=%s\n",
           task, AETHER_TEST_MODE ? "test" : "live");
    fflush(stdout);

#if !AETHER_TEST_MODE
    /* Live: prove end-to-end model inference over the proxy-socket NSI. */
    char text[512];
    int n = ollama_generate(text, sizeof text);
    if (n > 0) {
        printf("PRADYOS_AGENT_LIVE_OK response=%s\n", text);
    } else {
        printf("PRADYOS_AGENT_LIVE_FAIL (no Ollama at 10.0.2.2:%d?)\n", OLLAMA_PORT);
    }
    printf("PRADYOS_AGENT_DONE\n");
    fflush(stdout);
    nsi(SYS_EXIT, (n > 0) ? 0 : 1, 0, 0);
    return 0;
#else
    /* Test: a fixed model response -> ACTION_WRITE_FILE, submitted and executed.
     *
     * DDR-1066: this branch used to PRINT the path and the data on approval and
     * perform no filesystem call at all — the file contained zero SYS_OPEN and
     * zero SYS_WRITE — while this comment, the gate's comment and the log line
     * `AETHER_AGENT_EXEC WRITE_FILE` all read as an execution. `data` is the
     * string `smoke-aether` requires, so the gate's execute arm asserted on a
     * .rodata literal and could not fail for the reason it exists.
     *
     * The path moved to the FAT32 root because there is no /tmp: the volume's
     * only directory is ::/DOCS (Makefile `mmd`), so the old path could not have
     * been written even by a correct implementation. */
    const char *path = "/AETHER.TXT";
    const char *data = "PRADYOS_AGENT_VERIFIED";

    char payload[256];
    size_t pathlen = strlen(path);
    memcpy(payload, path, pathlen);
    payload[pathlen] = '\0';
    size_t dlen = strlen(data);
    memcpy(payload + pathlen + 1, data, dlen);
    size_t plen = pathlen + 1 + dlen + 1;

    long id = nsi(SYS_SUBMIT_ACTION, ACTION_WRITE_FILE, (long)payload, (long)plen);
    if (id < 0) {
        printf("PRADYOS_AGENT_DONE submit_err=%ld\n", id);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }

    long st = AE_PENDING;
    for (int i = 0; i < 50; i++) {
        st = nsi(SYS_POLL_RESULT, id, 0, 0);
        if (st != AE_PENDING) break;
        nsi(SYS_YIELD, 0, 0, 0);
    }

    if (st != AE_APPROVED) {
        printf("AETHER_AGENT_SKIP verdict=%ld\n", st);
        printf("PRADYOS_AGENT_DONE\n");
        fflush(stdout);
        nsi(SYS_EXIT, 0, 0, 0);
        return 1;                 /* SYS_EXIT does not return; this makes it
                                   * impossible to fall into the write below */
    }

    /* EXECUTE, and only now — the shape user/actionreadtest.c:101 already used
     * and this agent did not. The kernel approved; the agent acts (DDR-1013 §2:
     * there is no kernel executor for ACTION_WRITE_FILE, and there should not
     * be). */
    size_t dwrite = strlen(data);
    long wfd = nsi(SYS_OPEN, (long)path, O_CREAT | O_WRONLY, 0);
    if (wfd < 0)
        agent_exec_fail("open_w", wfd);
    long wn = nsi(SYS_WRITE, wfd, (long)data, (long)dwrite);
    nsi(SYS_CLOSE, wfd, 0, 0);
    if (wn != (long)dwrite)
        agent_exec_fail("write", wn);

    /* READ BACK through a SECOND open, deliberately WITHOUT O_CREAT: vfs_open on
     * a missing file returns -ENOENT, so a build that skipped the write above
     * cannot reach the print below. That is what makes the arm live. */
    char back[64];
    for (size_t i = 0; i < sizeof back; i++) back[i] = 0;
    long rfd = nsi(SYS_OPEN, (long)path, O_RDONLY, 0);
    if (rfd < 0)
        agent_exec_fail("open_r", rfd);
    long rn = nsi(SYS_READ, rfd, (long)back, (long)(sizeof back - 1));
    nsi(SYS_CLOSE, rfd, 0, 0);
    if (rn <= 0)
        agent_exec_fail("read", rn);
    back[rn] = '\0';

    /* The quantities, so presence of the marker alone is not the whole claim:
     * a write that stores the wrong length changes n= and is caught (DDR-1039 —
     * assert both directions). */
    { uline u; ul_init(&u);
      ul_s(&u, "PRADYOS_AGENT_EXEC_OK path="); ul_s(&u, path);
      ul_s(&u, " n=");     ul_d(&u, rn);
      ul_s(&u, " first="); ul_c(&u, back[0]);
      ul_s(&u, " last=");  ul_c(&u, back[rn - 1]);
      ul_s(&u, "\n");
      printf("%s", ul_end(&u)); }

    /* PRADYOS_AGENT_VERIFIED comes out of the READ-BACK BUFFER, never out of
     * `data`. smoke-aether has always required that sentinel; emitting it from
     * bytes that made a filesystem round trip is what turns the assertion the
     * gate already makes into the one its comment always described. */
    printf("AETHER_AGENT_EXEC WRITE_FILE %s\n%s\n", path, back);
    printf("PRADYOS_AGENT_DONE\n");
    fflush(stdout);
    nsi(SYS_EXIT, 0, 0, 0);
    return 0;
#endif
}
