// Large-N JIT/interpreter differential stress test for the emitter
// lookahead scheduler (PLAN.md Phase 6 item 12/L1, 2026-07-25).
//
// The existing test_jit_equivalence.cpp checks 16 (seed, input) pairs --
// good coverage for general JIT bugs, but the scheduler's specific failure
// mode is different: a silent, byte-level divergence between JIT and
// interpreter output that only manifests for RandomX programs with a
// particular shape (a long-latency multiply followed by a specific
// dependency pattern, near a CBRANCH whose target register happens to be
// written inside the swapped window). That shape is not something 16 fixed
// inputs have any particular reason to hit, and a scheduler bug here would
// not crash or assert -- it would just produce a wrong hash that still
// looks like a plausible 32-byte value. This test trades a small number of
// (expensive) Argon2 cache initializations for a large number of (cheap)
// distinct hash inputs against each -- every input produces a genuinely
// different RandomX program via the standard blake2b-seeded generation
// pipeline, so this samples many more program shapes than initializing many
// separate caches would, for a fraction of the runtime cost.
//
// A single mismatch here means the scheduler is unsafe and must not ship.

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

int main() {
    constexpr int kNumSeeds = 3;
    constexpr int kInputsPerSeed = 150;
    int total = 0;

    for (int si = 0; si < kNumSeeds; ++si) {
        const std::string seed = "jit_scheduler_stress_seed_" + std::to_string(si);
        std::vector<std::byte> key;
        key.reserve(seed.size());
        for (char c : seed) key.push_back(static_cast<std::byte>(c));

        armrx::Argon2dCache cache;
        cache.initialize(key);

        for (int ii = 0; ii < kInputsPerSeed; ++ii) {
            const std::string input = "scheduler stress input " + std::to_string(si) + "_" + std::to_string(ii);

            armrx::VirtualMachine vm_jit(armrx::kRandOMXFlagJit | armrx::kRandOMXFlagHardAes);
            vm_jit.set_cache(&cache);
            std::array<std::byte, 32> hash_jit{};
            armrx::randomx_calculate_hash(&vm_jit, input.data(), input.size(), hash_jit.data());

            armrx::VirtualMachine vm_interp(armrx::kRandOMXFlagDefault);
            vm_interp.set_cache(&cache);
            std::array<std::byte, 32> hash_interp{};
            armrx::randomx_calculate_hash(&vm_interp, input.data(), input.size(), hash_interp.data());

            ++total;
            if (hash_jit != hash_interp) {
                std::cerr << "FAIL: scheduler-induced JIT/interpreter mismatch for seed=\"" << seed
                          << "\" input=\"" << input << "\"\n";
                return 1;
            }
        }
    }

    std::cout << "JIT scheduler stress: " << total
              << " (seed, input) pairs checked, all byte-identical\n";
    return 0;
}
