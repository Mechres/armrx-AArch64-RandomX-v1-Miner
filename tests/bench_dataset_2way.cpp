// Track D1 (docs/plans/track-d1-superscalar-interleave-plan-20260728.md):
// the actual decision gate -- L1 instruction-cache refill rate, 1-way vs
// 2-way dataset-item derivation. Not part of the default ctest run (like
// bench_opcodes/bench_armrx, this is a perf-stat-wrapped microbenchmark
// invoked directly on device, taskset-pinned).
//
// Usage: bench_dataset_2way <1way|2way> <iterations> [warmup_iterations]
//
// Both modes derive the SAME total number of dataset items (iterations x 2)
// from the SAME cache, against the SAME seed's superscalar program list, so
// a perf-stat wrapper comparing the two binaries' runs is an apples-to-
// apples comparison of "N items via the existing single-stream path,
// called twice per iteration" vs "N items via the new 2-way path, called
// once per iteration for two independent item numbers." Item numbers are
// drawn from the same wide range the differential test used, so cache-line
// access patterns are representative rather than degenerately repetitive.

#include "armrx/argon2.hpp"
#include "armrx/jit_compiler_a64.hpp"
#include "armrx/jit_dataset_2way.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <1way|2way> <iterations> [warmup_iterations]\n";
        return 2;
    }
    const std::string mode = argv[1];
    const long long iterations = std::atoll(argv[2]);
    const long long warmup = (argc >= 4) ? std::atoll(argv[3]) : 0;

    if (mode != "1way" && mode != "2way") {
        std::cerr << "mode must be 1way or 2way, got: " << mode << "\n";
        return 2;
    }

    armrx::Argon2dCache cache;
    {
        const std::string seed = "track_d1_l1i_gate_seed";
        std::vector<std::byte> key;
        key.reserve(seed.size());
        for (char c : seed) key.push_back(static_cast<std::byte>(c));
        cache.initialize(key);
    }
    const void* cache_ptr = reinterpret_cast<const void*>(cache.blocks().data());

    armrx::JitCompilerA64 jit1way;
    jit1way.generateSuperscalarHash(cache.programs(), cache.reciprocal_cache());
    const auto fn1way = jit1way.getCalcDatasetItemFunc();

    armrx::JitDataset2Way jit2way;
    {
        auto scheduler = [&jit1way](const armrx::SuperscalarProgram& prog) {
            return jit1way.computeSuperscalarEmitOrder(prog);
        };
        jit2way.generate(cache.programs(), cache.reciprocal_cache(), scheduler);
    }
    const auto fn2way = jit2way.getFunc();

    std::cout << "1-way code size: " << jit1way.getCodeSize() << " + derivation region (see CalcDatasetItemSize)\n"
              << "2-way code size: " << jit2way.getCodeSize() << " bytes\n";

    std::mt19937_64 rng(0xB6B6B6B6ULL);
    std::uniform_int_distribution<std::uint64_t> item_dist(0, (1ULL << 40) - 1);

    std::array<std::byte, 64> outA{};
    std::array<std::byte, 64> outB{};

    const long long total = warmup + iterations;
    for (long long i = 0; i < total; ++i) {
        const std::uint64_t itemA = item_dist(rng);
        const std::uint64_t itemB = item_dist(rng);
        if (mode == "1way") {
            fn1way(cache_ptr, outA.data(), itemA);
            fn1way(cache_ptr, outB.data(), itemB);
        } else {
            fn2way(cache_ptr, outA.data(), itemA, outB.data(), itemB);
        }
    }

    // Prevent the loop from being optimized away; outputs are data-dependent
    // on every iteration already, this just gives the compiler an observable
    // use of the final result.
    unsigned checksum = 0;
    for (auto b : outA) checksum += static_cast<unsigned char>(b);
    for (auto b : outB) checksum += static_cast<unsigned char>(b);
    std::cout << mode << ": " << iterations << " iterations (" << (iterations * 2) << " items), warmup=" << warmup
              << ", checksum=" << checksum << "\n";
    return 0;
}
