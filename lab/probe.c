/* probe: one store to a read-only page; must die of SIGSEGV, and DynamoRIO's lab prints must show it. */
#include <sys/mman.h>
int main(void) {
    char *p = mmap(0, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    p[8] = 1;
    return 0;
}
