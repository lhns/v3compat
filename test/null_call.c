/* null_call: a call through a NULL pointer faults at pc 0 with address 0, and the app's handler must see exactly that
 * (it is not a translation failure). Prints "ok" after several rounds. */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

static sigjmp_buf env;
static volatile int bad;

static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void)sig;
    ucontext_t *uc = ctx;
    if (uc->uc_mcontext.gregs[REG_RIP] != 0 || si->si_addr != NULL) bad = 1;
    siglongjmp(env, 1);
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, 0);
    void (*volatile fn)(void) = 0;
    int n = 0;
    for (int i = 0; i < 5; i++)
        if (sigsetjmp(env, 1)) n++;
        else fn();
    if (n != 5 || bad) {
        printf("FAIL: handled=%d bad=%d\n", n, bad);
        return 1;
    }
    printf("ok\n");
    return 0;
}
