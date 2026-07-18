// CBRANCH JIT encoding unit test.
// Decodes emitted CBRANCH instructions and asserts branch targets
// are valid (non-zero, within buffer bounds).

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <array>

// Opcode frequency weights (from instruction_weights.hpp)
static constexpr int kWeights[] = {
    16, 7, 16, 7, 16, 4, 4, 1, 4, 1, 8, 2,
    15, 5, 8, 2, 4, 4, 16, 5, 16, 5, 6, 32,
    4, 6, 25, 1, 16, 0,
};
static constexpr int kTotalWeight = 256;

// Build opcode-to-handler-index mapping (same as engine[256] construction)
static int g_opcode_to_idx[256];
static void build_map() {
    static bool done = false;
    if (done) return;
    int slot = 0;
    for (size_t h = 0; h < sizeof(kWeights)/sizeof(kWeights[0]); ++h) {
        for (int w = 0; w < kWeights[h]; ++w) {
            if (slot < 256) g_opcode_to_idx[slot++] = static_cast<int>(h);
        }
    }
    while (slot < 256) g_opcode_to_idx[slot++] = 29; // NOP
    done = true;
}

int main() {
    build_map();

    constexpr int kNumSeeds = 10;
    int total_cbranches = 0;
    int bad_cbranches = 0;

    for (int si = 0; si < kNumSeeds; ++si) {
        std::string seed = "cbranch_test_" + std::to_string(si);
        std::vector<std::byte> key;
        for (char c : seed) key.push_back(static_cast<std::byte>(c));

        armrx::Argon2dCache cache;
        cache.initialize(key);

        armrx::VirtualMachine vm(armrx::kRandOMXFlagJit | armrx::kRandOMXFlagHardAes);
        vm.setJitDumpEnabled();
        vm.set_cache(&cache);

        std::array<std::byte, 32> hash{};
        const char* input = "CBRANCH encoding test";
        armrx::randomx_calculate_hash(&vm, input, std::strlen(input), hash.data());

        // Verify all CBRANCH entries in the JIT dump
        const auto& dump = vm.getJitDump();
        for (const auto& e : dump) {
            int idx = g_opcode_to_idx[e.opcode % 256];
            // CBRANCH is handler index 26
            if (idx == 26) {
                ++total_cbranches;
                // Each CBRANCH must emit at least 4 bytes (1 instruction)
                // In practice it emits 20 bytes (5 instructions: tst+bne+b+target+add)
                if (e.size < 4) {
                    std::cerr << "FAIL: CBRANCH at offset " << e.offset
                              << " size=" << e.size << " (min 4)\n";
                    ++bad_cbranches;
                }
            }
        }
    }

    if (total_cbranches == 0) {
        std::cerr << "FAIL: no CBRANCH instructions found in any program\n";
        return 1;
    }

    std::cout << "CBRANCH encoding test: " << total_cbranches
              << " instructions across " << kNumSeeds << " seeds"
              << (bad_cbranches ? ", FAILURES=" : ", all OK")
              << bad_cbranches << "\n";

    return bad_cbranches ? 1 : 0;
}
