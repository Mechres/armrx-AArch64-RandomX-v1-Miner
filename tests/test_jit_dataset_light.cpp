// Track C Phase A diagnostic test (master-plan-20260727.md):
// Compares dataset-item outputs from the light-mode reduced-register-preservation
// JIT prologue against the C++ reference implementation for many deterministic
// (seed, itemNumber) pairs. Any mismatch means a data-flow bug in the prologue.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include "armrx/argon2.hpp"
#include "armrx/dataset.hpp"
#include "armrx/jit_compiler_a64.hpp"
#include "armrx/jit_compiler_a64_static.hpp"

static int failures = 0;
static constexpr size_t kNumItems = 500;
static constexpr uint64_t kSeeds[] = {
    0xDEADBEEFCAFEBABEULL,
    0x0123456789ABCDEFULL,
    0xFEDCBA9876543210ULL,
    0x1111111111111111ULL,
    0x0000000000000000ULL
};

static void check(const char* label, bool condition) {
    if (!condition) {
        std::cerr << "FAIL: " << label << "\n";
        ++failures;
    }
}

int main() {
    std::cout << "Track C Phase A diagnostic: light prologue vs C++ reference\n"
              << "Testing " << kNumItems << " items across "
              << (sizeof(kSeeds) / sizeof(kSeeds[0])) << " seeds...\n";

    for (const auto seed_val : kSeeds) {
        // Build seed bytes
        std::vector<std::byte> key;
        key.reserve(8);
        for (int i = 0; i < 8; ++i)
            key.push_back(static_cast<std::byte>((seed_val >> (i * 8)) & 0xFF));

        // Create Argon2dCache (reference implementation's backing data)
        armrx::Argon2dCache cache;
        cache.initialize(key);

        // Create JIT compiler and generate superscalar hash (light prologue)
        armrx::JitCompilerA64 jit;
        jit.generateSuperscalarHash(cache.programs(), cache.reciprocal_cache());
        const auto fn = jit.getCalcDatasetItemFunc();
        const void* cache_ptr = reinterpret_cast<const void*>(cache.blocks().data());

        // Test items with various item numbers
        std::mt19937_64 rng(seed_val);
        for (size_t i = 0; i < kNumItems; ++i) {
            const uint64_t item_number = rng();

            // C++ reference
            const auto expected = armrx::generate_dataset_item(cache, item_number);

            // JIT (light prologue)
            std::array<std::byte, 64> actual{};
            fn(cache_ptr, actual.data(), item_number);

            // Compare
            bool ok = (actual == expected);
            check(("seed=0x" + std::to_string(seed_val) +
                   " item=" + std::to_string(item_number)).c_str(), ok);

            if (!ok) {
                std::cerr << "  Mismatch at item " << item_number << "\n  Expected: ";
                for (auto b : expected) std::cerr << std::hex << (int)b;
                std::cerr << "\n  Actual:   ";
                for (auto b : actual) std::cerr << std::hex << (int)b;
                std::cerr << std::dec << "\n";

                if (failures >= 5) {
                    std::cerr << "Too many failures, aborting early.\n";
                    goto done;
                }
            }
        }
    }

done:
    if (failures == 0) {
        std::cout << "ALL PASSED (" << kNumItems * (sizeof(kSeeds) / sizeof(kSeeds[0]))
                  << " total comparisons)\n";
    } else {
        std::cerr << failures << " FAILURES\n";
    }
    return failures;
}
