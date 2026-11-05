/* vfork_sig [term]: vfork children die of signals before exec; the parent must see each death and go on, with
 * another thread running. Same output natively and translated: "PASS" as the last line. With term the parent then
 * dies of SIGTERM itself, which is a process exit that must not wait for a dead child. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define F_VM 0x100ull
#define F_VFORK 0x4000ull
#define F_CLEAR_SIGHAND 0x100000000ull

struct args { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; };

static volatile int stop, caught;
static volatile unsigned long spins;
static int bad;

static void on_term(int s) { caught = s; }
static void *spin(void *a) { (void)a; while (!stop) spins++; return 0; }

static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

/* What the child does; none of it returns. Raw syscalls, as it shares the parent's memory. */
static __attribute__((noreturn)) void die(int how) {
    long self = sc2(SYS_getpid, 0, 0);
    switch (how) {
    case 0: sc2(SYS_kill, self, SIGTERM); break;
    case 1: *(volatile int *)8 = 1; break;
    case 2: sc2(SYS_kill, self, SIGKILL); break;
    case 3: {
        /* Default action asked for by the child, with the parent's handler inherited. */
        struct { void *h; unsigned long f; void *r; uint64_t m; } a = {SIG_DFL, 0, 0, 0};
        register long r10 __asm__("r10") = 8;
        long r;
        __asm__ volatile("syscall" : "=a"(r) : "a"(SYS_rt_sigaction), "D"(SIGTERM), "S"(&a), "d"(0), "r"(r10)
                         : "rcx", "r11", "memory");
        sc2(SYS_kill, self, SIGTERM);
        break;
    }
    case 4: sc2(SYS_kill, self, SIGABRT); break;
    }
    sc2(SYS_exit_group, 99, 0);
    __builtin_unreachable();
}

static void expect_death(const char *what, pid_t c, int sig) {
    int st = 0;
    if (c < 0 || waitpid(c, &st, 0) != c) { printf("FAIL %s: fork or wait: %s\n", what, strerror(errno)); bad = 1; return; }
    if (WIFSIGNALED(st) && WTERMSIG(st) == sig) printf("ok %s\n", what);
    else { printf("FAIL %s: status %#x, wanted signal %d\n", what, st, sig); bad = 1; }
}

static pid_t vfork_die(int how) {
    pid_t c = vfork();
    if (c == 0) die(how);
    return c;
}

/* As posix_spawn: clone3 with CLONE_VM, CLONE_VFORK, CLONE_CLEAR_SIGHAND, on a stack only the child uses. */
static pid_t clone3_die(int how) {
    static char stack[65536] __attribute__((aligned(16)));
    struct args ca;
    long r;
    register long r8 __asm__("r8") = (long)die;
    register long r9 __asm__("r9") = how;
    memset(&ca, 0, sizeof ca);
    ca.flags = F_VM | F_VFORK | F_CLEAR_SIGHAND;
    ca.exit_signal = SIGCHLD;
    ca.stack = (uint64_t)stack;
    ca.stack_size = sizeof stack;
    __asm__ volatile("syscall\n test %%rax,%%rax\n jnz 1f\n mov %%r9d,%%edi\n call *%%r8\n 1:\n"
                     : "=a"(r) : "0"(SYS_clone3), "D"(&ca), "S"(sizeof ca), "r"(r8), "r"(r9)
                     : "rcx", "r11", "rdx", "memory");
    return r < 0 ? (errno = (int)-r, -1) : (pid_t)r;
}

int main(int argc, char **argv) {
    pthread_t t;
    int st;
    pid_t c;
    struct rlimit nocore = {0, 0};
    setrlimit(RLIMIT_CORE, &nocore);
    if (pthread_create(&t, 0, spin, 0)) return 2;

    for (int i = 0; i < 3; i++) {
        expect_death("vfork, SIGTERM from itself", vfork_die(0), SIGTERM);
        expect_death("vfork, SIGSEGV by a fault", vfork_die(1), SIGSEGV);
        expect_death("vfork, SIGKILL", vfork_die(2), SIGKILL);
        expect_death("vfork, SIGABRT", vfork_die(4), SIGABRT);
        expect_death("clone3, SIGTERM", clone3_die(0), SIGTERM);
        expect_death("clone3, SIGSEGV by a fault", clone3_die(1), SIGSEGV);
        expect_death("clone3, SIGKILL", clone3_die(2), SIGKILL);
    }

    /* The child's table is its own: its death must leave the parent's handler in place. */
    signal(SIGTERM, on_term);
    expect_death("vfork, SIG_DFL set by the child", vfork_die(3), SIGTERM);
    kill(getpid(), SIGTERM);
    for (int i = 0; i < 100000000 && !caught; i++) {}
    printf("%s parent's handler survives\n", caught == SIGTERM ? "ok" : "FAIL");
    if (caught != SIGTERM) bad = 1;
    signal(SIGTERM, SIG_DFL);

    /* A later vfork and exec still work, and so does a new thread. */
    c = vfork();
    if (c == 0) { execl("/bin/true", "true", (char *)0); _exit(127); }
    st = -1;
    if (waitpid(c, &st, 0) != c || st != 0) { printf("FAIL exec after the deaths: %#x\n", st); bad = 1; }

    stop = 1;
    pthread_join(t, 0);
    stop = 0;
    if (pthread_create(&t, 0, spin, 0)) return 2;
    stop = 1;
    pthread_join(t, 0);

    if (argc > 1 && !strcmp(argv[1], "term")) {
        fflush(stdout);
        kill(getpid(), SIGTERM);
        for (;;) pause();
    }
    printf("%s\n", bad ? "FAILED" : "PASS");
    return bad;
}
