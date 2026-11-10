/* vfork_kill [runs] [zero]: a vfork child is SIGKILLed at a random point from its creation on, so some die before their
 * thread setup or while holding a lock of the translator's; the rest exec /bin/true. The parent must go on: create
 * a thread, vfork again, exit. zero kills at once, mostly before the child's thread setup. Same output natively and
 * translated: "PASS" as the last line. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define F_VM 0x100ull
#define F_VFORK 0x4000ull
#define F_PARENT_SETTID 0x100000ull

struct args { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; };

static volatile int child_tid, delay, killed;
static char *const targv[] = {"true", 0};
static char *const tenv[] = {0};

/* Raw syscalls only, as it shares the parent's memory. */
static __attribute__((noreturn)) void child(void) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(SYS_execve), "D"("/bin/true"), "S"(targv), "d"(tenv) : "rcx", "r11", "memory");
    __asm__ volatile("syscall" : : "a"(SYS_exit_group), "D"(127) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

/* The kernel stores the child's id before the child runs, so the killer can strike before the child's first
 * instruction. */
static pid_t clone3_vfork(void) {
    static char stack[65536] __attribute__((aligned(16)));
    struct args ca;
    long r;
    register long r8 __asm__("r8") = (long)child;
    memset(&ca, 0, sizeof ca);
    ca.flags = F_VM | F_VFORK | F_PARENT_SETTID;
    ca.parent_tid = (uint64_t)&child_tid;
    ca.exit_signal = SIGCHLD;
    ca.stack = (uint64_t)stack;
    ca.stack_size = sizeof stack;
    __asm__ volatile("syscall\n test %%rax,%%rax\n jnz 1f\n call *%%r8\n 1:\n"
                     : "=a"(r) : "0"(SYS_clone3), "D"(&ca), "S"(sizeof ca), "r"(r8) : "rcx", "r11", "rdx", "memory");
    return r < 0 ? (errno = (int)-r, -1) : (pid_t)r;
}

static void *killer(void *a) {
    (void)a;
    for (;;) {
        int t;
        while ((t = __atomic_load_n(&child_tid, __ATOMIC_ACQUIRE)) == 0) __builtin_ia32_pause();
        if (t < 0) return 0;
        for (int i = delay; i > 0; i--) __builtin_ia32_pause();
        kill(t, SIGKILL);
        __atomic_store_n(&child_tid, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&killed, 1, __ATOMIC_RELEASE);
    }
}

static void *nothing(void *a) { return a; }

int main(int argc, char **argv) {
    int runs = argc > 1 ? atoi(argv[1]) : 300, zero = argc > 2 && !strcmp(argv[2], "zero"), by_kill = 0, by_exec = 0, bad = 0, st;
    pthread_t k, t;
    if (pthread_create(&k, 0, killer, 0)) return 2;
    srand(1);
    for (int i = 0; i < runs; i++) {
        /* Most kills land in the first few tens of microseconds. */
        delay = zero ? 0 : rand() % 4 ? rand() % 2000 : rand() % 200000;
        killed = 0;
        pid_t c = clone3_vfork();
        if (c < 0) { printf("FAIL clone3: %s\n", strerror(errno)); return 1; }
        /* Reap only after the kill, so the id is not reused before it. */
        while (!__atomic_load_n(&killed, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
        if (waitpid(c, &st, 0) != c) { printf("FAIL wait: %s\n", strerror(errno)); return 1; }
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL) by_kill++;
        else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) by_exec++;
        else { printf("FAIL child status %#x\n", st); bad = 1; }
    }
    __atomic_store_n(&child_tid, -1, __ATOMIC_RELEASE);
    pthread_join(k, 0);
    printf("ok %d killed, %d exec'ed\n", by_kill, by_exec);

    if (pthread_create(&t, 0, nothing, 0) || pthread_join(t, 0)) { printf("FAIL thread after the kills\n"); bad = 1; }
    pid_t c = vfork();
    if (c == 0) { execl("/bin/true", "true", (char *)0); _exit(127); }
    if (c < 0 || waitpid(c, &st, 0) != c || st != 0) { printf("FAIL exec after the kills: %#x\n", st); bad = 1; }
    printf("%s\n", bad ? "FAILED" : "PASS");
    return bad;
}
