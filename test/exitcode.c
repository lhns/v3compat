#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* exitcode [N | term | kill | segv | abrt]: end with that status or signal (default 42); it must reach the caller unchanged. */
int main(int argc, char **argv) {
    const char *a = argc > 1 ? argv[1] : "42";
    if (!strcmp(a, "term")) { kill(getpid(), SIGTERM); pause(); }
    if (!strcmp(a, "kill")) { kill(getpid(), SIGKILL); pause(); }
    if (!strcmp(a, "abrt")) abort();
    if (!strcmp(a, "segv")) { signal(SIGSEGV, SIG_DFL); *(volatile int *)0 = 1; }
    return atoi(a);
}
