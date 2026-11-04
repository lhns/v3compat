// lzcnt dl LIB     dlopen LIB and call its lz()
// lzcnt exec PROG  exec PROG (a static lzcnt run) from this process
// lzcnt self       run lzcnt here
// lzcnt translated lzcnt here, and PASS only if libdynamorio is mapped: on a CPU with lzcnt a child that
//                  escaped the client would pass the arithmetic alone
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

static int translated(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[4096];
    int found = 0;
    while (f && fgets(line, sizeof line, f)) if (strstr(line, "libdynamorio")) found = 1;
    if (f) fclose(f);
    if (!found) { puts("FAIL not translated: libdynamorio is not in /proc/self/maps"); return 1; }
    return check(lz_here);
}

int main(int argc, char **argv) {
    /* argc 0 is an exec with a NULL argv, which carries no mode */
    if (argc == 0 || (argc > 1 && !strcmp(argv[1], "translated"))) return translated();
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
