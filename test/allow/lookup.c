/* lookup: allow_listed() must find every key of both tables of allowlist.inc. The tables are static in allow.c. */
#include <stdio.h>
#include "allow.c"

int main(void) {
    size_t ne = sizeof allow_emulated / sizeof *allow_emulated, nn = sizeof allow_native / sizeof *allow_native, bad = 0;
    for (size_t i = 0; i < ne; i++) bad += !allow_listed(allow_emulated[i], true);
    for (size_t i = 0; i < nn; i++) bad += !allow_listed(allow_native[i], false);
    printf("%zu emulated and %zu native keys, %zu not found\n", ne, nn, bad);
    puts(bad ? "FAIL" : "PASS");
    return bad != 0;
}
