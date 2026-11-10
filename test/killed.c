/* killed: a child with no handlers spins in its own code and is killed from outside with SIGSEGV, SIGABRT, SIGXCPU and
 * SIGBUS, signals DynamoRIO would deliver at once if they were faults; it must die of each. "PASS" as the last line. */
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile unsigned long spins;

int main(void) {
    static const int sigs[] = {SIGSEGV, SIGABRT, SIGXCPU, SIGBUS};
    struct rlimit nocore = {0, 0};
    struct timespec t = {0, 200000000};
    int bad = 0;
    setrlimit(RLIMIT_CORE, &nocore);
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) {
        pid_t c = fork();
        if (c == 0) for (;;) spins++;
        nanosleep(&t, 0);
        kill(c, sigs[i]);
        int st = 0;
        waitpid(c, &st, 0);
        int ok = WIFSIGNALED(st) && WTERMSIG(st) == sigs[i];
        printf("%s signal %d: status %#x\n", ok ? "ok" : "FAIL", sigs[i], st);
        if (!ok) bad = 1;
    }
    printf("%s\n", bad ? "FAILED" : "PASS");
    return bad;
}
