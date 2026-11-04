/* swapper swap A B | chmod FILE MODE1 MODE2 (octal): flips forever, until killed, to race an exec. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE 2
#endif

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "swap")) {
        for (;;)
            if (syscall(SYS_renameat2, -100, argv[2], -100, argv[3], RENAME_EXCHANGE)) { perror("renameat2"); return 1; }
    }
    if (argc == 5 && !strcmp(argv[1], "chmod")) {
        mode_t a = (mode_t)strtol(argv[3], 0, 8), b = (mode_t)strtol(argv[4], 0, 8);
        for (;;)
            if (chmod(argv[2], a) || chmod(argv[2], b)) { perror("chmod"); return 1; }
    }
    return 2;
}
