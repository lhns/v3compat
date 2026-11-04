/* clone3 as posix_spawn uses it, with CLONE_CLEAR_SIGHAND: DynamoRIO must keep its own handlers in the child
 * and the app must see the kernel's result. Same output natively and translated: "PASS" as the last line. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define F_VM 0x100ull
#define F_SIGHAND 0x800ull
#define F_VFORK 0x4000ull
#define F_CLEAR_SIGHAND 0x100000000ull

struct args { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; };
struct ksa { void *handler; unsigned long flags; void *restorer; uint64_t mask; };

extern char **environ;
static int bad;
static volatile int seen;
static void on_sig(int s) { seen = s; }
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) bad = 1;
}

/* A raw syscall, as the child may share the parent's memory and must not touch errno or stdio. */
static long sc(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return r;
}

/* The child's view of its handlers, returned as its exit status: 0 if they are the kernel's result. */
static int child_view(void) {
    struct ksa a;
    if (sc(SYS_rt_sigaction, SIGUSR1, 0, (long)&a, 8)) return 10;
    if (a.handler != SIG_DFL || a.flags != 0) return 11;
    if (sc(SYS_rt_sigaction, SIGUSR2, 0, (long)&a, 8)) return 12;
    if (a.handler != SIG_IGN) return 13;
    if (sc(SYS_rt_sigaction, SIGTERM, 0, (long)&a, 8)) return 14;
    if (a.handler != SIG_DFL) return 15;
    return 0;
}

/* clone3 on a new stack if there is one; the child runs fn, on a stack only it uses, and exits with its result. */
static long clone3_run(struct args *ca, int (*fn)(void)) {
    long r;
    register long r8 __asm__("r8") = (long)fn;
    __asm__ volatile("syscall\n test %%rax,%%rax\n jnz 1f\n and $-16,%%rsp\n call *%%r8\n"
                     "mov %%eax,%%edi\n mov $231,%%eax\n syscall\n 1:\n"
                     : "=a"(r) : "0"(SYS_clone3), "D"(ca), "S"(sizeof *ca), "r"(r8)
                     : "rcx", "r11", "rdx", "memory");
    return r;
}

static int child_raise(void) {
    sc(SYS_kill, sc(SYS_getpid, 0, 0, 0, 0), SIGUSR1, 0, 0);
    return 3; /* reached only if the signal did not kill it */
}

static int wait_for(long pid) {
    int st = -1;
    if (pid > 0) waitpid((pid_t)pid, &st, 0);
    return st;
}

int main(void) {
    enum { STACK = 1 << 16 };
    char *stk = mmap(0, STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, 0);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);

    /* The form posix_spawn uses, then a fork-like one. */
    struct args vf = {F_VM | F_VFORK | F_CLEAR_SIGHAND, 0, 0, 0, SIGCHLD, (uint64_t)stk, STACK, 0};
    int st = wait_for(clone3_run(&vf, child_view));
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "vfork-like child sees cleared handlers");
    check(vf.flags == (F_VM | F_VFORK | F_CLEAR_SIGHAND) && vf.stack == (uint64_t)stk && vf.stack_size == STACK,
          "the app's clone_args is unchanged");

    struct args fk = {F_CLEAR_SIGHAND, 0, 0, 0, SIGCHLD, 0, 0, 0};
    st = wait_for(clone3_run(&fk, child_view));
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "fork-like child sees cleared handlers");
    check(fk.flags == F_CLEAR_SIGHAND, "the app's clone_args is unchanged by a fork");

    /* The default action, not the parent's handler, and the parent's own table stays. */
    st = wait_for(clone3_run(&vf, child_raise));
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGUSR1, "vfork-like child dies of a signal whose handler was cleared");
    st = wait_for(clone3_run(&fk, child_raise));
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGUSR1, "fork-like child dies of a signal whose handler was cleared");
    raise(SIGUSR1);
    check(seen == SIGUSR1, "the parent keeps its handler");

    /* The kernel rejects the pair; hiding the flag from it must not change that. */
    struct args both = {F_VM | F_SIGHAND | F_CLEAR_SIGHAND, 0, 0, 0, SIGCHLD, (uint64_t)stk, STACK, 0};
    long r = sc(SYS_clone3, (long)&both, sizeof both, 0, 0);
    check(r == -EINVAL, "CLONE_CLEAR_SIGHAND with CLONE_SIGHAND is EINVAL");

    /* What glibc does, end to end. A static binary's child faults harmlessly inside DynamoRIO on the way. */
    pid_t c;
    char *av[] = {"/bin/true", 0};
    int e = posix_spawn(&c, "/bin/true", 0, 0, av, environ);
    st = e ? -1 : wait_for(c);
    check(e == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "posix_spawn");

    puts(bad ? "FAIL" : "PASS");
    return bad;
}
