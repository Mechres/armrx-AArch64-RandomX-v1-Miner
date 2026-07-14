/**
 * Micro-benchmarks for armrx pipeline components.
 * Run on the AArch64 device to find where we're losing time vs XMRig.
 *
 * Build: cmake --build build -j && ./build/bench_armrx
 */

#include "armrx/argon2.hpp"
#include "armrx/superscalar.hpp"
#include "armrx/dataset.hpp"
#include "armrx/vm.hpp"
#include "armrx/aes_hash.hpp"
#include "armrx/blake2b.hpp"
#include "armrx/randomx_config.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <span>

namespace {

using bench_clock = std::chrono::steady_clock;

/** Run `fn` N times, report average duration in μs and throughput. */
template<typename F>
void benchmark(const char* name, unsigned iterations, F&& fn,
               const char* unit = "ops", double scale = 1.0) {
    // Warmup
    for (unsigned i = 0; i < 3; ++i) fn();

    auto start = bench_clock::now();
    for (unsigned i = 0; i < iterations; ++i) fn();
    auto end = bench_clock::now();

    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double per_op_us = static_cast<double>(total_ns) / iterations / 1000.0;
    double ops_per_sec = static_cast<double>(iterations) * 1e9 / total_ns * scale;

    std::cout << std::left << std::setw(40) << name
              << std::right << std::fixed << std::setprecision(2)
              << std::setw(10) << per_op_us << " μs/op  "
              << std::setw(10) << ops_per_sec << " " << unit << "/s\n";
}

} // namespace

int main() {
    std::cout << "\n=== armrx Micro-Benchmarks ===\n\n";

    // ── 1. Blake2b throughput ──────────────────────────────────────────
    std::array<std::byte, 64> blake_input{};
    std::array<std::byte, 64> blake_output{};
    benchmark("blake2b (64-in, 64-out)", 10000, [&] {
        auto res = armrx::blake2b(std::span<const std::byte>(blake_input), 64);
        std::memcpy(blake_output.data(), res.data(), 64);
    });

    // ── 2. AES round primitives ───────────────────────────────────────
    alignas(16) std::array<std::byte, 64> aes_buf{};
    armrx::AesState aes_state{};
    benchmark("fill_aes_1r_x4 (2 MiB)", 50, [&] {
        armrx::fill_aes_1r_x4(aes_state,
            std::span<std::byte>(reinterpret_cast<std::byte*>(aes_buf.data()), 64));
    }, "pages"); // 64 bytes per call, not a full 2 MiB

    // ── 3. Cache initialization ───────────────────────────────────────
    std::vector<std::byte> seed_key = {std::byte{0x00}, std::byte{0x11}};
    // Initialize once
    armrx::Argon2dCache cache;
    cache.initialize(seed_key);

    // ── 4. Cache line read ────────────────────────────────────────────
    benchmark("load_cache_line (random access)", 100000, [&] {
        volatile auto item = armrx::load_cache_line(cache, 42);
        (void)item;
    }, "lines");

    // ── 5. SuperscalarHash (dataset item derivation) ──────────────────
    benchmark("generate_dataset_item", 5000, [&] {
        volatile auto item = armrx::generate_dataset_item(cache, 1000000);
        (void)item;
    }, "items");

    std::vector<std::byte> dataset_buf(5000 * armrx::kRandomXDatasetItemBytes);
    benchmark("initialize_dataset (5000 items)", 10, [&] {
        armrx::initialize_dataset(dataset_buf, cache, 0, 5000);
    }, "items", 5000.0);

    // ── 6. JIT compilation only (warm VM with JIT) ────────────────────
    std::uint32_t jit_flags = armrx::kRandOMXFlagHardAes | armrx::kRandOMXFlagJit;
    armrx::VirtualMachine vm(jit_flags);
    vm.set_cache(&cache);

    // JIT compilation benchmark: generate and compile one program
    // The VM needs entropy to generate a program — run one full hash first
    std::vector<std::byte> test_input = {std::byte{0x00}};
    alignas(16) std::array<std::byte, 32> vm_out{};
    armrx::randomx_calculate_hash(&vm, test_input.data(), test_input.size(), vm_out.data());

    // ── 7. Full hash throughput (light mode, JIT) ─────────────────────
    // Measure how many hashes/sec in the current configuration
    std::array<std::byte, 76> block_template{};
    for (size_t i = 0; i < block_template.size(); ++i) block_template[i] = static_cast<std::byte>(i & 0xff);
    unsigned hash_count = 200;

    auto hash_start = bench_clock::now();
    for (unsigned i = 0; i < hash_count; ++i) {
        block_template[39] = static_cast<std::byte>(i); // vary nonce byte
        armrx::randomx_calculate_hash(&vm, block_template.data(),
                                       block_template.size(), vm_out.data());
    }
    auto hash_end = bench_clock::now();
    auto hash_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(hash_end - hash_start).count();
    double hash_us = static_cast<double>(hash_ns) / hash_count / 1000.0;
    double hash_ps = static_cast<double>(hash_count) * 1e9 / hash_ns;

    std::cout << std::left << std::setw(40) << "full hash (light, JIT)"
              << std::right << std::fixed << std::setprecision(2)
              << std::setw(10) << hash_us << " μs/hash  "
              << std::setw(10) << hash_ps << " hashes/s" << std::endl;

    // ── 8. Interpreted mode comparison ────────────────────────────────
    std::uint32_t interp_flags = 0; // No JIT, no HardAes
    armrx::VirtualMachine vm_interp(interp_flags);
    vm_interp.set_cache(&cache);

    // Warmup
    armrx::randomx_calculate_hash(&vm_interp, test_input.data(), test_input.size(), vm_out.data());

    unsigned interp_hash_count = 10;
    auto interp_start = bench_clock::now();
    for (unsigned i = 0; i < interp_hash_count; ++i) {
        block_template[39] = static_cast<std::byte>(i + 100);
        armrx::randomx_calculate_hash(&vm_interp, block_template.data(),
                                        block_template.size(), vm_out.data());
    }
    auto interp_end = bench_clock::now();
    auto interp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(interp_end - interp_start).count();
    double interp_us = static_cast<double>(interp_ns) / interp_hash_count / 1000.0;
    double interp_ps = static_cast<double>(interp_hash_count) * 1e9 / interp_ns;

    std::cout << std::left << std::setw(40) << "full hash (light, interpreted)"
              << std::right << std::fixed << std::setprecision(2)
              << std::setw(10) << interp_us << " μs/hash  "
              << std::setw(10) << interp_ps << " hashes/s" << std::endl;

    // ── 9. Summary comparison vs XMRig ────────────────────────────────
    std::cout << "\n=== Summary ===\n";
    std::cout << "XMRig light mode target:  ~27 hashes/s (37 ms/hash)\n";
    std::cout << "Our light JIT mode:        " << hash_ps << " hashes/s (" << hash_us / 1000.0 << " ms/hash)\n";
    std::cout << "Our light interpreted:     " << interp_ps << " hashes/s\n";
    std::cout << "Gap to XMRig:             " << (27.0 / hash_ps - 1.0) * 100.0 << "%\n\n";

    return 0;
}
