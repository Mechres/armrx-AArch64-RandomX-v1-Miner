// Track D1 (docs/plans/track-d1-superscalar-interleave-plan-20260728.md):
// exhaustive differential test for the new 2-way interleaved dataset-item
// derivation entry point (randomx_calc_dataset_item_aarch64_2way /
// JitDataset2Way).
//
// The derivation is a pure function of (cache, item number), so the
// oracle is total: generate_dataset_item() is the same reference
// implementation test_partial_dataset.cpp and test_blake2b.cpp already
// use to validate every other dataset-item derivation path in this
// project (fast-mode NEON, light-mode JIT, the partial-dataset cache).
// A single mismatch here means the 2-way entry point is unsafe.
//
// Seed diversity matters more than per-seed pair count for this code
// region (see test_jit_superscalar_scheduler_stress.cpp's header comment
// for why: generateSuperscalarHash()-class code compiles once per seed
// and is reused for every item derived against it), so this test spans
// many distinct seeds and, within each, thousands of (itemA, itemB)
// pairs -- item numbers drawn from the full production range, not just
// small values, so the AND-mask/modulo-by-power-of-two cache-line
// indexing is exercised across its whole domain.

#include "armrx/argon2.hpp"
#include "armrx/dataset.hpp"
#include "armrx/jit_compiler_a64.hpp"
#include "armrx/jit_dataset_2way.hpp"

#include <array>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

int main() {
    constexpr int kNumSeeds = 20;
    constexpr int kPairsPerSeed = 2000;
    long long total = 0;

    // Reused purely to reach the verified superscalar scheduler via the
    // new read-only passthrough (computeSuperscalarEmitOrder) -- this
    // instance's own JIT buffer/program generation is never touched.
    armrx::JitCompilerA64 scheduler_source;
    auto scheduler = [&scheduler_source](const armrx::SuperscalarProgram& prog) {
        return scheduler_source.computeSuperscalarEmitOrder(prog);
    };

    armrx::JitDataset2Way jit2way;

    std::mt19937_64 rng(0xD1D1D1D1ULL);
    std::uniform_int_distribution<std::uint64_t> item_dist(0, (1ULL << 40) - 1);

    for (int si = 0; si < kNumSeeds; ++si) {
        const std::string seed = "track_d1_2way_diff_seed_" + std::to_string(si);
        std::vector<std::byte> key;
        key.reserve(seed.size());
        for (char c : seed) key.push_back(static_cast<std::byte>(c));

        armrx::Argon2dCache cache;
        cache.initialize(key);

        jit2way.generate(cache.programs(), cache.reciprocal_cache(), scheduler);
        const armrx::Dataset2WayFunc fn = jit2way.getFunc();
        const void* cache_ptr = reinterpret_cast<const void*>(cache.blocks().data());

        for (int pi = 0; pi < kPairsPerSeed; ++pi) {
            const std::uint64_t itemA = item_dist(rng);
            const std::uint64_t itemB = item_dist(rng);

            std::array<std::byte, 64> outA{};
            std::array<std::byte, 64> outB{};
            fn(cache_ptr, outA.data(), itemA, outB.data(), itemB);

            const auto expectedA = armrx::generate_dataset_item(cache, itemA);
            const auto expectedB = armrx::generate_dataset_item(cache, itemB);

            ++total;
            if (outA != expectedA) {
                std::cerr << "FAIL: stream A mismatch, seed=\"" << seed << "\" itemA=" << itemA << " itemB=" << itemB
                          << "\n";
                return 1;
            }
            if (outB != expectedB) {
                std::cerr << "FAIL: stream B mismatch, seed=\"" << seed << "\" itemA=" << itemA << " itemB=" << itemB
                          << "\n";
                return 1;
            }
        }

        // Also cover itemA == itemB explicitly (both streams computing the
        // identical item from the identical cache) -- not a special case
        // mechanically, but worth pinning down since it's the one input
        // shape where both streams' outputs must also match each other.
        {
            const std::uint64_t item = item_dist(rng);
            std::array<std::byte, 64> outA{};
            std::array<std::byte, 64> outB{};
            fn(cache_ptr, outA.data(), item, outB.data(), item);
            const auto expected = armrx::generate_dataset_item(cache, item);
            ++total;
            if (outA != expected || outB != expected || outA != outB) {
                std::cerr << "FAIL: itemA==itemB mismatch, seed=\"" << seed << "\" item=" << item << "\n";
                return 1;
            }
        }
    }

    std::cout << "JIT 2-way dataset-item derivation: " << total << " item derivations across " << kNumSeeds
              << " distinct seeds, all byte-identical to generate_dataset_item()\n";
    return 0;
}
