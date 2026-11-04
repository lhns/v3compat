#include <stdio.h>
#include <string.h>
#include <unistd.h>
/* proc maps: report whether DynamoRIO is mapped. proc exec: do that in an exec'd child. */
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "exec")) {
        execl("/proc/self/exe", argv[0], "maps", (char *)0);
        return 127;
    }
    FILE *f = fopen("/proc/self/maps", "r");
    char line[4096];
    int found = 0;
    while (f && fgets(line, sizeof line, f)) if (strstr(line, "libdynamorio")) found = 1;
    puts(found ? "dynamorio-mapped" : "dynamorio-absent");
    return found ? 0 : 1;
}
