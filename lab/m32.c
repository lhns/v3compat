// m32 [maps]: one MAP_32BIT mapping, as test/gather.c makes it. On failure: /proc/self/maps on stderr, exit 2.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(int argc, char **argv) {
    void *p = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p != MAP_FAILED) printf("ok %p\n", p);
    else printf("MAP_32BIT failed: %s\n", strerror(errno));
    if (p != MAP_FAILED && argc < 2) return 0;
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) fputs(line, p == MAP_FAILED ? stderr : stdout);
    return p == MAP_FAILED ? 2 : 0;
}
