/* close_range [exec]: close_range up to INT_MAX (CPython's close_fds) and ~0U (glibc's closefrom()) must be prompt
 * under DynamoRIO, close exactly the range (or mark it close-on-exec), and leave DynamoRIO working: std fds closed in
 * a forked child, then an exec'd child. Prints PASS. */
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef CLOSE_RANGE_CLOEXEC
#define CLOSE_RANGE_CLOEXEC 4
#endif
static int fails;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL %s\n", what); fails++; } }
static long cr(unsigned first, unsigned last, unsigned flags) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    long r = syscall(SYS_close_range, first, last, flags);
    clock_gettime(CLOCK_MONOTONIC, &b);
    double s = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
    printf("close_range(%u, %u, %u) = %ld in %.3f s\n", first, last, flags, r, s);
    check(r == 0 && s < 2, "close_range result or time");
    return r;
}
static int is_open(int fd) { return fcntl(fd, F_GETFD) != -1; }
int main(int argc, char **argv) {
    if (argc > 1) { puts("PASS exec"); return 0; }
    int fd[4];
    for (int i = 0; i < 4; i++) fd[i] = dup(1);
    cr(fd[2], INT_MAX, 0);
    check(is_open(fd[0]) && is_open(fd[1]) && !is_open(fd[2]) && !is_open(fd[3]), "INT_MAX range");
    fd[2] = dup(1); fd[3] = dup(1);
    cr(fd[1], ~0U, CLOSE_RANGE_CLOEXEC);
    check(fcntl(fd[0], F_GETFD) == 0 && fcntl(fd[1], F_GETFD) == FD_CLOEXEC && fcntl(fd[3], F_GETFD) == FD_CLOEXEC,
          "cloexec range");
    cr(fd[1], ~0U, 0);
    check(is_open(fd[0]) && !is_open(fd[1]) && !is_open(fd[3]), "~0U range");
    pid_t p = fork();
    if (p == 0) _exit(syscall(SYS_close_range, 0, ~0U, 0) == 0 && !is_open(0) && !is_open(1) && !is_open(fd[0]) ? 0 : 1);
    int st;
    check(waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 0, "std fds in range, forked child");
    fflush(stdout);
    cr(3, ~0U, 0);
    if (fails) return 1;
    execl("/proc/self/exe", argv[0], "exec", (char *)0);
    puts("FAIL exec");
    return 1;
}
