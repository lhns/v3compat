// ibl: ns per indirect branch under DynamoRIO: indirect calls through a table of 64 functions (each returning, so
// one call and one return per iteration) and a switch compiled to a jump table. Prints a checksum to compare.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

#define F(n) __attribute__((noinline)) static uint64_t f##n(uint64_t x) { return x * (2 * n + 1) + n; }
#define F8(a) F(a##0) F(a##1) F(a##2) F(a##3) F(a##4) F(a##5) F(a##6) F(a##7)
F8(1) F8(2) F8(3) F8(4) F8(5) F8(6) F8(7) F8(8)
#define R8(a) f##a##0, f##a##1, f##a##2, f##a##3, f##a##4, f##a##5, f##a##6, f##a##7,
static uint64_t (*const table[64])(uint64_t) = {R8(1) R8(2) R8(3) R8(4) R8(5) R8(6) R8(7) R8(8)};

__attribute__((noinline)) static uint64_t sw(uint64_t x, unsigned k) {
    switch (k & 15) {
    case 0: return x + 1;   case 1: return x ^ 3;   case 2: return x * 5;   case 3: return x - 7;
    case 4: return x + 11;  case 5: return x ^ 13;  case 6: return x * 17;  case 7: return x - 19;
    case 8: return x + 23;  case 9: return x ^ 29;  case 10: return x * 31; case 11: return x - 37;
    case 12: return x + 41; case 13: return x ^ 43; case 14: return x * 47; default: return x - 53;
    }
}

int main(int argc, char **argv) {
    uint64_t n = argc > 1 ? strtoull(argv[1], 0, 10) : 100000000ull, x = 1, k = 0x9E3779B97F4A7C15ull;
    double t0 = now();
    for (uint64_t i = 0; i < n; i++) {
        k = k * 6364136223846793005ull + 1442695040888963407ull;
        x = table[k >> 58](x);
    }
    double t1 = now();
    for (uint64_t i = 0; i < n; i++) {
        k = k * 6364136223846793005ull + 1442695040888963407ull;
        x = sw(x, (unsigned)(k >> 60));
    }
    double t2 = now();
    printf("micro icall_ret %.2f ns/iter\nmicro switch_ret %.2f ns/iter\nchecksum %016llx\n",
           (t1 - t0) * 1e9 / (double)n, (t2 - t1) * 1e9 / (double)n, (unsigned long long)x);
    return 0;
}
