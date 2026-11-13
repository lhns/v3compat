/* pcache_main LIB...: loads each LIB in turn (a test swaps or patches them between runs), prints what it
 * computes and unloads it, so the next one may land where it was. Each one's address goes to stderr. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    for (int a = 1; a < argc; a++) {
        void *h = dlopen(argv[a], RTLD_NOW);
        if (!h) { fprintf(stderr, "%s\n", dlerror()); return 3; }
        long (*work)(int) = (long (*)(int))dlsym(h, "lib_work");
        int (*mark)(void) = (int (*)(void))dlsym(h, "lib_mark");
        char buf[64];
        long s = 0;
        for (int i = 1; i <= 200; i++) {
            snprintf(buf, sizeof buf, "%d", i * 37);
            s += work(i) + atoi(buf);
        }
        printf("mark %x sum %ld\n", mark(), s);
        fprintf(stderr, "base %p\n", (void *)work);
        dlclose(h);
    }
    return 0;
}
