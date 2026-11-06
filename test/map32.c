// map32: an mmap with MAP_32BIT succeeds below 2 GB, as old JITs and test/gather.c rely on.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void) {
    size_t len = 4 * 4096;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) { perror("mmap MAP_32BIT"); return 1; }
    if ((uintptr_t)p + len > 0x80000000u) { printf("FAIL: %p is above 2 GB\n", (void *)p); return 1; }
    memset(p, 1, len);
    puts("PASS");
    return 0;
}
