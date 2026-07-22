// JIT/interpreter equivalence sweep.
//
// armrx_tests only KAT-checks 2 fixed inputs against both the interpreter
// and JIT paths — a CBRANCH (or any JIT-encoding) bug tied to specific
// immediate/displacement values could easily miss 2 fixed programs. This
// sweeps many different seeds x inputs, each producing a genuinely
// different RandomX program, and asserts the JIT and interpreter produce
// byte-identical hashes for every one of them. Intended as a regression
// gate for any future JIT hot-path change (e.g. CBRANCH), not just CBRANCH
// itself — a broad correctness net is cheaper to run once than to rebuild
// per change.

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

int main() {
    // Argon2dCache::initialize() dominates runtime (light-mode cache init is
    // expensive) — kept small enough to stay under ~2 min per run so this is
    // practical to re-run repeatedly during JIT hot-path development, while
    // still covering far more programs than the 2 fixed KAT inputs elsewhere.
    constexpr int kNumSeeds = 8;
    constexpr int kInputsPerSeed = 2;
    int total = 0;

    for (int si = 0; si < kNumSeeds; ++si) {
        const std::string seed = "jit_equiv_seed_" + std::to_string(si);
        std::vector<std::byte> key;
        key.reserve(seed.size());
        for (char c : seed) key.push_back(static_cast<std::byte>(c));

        armrx::Argon2dCache cache;
        cache.initialize(key);

        for (int ii = 0; ii < kInputsPerSeed; ++ii) {
            const std::string input = "equivalence input " + std::to_string(si) + "_" + std::to_string(ii);

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
                std::cerr << "FAIL: JIT/interpreter mismatch for seed=\"" << seed
                          << "\" input=\"" << input << "\"\n";
                return 1;
            }
        }
    }

    std::cout << "JIT/interpreter equivalence: " << total
              << " (seed, input) pairs checked, all byte-identical\n";
    return 0;
}
