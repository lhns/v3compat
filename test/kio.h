/* kio: the kernel raises SIGXCPU in a chosen thread, as for RLIMIT_CPU but thousands of times a second. A pipe's read
 * end with O_ASYNC and F_SETSIG gets the signal on every write, with si_code POLL_IN; DynamoRIO holds a signal it may
 * not always delay to the fragment's end only when si_code says kill or tgkill (<= 0), so this one is delivered at once,
 * from wherever the thread is in the code cache. kio_add() from the thread to be hit; kio_start() runs the writer. */
#ifndef KIO_H
#define KIO_H
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#define KIO_MAX 64
static int kio_fd[KIO_MAX][2];
static volatile int kio_n;
static volatile int kio_stop;

static int kio_add(void) {
    int i = __atomic_fetch_add(&kio_n, 1, __ATOMIC_SEQ_CST);
    if (i >= KIO_MAX || pipe2(kio_fd[i], O_NONBLOCK)) return -1;
    struct f_owner_ex o = {F_OWNER_TID, (pid_t)syscall(SYS_gettid)};
    if (fcntl(kio_fd[i][0], F_SETSIG, SIGXCPU) || fcntl(kio_fd[i][0], F_SETOWN_EX, &o) ||
        fcntl(kio_fd[i][0], F_SETFL, O_ASYNC | O_NONBLOCK))
        return -1;
    return 0;
}

static void *kio_writer(void *arg) {
    (void)arg;
    char c = 0;
    while (!kio_stop) {
        int n = kio_n < KIO_MAX ? kio_n : KIO_MAX;
        for (int i = 0; i < n; i++)
            if (write(kio_fd[i][1], &c, 1) == 1 && read(kio_fd[i][0], &c, 1) < 0) c = 0;
        usleep(50);
    }
    return NULL;
}

static int kio_start(void) {
    pthread_t t;
    return pthread_create(&t, NULL, kio_writer, NULL);
}
#endif
