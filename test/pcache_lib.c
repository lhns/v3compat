/* The translation cache test's library: rip-relative data, a jump table, calls out through the PLT. */
#include <string.h>
#ifndef PCACHE_EXTRA
#define PCACHE_EXTRA 0 /* another build of the library: other code, other build-id */
#endif

static int table[64];
static const char *names[] = {"zero", "one", "two", "three", "four", "five", "six", "seven"};

int lib_mark(void) { return 0x11223344; } /* pcache.sh patches this constant */

long lib_work(int n) {
    long s = 0;
    for (int i = 0; i < n; i++) {
        table[i & 63] += i;
        switch (i % 7) {
        case 0: s += table[(i * 3) & 63]; break;
        case 1: s ^= (long)strlen(names[i & 7]); break;
        case 2: s -= i; break;
        case 3: s += names[(i >> 1) & 7][0]; break;
        case 4: s *= 3; break;
        case 5: s += table[i & 63] >> 2; break;
        default: s += 11 + PCACHE_EXTRA;
        }
    }
    return s + lib_mark();
}
