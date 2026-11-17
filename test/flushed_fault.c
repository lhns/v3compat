/* flushed_fault SECONDS: a thread faults over and over in a loop on a writable code page (a JIT's safepoint poll)
 * while a writer dirties a byte of that page that is never executed. Each write flushes the page, and after
 * -ro2sandbox_threshold writes DynamoRIO sandboxes it. A fault in a block flushed just before used to be translated
 * against a sandboxed rebuild of that block, which did not match the emitted code, and the handler saw pc 0 (fork fix
 * fix-xl8-flushed-unsandboxed). The handler checks the pc of every fault. Prints "ok" and exits 0 when all were right. */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

enum { ADDS = 250, LOAD = ADDS * 4, DATA = 3072 };

static unsigned char *page, *bad_page;
static atomic_int stop;
static atomic_ulong faults, writes;

static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    ucontext_t *uc = ctx;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    if (pc != (uintptr_t)page + LOAD || si->si_addr != bad_page) {
        char m[128];
        int n = snprintf(m, sizeof m, "FAIL: fault at pc %#lx addr %p, expected pc %p\n", (unsigned long)pc,
                         si->si_addr, (void *)(page + LOAD));
        write(1, m, n);
        _exit(1);
    }
    uc->uc_mcontext.gregs[REG_RIP] = pc + 2;   /* skip the load */
    atomic_fetch_add(&faults, 1);
}

static void *runner(void *arg) {
    (void)arg;
    void (*f)(long, void *) = (void (*)(long, void *))page;
    while (!atomic_load(&stop)) f(100, bad_page);
    return 0;
}

static void *writer(void *arg) {
    (void)arg;
    while (!atomic_load(&stop)) {
        ((volatile unsigned char *)page)[DATA]++;
        atomic_fetch_add(&writes, 1);
        struct timespec t = {0, 20000};
        nanosleep(&t, 0);
    }
    return 0;
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    page = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bad_page = mmap(0, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED || bad_page == MAP_FAILED) return 2;
    /* loop: ADDS x add rcx, 1; mov eax, [rsi] (faults); dec rdi; jnz loop; ret. One block of ADDS + 3. */
    unsigned char *p = page;
    for (int i = 0; i < ADDS; i++) { memcpy(p, "\x48\x83\xc1\x01", 4); p += 4; }
    memcpy(p, "\x8b\x06\x48\xff\xcf\x0f\x85", 7); p += 7;
    int32_t back = (int32_t)(page - (p + 4));
    memcpy(p, &back, 4); p += 4;
    *p = 0xc3;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    pthread_t r, w;
    pthread_create(&r, 0, runner, 0);
    pthread_create(&w, 0, writer, 0);
    struct timespec t = {secs, 0};
    nanosleep(&t, 0);
    atomic_store(&stop, 1);
    pthread_join(r, 0);
    pthread_join(w, 0);
    printf("ok faults=%lu writes=%lu\n", atomic_load(&faults), atomic_load(&writes));
    return 0;
}
