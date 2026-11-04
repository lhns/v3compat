// lzcnt dl LIB     dlopen LIB and call its lz()
// lzcnt exec PROG  exec PROG (a static lzcnt run) from this process
// lzcnt self       run lzcnt here
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static unsigned long lz_here(unsigned long x) {
    unsigned long r;
    __asm__("lzcnt %1, %0" : "=r"(r) : "r"(x));
    return r;
}

static int check(unsigned long (*f)(unsigned long)) {
    unsigned long in[] = {0, 1, 0x80, 1ul << 63, 0x123456789abcdeful};
    unsigned long want[] = {64, 63, 56, 0, 7};
    for (int i = 0; i < 5; i++)
        if (f(in[i]) != want[i]) {
            printf("FAIL lzcnt(%lx) = %lu, wanted %lu\n", in[i], f(in[i]), want[i]);
            return 1;
        }
    puts("PASS");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "dl")) {
        void *h = dlopen(argv[2], RTLD_NOW);
        unsigned long (*f)(unsigned long) = h ? (unsigned long (*)(unsigned long))dlsym(h, "lz") : 0;
        if (!f) { puts("FAIL dlopen"); return 1; }
        return check(f);
    }
    if (argc > 2 && !strcmp(argv[1], "exec")) {
        execl(argv[2], argv[2], "self", (char *)0);
        return 127;
    }
    return check(lz_here);
}
