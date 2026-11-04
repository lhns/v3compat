#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>
/* ids [translated]: the ids and privilege state this process runs with; with "translated", also whether
 * libdynamorio is in its maps. */
int main(int argc, char **argv) {
    printf("euid=%d egid=%d nnp=%d", (int)geteuid(), (int)getegid(), (int)prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0));
    if (argc > 1) {
        FILE *f = fopen("/proc/self/maps", "r");
        char line[4096];
        int dr = 0;
        while (f && fgets(line, sizeof line, f)) if (strstr(line, "libdynamorio")) dr = 1;
        printf(" translated=%d", dr);
    }
    putchar(10);
    return 0;
}
