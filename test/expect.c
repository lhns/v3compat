/* expect exit N|signal N [stdout-has STR] -- cmd...: run cmd and check how it ended. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
int main(int argc, char **argv) {
    int i = 1, want_sig = 0, want = 0;
    const char *has = NULL;
    for (; i < argc && strcmp(argv[i], "--"); i += 2) {
        if (i + 1 >= argc) return 2;
        if (!strcmp(argv[i], "exit")) want = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "signal")) { want_sig = 1; want = atoi(argv[i + 1]); }
        else if (!strcmp(argv[i], "stdout-has")) has = argv[i + 1];
        else return 2;
    }
    if (i >= argc) return 2;
    int p[2];
    if (pipe(p)) return 2;
    fflush(NULL);
    pid_t c = fork();
    if (c == 0) {
        dup2(p[1], 1); close(p[0]); close(p[1]);
        execvp(argv[i + 1], argv + i + 1);
        _exit(127);
    }
    close(p[1]);
    static char out[1 << 16];
    size_t n = 0; ssize_t r;
    while ((r = read(p[0], out + n, sizeof out - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0;
    fputs(out, stdout);
    int st;
    waitpid(c, &st, 0);
    int ok = want_sig ? (WIFSIGNALED(st) && WTERMSIG(st) == want) : (WIFEXITED(st) && WEXITSTATUS(st) == want);
    if (has && !strstr(out, has)) { fprintf(stderr, "expect: stdout lacks '%s'\n", has); ok = 0; }
    if (!ok) fprintf(stderr, "expect: wrong status 0x%x (wanted %s %d)\n", st, want_sig ? "signal" : "exit", want);
    return !ok;
}
