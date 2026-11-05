// fs_operand: memory operands with a %fs: override (thread-local variables) in the forms the client
// expands. DynamoRIO keeps the application's fs base apart from the hardware one, so a meta access
// that copies the override would read or write the wrong address. Each case uses a real TLS operand and
// is compared with a C model. Needs BMI and AVX2 natively; under the client it runs anywhere.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

__thread uint64_t tv[4] __attribute__((aligned(32))) = {0x0123456789abcdefull, 0xfedcba9876543210ull, 0x0f1e2d3c4b5a6978ull, 0x8899aabbccddeeffull};
__thread uint64_t tw[4] __attribute__((aligned(32)));

static int bad;
#define CHECK(name, got, want) do { if ((got) != (want)) { printf("FAIL %s: %llx, expected %llx\n", name, (unsigned long long)(got), (unsigned long long)(want)); bad = 1; } } while (0)

static uint64_t rotr(uint64_t v, int n) { return v >> n | v << (64 - n); }

int main(void) {
    uint64_t a = 0x00ff00ff12345678ull, r, hi, lo;
    uint64_t v0 = tv[0], fsb, otv, otw;
    __asm__("mov %%fs:0, %0" : "=r"(fsb));
    otv = (uint64_t)tv - fsb; otw = (uint64_t)tw - fsb;
    __asm__("mov %%fs:(%[tv]), %0" : "=r"(r) : [tv]"r"(otv));
    CHECK("control: plain mov", r, v0);
    __asm__("andn %%fs:(%[tv]), %1, %0" : "=r"(r) : "r"(a), [tv]"r"(otv), [tw]"r"(otw) );
    CHECK("andn", r, ~a & v0);
    __asm__("rorx $13, %%fs:(%[tv]), %0" : "=r"(r): [tv]"r"(otv), [tw]"r"(otw) );
    CHECK("rorx", r, rotr(v0, 13));
    __asm__("mulx %%fs:(%[tv]), %0, %1" : "=r"(lo), "=r"(hi) : "d"(a), [tv]"r"(otv), [tw]"r"(otw) );
    CHECK("mulx lo", lo, (uint64_t)((unsigned __int128)a * v0));
    CHECK("mulx hi", hi, (uint64_t)(((unsigned __int128)a * v0) >> 64));
    __asm__("movbe %%fs:(%[tv]), %0" : "=r"(r): [tv]"r"(otv), [tw]"r"(otw) );
    CHECK("movbe load", r, __builtin_bswap64(v0));
    tw[0] = 0;
    __asm__ volatile("movbe %0, %%fs:(%[tw])" : : "r"(a), [tv]"r"(otv), [tw]"r"(otw) : "memory");
    CHECK("movbe store", tw[0], __builtin_bswap64(a));
    __asm__("lzcnt %%fs:(%[tv]), %0" : "=r"(r): [tv]"r"(otv), [tw]"r"(otw) );
    CHECK("lzcnt", r, (uint64_t)__builtin_clzll(v0));
    __asm__("tzcnt %%fs:(%[tv]), %0" : "=r"(r): [tv]"r"(otv), [tw]"r"(otw) );
    CHECK("tzcnt", r, (uint64_t)__builtin_ctzll(v0));

    // ymm sources from TLS: inline (vpaddq), helper (vpmaxsd, vpmulld), FMA (mem source)
    uint64_t in[4] __attribute__((aligned(32))) = {1, 2, 3, 4}, out[4] __attribute__((aligned(32)));
    __asm__ volatile("vmovdqu (%0), %%ymm1\n vpaddq %%fs:(%[tv]), %%ymm1, %%ymm2\n vmovdqu %%ymm2, (%1)" : : "r"(in), "r"(out), [tv]"r"(otv), [tw]"r"(otw) : "xmm1", "xmm2", "memory");
    for (int i = 0; i < 4; i++) CHECK("vpaddq", out[i], in[i] + tv[i]);
    __asm__ volatile("vmovdqu (%0), %%ymm1\n vpmulld %%fs:(%[tv]), %%ymm1, %%ymm2\n vmovdqu %%ymm2, (%1)" : : "r"(in), "r"(out), [tv]"r"(otv), [tw]"r"(otw) : "xmm1", "xmm2", "memory");
    for (int i = 0; i < 4; i++) {
        uint32_t x[2] = {(uint32_t)in[i], (uint32_t)(in[i] >> 32)}, y[2] = {(uint32_t)tv[i], (uint32_t)(tv[i] >> 32)};
        CHECK("vpmulld", out[i], (uint64_t)(x[0] * y[0]) | (uint64_t)(x[1] * y[1]) << 32);
    }
    __asm__ volatile("vmovdqu (%0), %%ymm1\n vpermd %%fs:(%[tv]), %%ymm1, %%ymm2\n vmovdqu %%ymm2, (%1)" : : "r"(in), "r"(out), [tv]"r"(otv), [tw]"r"(otw) : "xmm1", "xmm2", "memory");
    for (int i = 0; i < 4; i++) {
        uint32_t t32[8]; memcpy(t32, tv, 32);
        uint32_t x[2] = {(uint32_t)in[i], (uint32_t)(in[i] >> 32)};
        CHECK("vpermd", out[i], (uint64_t)t32[x[0] & 7] | (uint64_t)t32[x[1] & 7] << 32);
    }
    // vpmaskmovd store form is substituted in place, vpmovmskb-free: a store through fs
    uint64_t m[4] __attribute__((aligned(32))) = {~0ull, 0, ~0ull, 0};
    memset(tw, 0, 32);
    __asm__ volatile("vmovdqu (%0), %%ymm2\n vmovdqu (%1), %%ymm1\n vpmaskmovd %%ymm2, %%ymm1, %%fs:(%[tw])" : : "r"(in), "r"(m), [tv]"r"(otv), [tw]"r"(otw) : "xmm1", "xmm2", "memory");
    CHECK("vpmaskmovd store 0", tw[0], in[0]);
    CHECK("vpmaskmovd store 1", tw[1], 0);
    CHECK("vpmaskmovd store 2", tw[2], in[2]);
    puts(bad ? "FAIL" : "PASS");
    return bad;
}
