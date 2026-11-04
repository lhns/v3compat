#include <stdio.h>
#include <string.h>
#include <unistd.h>
/* argv0: print argv[0]. argv0 exec PROG: exec PROG under the name "custom name". */
int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "exec")) {
        execl(argv[2], "custom name", (char *)0);
        return 127;
    }
    printf("argv0=%s\n", argv[0]);
    return 0;
}
