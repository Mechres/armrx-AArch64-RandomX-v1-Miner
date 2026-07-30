/*
 * mul_latency_bench.c — Track F3 premise test.
 *
 * Measures the LATENCY of integer multiply on this Cortex-A53 in a
 * serialized RAW dependency chain, for four variants:
 *   v0: scalar  `mul`   (64-bit lower product)
 *   v1: scalar  `umulh` (64-bit high product)
 *   v2: NEON    `mul  v.4s` (4x 32-bit products / instruction)
 *   v3: NEON    `umull v.2d`(2x 32->64 products / instruction)
 *
 * NEON variants keep state entirely in SIMD registers throughout the
 * inner K-chain (no GPR round-trip per multiply). The constant multiplier
 * is loaded once before the loop.
 *
 * Decision rule (master plan Track F3 gate):
 *   - If NEON mul latency < scalar mul latency AND the NEON pipe is
 *     dual-issue independent of the scalar integer pipe, offload is
 *     theoretically viable.
 *   - Otherwise F3 is a measured negative and closes.
 *
 * Build: gcc -O2 -static -o mul_latency_bench mul_latency_bench.c
 * Run:   taskset -c 3 perf stat -e cycles,instructions ./mul_latency_bench <variant> <iters> <K>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static volatile uint64_t sink;

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <variant 0..3> <iters> <K>\n", argv[0]);
        return 2;
    }
    int  variant = atoi(argv[1]);
    uint64_t iters = strtoull(argv[2], NULL, 10);
    int  K       = atoi(argv[3]);

    uint64_t a = 0x123456789abcdef0ULL;
    uint64_t b = 0x0f1e2d3c4b5a6978ULL;
    uint64_t x = a;
    uint64_t res = 0;

    switch (variant) {

    /* ---- v0: scalar mul (64b lower) ---- */
    case 0: {
        uint64_t t = x;
        for (uint64_t i = 0; i < iters; i++) {
            for (int k = 0; k < K; k++) {
                asm volatile("mul %0, %0, %1" : "+r"(t) : "r"(b));
            }
        }
        res = t;
        break;
    }

    /* ---- v1: scalar umulh (64b high) ---- */
    case 1: {
        uint64_t t = x;
        for (uint64_t i = 0; i < iters; i++) {
            for (int k = 0; k < K; k++) {
                asm volatile("umulh %0, %0, %1" : "+r"(t) : "r"(b));
            }
        }
        res = t;
        break;
    }

    /* ---- v2: NEON mul v.4s (4x 32b) ---- */
    case 2: {
        /* Load constant multiplier into v1 once */
        asm volatile("movi v1.4s, #3" : : : "v1");
        uint64_t t = x;
        for (uint64_t i = 0; i < iters; i++) {
            /* Transfer initial value to NEON reg */
            asm volatile("fmov d0, %[t]" : : [t]"r"(t) : "v0");
            /* K dependent multiplies — pure RAW chain in v0 */
            for (int k = 0; k < K; k++) {
                asm volatile("mul v0.4s, v0.4s, v1.4s" : : : "v0");
            }
            /* Extract result back to GPR */
            asm volatile("fmov %[t], d0" : [t]"=r"(t) : : "v0");
        }
        res = t;
        break;
    }

    /* ---- v3: NEON umull v.2d (2x 32->64) ---- */
    case 3: {
        /* Load constant multiplier into v1 (lower 32b of each 64b lane) */
        asm volatile("movi v1.2s, #3" : : : "v1");
        uint64_t t = x;
        for (uint64_t i = 0; i < iters; i++) {
            asm volatile("fmov d0, %[t]" : : [t]"r"(t) : "v0");
            for (int k = 0; k < K; k++) {
                asm volatile("umull v0.2d, v0.2s, v1.2s" : : : "v0");
            }
            asm volatile("fmov %[t], d0" : [t]"=r"(t) : : "v0");
        }
        res = t;
        break;
    }

    default:
        fprintf(stderr, "bad variant\n");
        return 2;
    }

    sink = res;
    printf("variant=%d iters=%llu K=%d result=%llx\n",
           variant, (unsigned long long)iters, K, (unsigned long long)res);
    return 0;
}
