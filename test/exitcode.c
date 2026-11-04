#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* exitcode 42 | exitcode term: exit status or fatal signal must reach the caller unchanged. */
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "term")) { kill(getpid(), SIGTERM); pause(); }
    return 42;
}
