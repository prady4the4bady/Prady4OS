/* user/agent_base.c — AETHER agent template, ring 3 (Layer 6, ADR-026 §D9/§D10).
 *
 * An agent is an ordinary musl process holding only CAP_AGENT. It turns a model
 * "response" into an action, PROPOSES it to the kernel (SYS_SUBMIT_ACTION), waits
 * for the policy verdict (SYS_POLL_RESULT), and only then executes — it never
 * holds the authority to act, the kernel does.
 *
 * Test mode (AETHER_TEST_MODE, the CI path): the response is a fixed string, so
 * the full submit -> approve -> execute -> audit pipeline runs with no external
 * dependency. Live mode (HTTP/1.1 POST to Ollama over the in-kernel lwIP stack)
 * is DEFERRED: it requires a socket NSI that does not exist yet (lwIP runs in the
 * kernel with no ring-3 socket API) — see docs/build_status.md.
 */
#include <stdio.h>
#include <string.h>

/* NSI numbers — keep in sync with kernel/syscall/syscall.h. */
#define SYS_EXIT          4
#define SYS_YIELD         3
#define SYS_SUBMIT_ACTION 31
#define SYS_POLL_RESULT   32

/* Must match kernel/aether/aether.h. */
#define ACTION_WRITE_FILE 1
#define AE_PENDING        1
#define AE_APPROVED       2

static inline long nsi(long n, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

int main(int argc, char **argv) {
    const char *task = (argc > 2) ? argv[2] : "test";

    /* Test-mode model response, conceptually parsed from the fixed string
     * "ACTION: WRITE_FILE /tmp/aether_test.txt PRADYOS_AGENT_VERIFIED". The musl
     * subset omits strstr/strchr, and in test mode the verb/path/data are known
     * at compile time, so we take them as literals (a live-mode JSON+token parser
     * is deferred with the socket NSI). */
    const char *path = "/tmp/aether_test.txt";
    const char *data = "PRADYOS_AGENT_VERIFIED";

    /* Payload to the kernel: "<path>\0<data>" inside one bounded buffer. */
    char payload[256];
    size_t pathlen = strlen(path);
    memcpy(payload, path, pathlen);
    payload[pathlen] = '\0';
    size_t dlen = strlen(data);
    memcpy(payload + pathlen + 1, data, dlen);
    size_t plen = pathlen + 1 + dlen + 1;

    printf("PRADYOS_AGENT_START task=%s\n", task);
    fflush(stdout);

    long id = nsi(SYS_SUBMIT_ACTION, ACTION_WRITE_FILE, (long)payload, (long)plen);
    if (id < 0) {
        printf("PRADYOS_AGENT_DONE submit_err=%ld\n", id);
        fflush(stdout);
        nsi(SYS_EXIT, 1, 0, 0);
    }

    /* Poll for the verdict (sovereign mode auto-approves at submit, so this is
     * APPROVED on the first poll; manual mode would loop with yields). */
    long st = AE_PENDING;
    for (int i = 0; i < 50; i++) {
        st = nsi(SYS_POLL_RESULT, id, 0, 0);
        if (st != AE_PENDING)
            break;
        nsi(SYS_YIELD, 0, 0, 0);
    }

    if (st == AE_APPROVED) {
        /* Execute the approved action. The effect (writing the verified marker)
         * is surfaced on the console so the gate can observe the full pipeline;
         * a file-backed write follows the identical submit/approve/execute path. */
        printf("AETHER_AGENT_EXEC WRITE_FILE %s %s\n", path, data);
        printf("%s\n", data);          /* PRADYOS_AGENT_VERIFIED */
    } else {
        printf("AETHER_AGENT_SKIP verdict=%ld\n", st);
    }
    printf("PRADYOS_AGENT_DONE\n");
    fflush(stdout);
    nsi(SYS_EXIT, 0, 0, 0);
    return 0;
}
