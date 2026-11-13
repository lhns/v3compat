#include <stdio.h>
#include <string.h>
#include <unistd.h>
/* proc maps: report whether DynamoRIO is mapped. proc exec: do that in an exec'd child.
 * proc gencode: whether DynamoRIO keeps its generated code read-only (an anonymous r-x mapping; -protect_mask). */
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "exec")) {
        execl("/proc/self/exe", argv[0], "maps", (char *)0);
        return 127;
    }
    FILE *f = fopen("/proc/self/maps", "r");
    char line[4096];
    int found = 0;
    if (argc > 1 && !strcmp(argv[1], "gencode")) {
        char perm[8], path[4096];
        while (f && fgets(line, sizeof line, f))
            if (sscanf(line, "%*s %7s %*s %*s %*s %4095s", perm, path) == 1 && !strcmp(perm, "r-xp")) found = 1;
        puts(found ? "gencode-readonly" : "gencode-writable");
        return 0;
    }
    while (f && fgets(line, sizeof line, f)) if (strstr(line, "libdynamorio")) found = 1;
    puts(found ? "dynamorio-mapped" : "dynamorio-absent");
    return found ? 0 : 1;
}
