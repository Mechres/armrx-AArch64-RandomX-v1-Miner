// Seed-diversity JIT/interpreter differential stress test for the
// superscalar/dataset-derivation scheduler (PLAN.md Phase 6 item 12
// extension, 2026-07-25).
//
// test_jit_scheduler_stress.cpp (many inputs, few seeds) is the right
// shape for the main-program scheduler, which recompiles fresh for every
// hash. It is the WRONG shape for this scheduler: generateSuperscalarHash()
// compiles once per seed rotation and that compiled code is reused for
// every hash computed against that seed's cache, so many inputs against
// few seeds mostly re-executes the same already-tested compiled schedule
// over and over. What actually stresses scheduleSuperscalarProgram()'s
// hazard analysis (register RAW/WAR/WAW across a flat 8-register file, and
// the IMUL_RCP literal-pool-ordering exclusion) is many DISTINCT
// SuperscalarProgram shapes -- i.e. many distinct seeds. Each seed
// produces a SuperscalarProgramList of 8 chained programs, so even a
// single hash per seed exercises 8 independently-scheduled programs.
//
// A single mismatch here means the superscalar scheduler is unsafe and
// must not ship.

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <array>
#include <iostream>
#include <string>
#include <vector>

int main() {
    constexpr int kNumSeeds = 100;
    constexpr int kInputsPerSeed = 2;
    int total = 0;

    for (int si = 0; si < kNumSeeds; ++si) {
        const std::string seed = "superscalar_sched_stress_seed_" + std::to_string(si);
        std::vector<std::byte> key;
        key.reserve(seed.size());
        for (char c : seed) key.push_back(static_cast<std::byte>(c));

        armrx::Argon2dCache cache;
        cache.initialize(key);

        for (int ii = 0; ii < kInputsPerSeed; ++ii) {
            const std::string input = "superscalar stress input " + std::to_string(si) + "_" + std::to_string(ii);

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
                std::cerr << "FAIL: superscalar-scheduler-induced JIT/interpreter mismatch for seed=\"" << seed
                          << "\" input=\"" << input << "\"\n";
                return 1;
            }
        }
    }

    std::cout << "JIT superscalar scheduler stress: " << total
              << " (seed, input) pairs across " << kNumSeeds << " distinct superscalar program sets, all byte-identical\n";
    return 0;
}
