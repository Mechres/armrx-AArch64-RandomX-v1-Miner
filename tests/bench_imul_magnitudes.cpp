// Copyright (c) 2026, armrx contributors
//
// IMUL_R operand-magnitude sampler (T3-3 gate check).
//
// Hooks the interpreter's IMUL_R bytecode case (VirtualMachine::
// setImulSampleCallback) and records the actual runtime src/dst 64-bit operand
// values, distinguishing genuine IMUL_R from IMUL_RCP-lowered instructions
// (ibc.isrc == &ibc.imm carries a full-width reciprocal constant, useless for
// lane packing — F3 ruled RCP out). The gate criterion
// (docs/audits/combined-audit-20260731.md T3-3) is: >= 30% of executed genuine
// IMUL_R instances have at least one operand <= 2^32. Interpreter-only — no
// JIT dependency, runs on any host (the interpreter is byte-identical to the
// JIT per test_jit_equivalence; the result is a property of RandomX's runtime
// data distribution, not of hardware).
//
// Usage: bench_imul_magnitudes [num_seeds] [hashes_per_seed]
//   defaults 40 8 (target runtime <= ~60 s on host; 40x40 measures ~200 s on
//   a 256 MiB Argon2d-cache-per-seed budget, so hashes/seed defaults to 8).

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

// --- Global accumulators, fed by the sampling callback ---
std::uint64_t g_imul_total = 0; // genuine IMUL_R executions (is_rcp == false)
std::uint64_t g_rcp_total = 0;  // IMUL_RCP-lowered executions (is_rcp == true)

// Unsigned magnitude tiers (genuine IMUL_R only): one = src OR dst meets it,
// both = src AND dst meet it.
std::uint64_t g_one_u32 = 0;
std::uint64_t g_both_u32 = 0;
std::uint64_t g_one_u16 = 0;
std::uint64_t g_both_u16 = 0;
std::uint64_t g_one_u8 = 0;
std::uint64_t g_both_u8 = 0;

// Signed magnitude tiers.
std::uint64_t g_one_s32 = 0;
std::uint64_t g_both_s32 = 0;
std::uint64_t g_one_s16 = 0;
std::uint64_t g_both_s16 = 0;

void imul_sample_cb(std::uint64_t src, std::uint64_t dst, bool is_rcp) {
    if (is_rcp) {
        ++g_rcp_total;
        return;
    }
    ++g_imul_total;

    // Unsigned tiers
    const bool src_u32 = (src >> 32) == 0;
    const bool dst_u32 = (dst >> 32) == 0;
    if (src_u32 || dst_u32) ++g_one_u32;
    if (src_u32 && dst_u32) ++g_both_u32;

    const bool src_u16 = (src >> 16) == 0;
    const bool dst_u16 = (dst >> 16) == 0;
    if (src_u16 || dst_u16) ++g_one_u16;
    if (src_u16 && dst_u16) ++g_both_u16;

    const bool src_u8 = (src >> 8) == 0;
    const bool dst_u8 = (dst >> 8) == 0;
    if (src_u8 || dst_u8) ++g_one_u8;
    if (src_u8 && dst_u8) ++g_both_u8;

    // Signed magnitude tiers (std::llabs; INT64_MIN guarded — llabs(INT64_MIN)
    // is UB, and |INT64_MIN| = 2^63 exceeds every tier anyway).
    const std::int64_t ssrc = static_cast<std::int64_t>(src);
    const std::int64_t sdst = static_cast<std::int64_t>(dst);
    const bool src_s32 = ssrc != INT64_MIN && std::llabs(ssrc) <= 0x7FFFFFFF;
    const bool dst_s32 = sdst != INT64_MIN && std::llabs(sdst) <= 0x7FFFFFFF;
    if (src_s32 || dst_s32) ++g_one_s32;
    if (src_s32 && dst_s32) ++g_both_s32;

    const bool src_s16 = ssrc != INT64_MIN && std::llabs(ssrc) <= 0x7FFF;
    const bool dst_s16 = sdst != INT64_MIN && std::llabs(sdst) <= 0x7FFF;
    if (src_s16 || dst_s16) ++g_one_s16;
    if (src_s16 && dst_s16) ++g_both_s16;
}

struct PerSeed {
    std::uint64_t imul_total;
    std::uint64_t one_u32;
};

std::vector<std::byte> make_seed_key(int seed_idx) {
    std::string seed_str = "t33_seed_" + std::to_string(seed_idx);
    std::vector<std::byte> key_bytes;
    key_bytes.reserve(seed_str.size());
    for (char c : seed_str) key_bytes.push_back(static_cast<std::byte>(c));
    return key_bytes;
}

// Deterministic 32-byte input: bytes 0-7 = LE (uint64)s, bytes 8-15 = LE
// (uint64)h, bytes 16-31 = 0.
std::array<std::byte, 32> make_input(std::uint64_t s, std::uint64_t h) {
    std::array<std::byte, 32> input{};
    std::memcpy(input.data(), &s, sizeof(s));
    std::memcpy(input.data() + sizeof(s), &h, sizeof(h));
    return input;
}

// Self-check (must run first): seed 0, hash 0, once with the hook disabled and
// once enabled — the two 32-byte outputs must be identical, proving the hook
// is observation-only. Also verifies the hook actually fired.
bool run_self_check() {
    armrx::Argon2dCache cache;
    cache.initialize(make_seed_key(0));

    armrx::VirtualMachine vm_plain(0);
    vm_plain.set_cache(&cache);
    armrx::VirtualMachine vm_sampled(0);
    vm_sampled.set_cache(&cache);
    vm_sampled.setImulSampleCallback(&imul_sample_cb);

    const auto input = make_input(0, 0);
    alignas(16) std::array<std::byte, 32> out_plain{};
    alignas(16) std::array<std::byte, 32> out_sampled{};
    armrx::randomx_calculate_hash(&vm_plain, input.data(), input.size(), out_plain.data());
    const std::uint64_t imul_before = g_imul_total;
    armrx::randomx_calculate_hash(&vm_sampled, input.data(), input.size(), out_sampled.data());

    if (out_plain != out_sampled) {
        std::cerr << "SELF-CHECK FAILED: hook disabled vs enabled produced different hashes\n";
        return false;
    }
    if (g_imul_total == imul_before) {
        std::cerr << "SELF-CHECK FAILED: sampling hook never fired (imul_total unchanged)\n";
        return false;
    }
    return true;
}

void reset_accumulators() {
    g_imul_total = 0;
    g_rcp_total = 0;
    g_one_u32 = 0;
    g_both_u32 = 0;
    g_one_u16 = 0;
    g_both_u16 = 0;
    g_one_u8 = 0;
    g_both_u8 = 0;
    g_one_s32 = 0;
    g_both_s32 = 0;
    g_one_s16 = 0;
    g_both_s16 = 0;
}

} // anonymous namespace

int main(int argc, char** argv) {
    int num_seeds = 40;
    int hashes_per_seed = 8;
    if (argc > 1) num_seeds = std::stoi(argv[1]);
    if (argc > 2) hashes_per_seed = std::stoi(argv[2]);
    if (num_seeds < 1) num_seeds = 1;
    if (hashes_per_seed < 1) hashes_per_seed = 1;

    auto start_time = std::chrono::steady_clock::now();

    if (!run_self_check()) return 1;
    reset_accumulators();

    std::vector<PerSeed> per_seed;
    per_seed.reserve(static_cast<std::size_t>(num_seeds));

    for (int s = 0; s < num_seeds; ++s) {
        armrx::Argon2dCache cache;
        cache.initialize(make_seed_key(s));

        armrx::VirtualMachine vm(0);
        vm.set_cache(&cache);
        vm.setImulSampleCallback(&imul_sample_cb);

        const std::uint64_t seed_imul_before = g_imul_total;
        const std::uint64_t seed_one_u32_before = g_one_u32;

        for (int h = 0; h < hashes_per_seed; ++h) {
            const auto input = make_input(static_cast<std::uint64_t>(s), static_cast<std::uint64_t>(h));
            alignas(16) std::array<std::byte, 32> out{};
            armrx::randomx_calculate_hash(&vm, input.data(), input.size(), out.data());
        }

        per_seed.push_back({g_imul_total - seed_imul_before, g_one_u32 - seed_one_u32_before});
    }

    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_time).count();

    // --- Header ---
    std::cout << "=== IMUL_R Operand-Magnitude Sampler (T3-3 gate check) ===\n";
    std::cout << "Seeds: " << num_seeds << " | hashes/seed: " << hashes_per_seed
              << " | total hashes: " << static_cast<std::uint64_t>(num_seeds) * hashes_per_seed
              << " | elapsed: " << elapsed << " s\n";
    std::cout << "Self-check (seed 0, hash 0, hook disabled vs enabled): PASS\n\n";

    // --- Totals table ---
    const std::uint64_t denom = std::max<std::uint64_t>(g_imul_total, 1);
    std::cout << "--- Totals ---\n";
    std::cout << "Genuine IMUL_R executions: " << g_imul_total << "\n";
    std::cout << "IMUL_RCP-lowered executions: " << g_rcp_total << "\n\n";

    struct TierRow {
        const char* name;
        std::uint64_t one;
        std::uint64_t both;
    };
    const TierRow tiers[] = {
        {"v <= 0xFFFFFFFF (unsigned 32)", g_one_u32, g_both_u32},
        {"v <= 0xFFFF (unsigned 16)", g_one_u16, g_both_u16},
        {"v <= 0xFF (unsigned 8)", g_one_u8, g_both_u8},
        {"|v| <= 0x7FFFFFFF (signed 31)", g_one_s32, g_both_s32},
        {"|v| <= 0x7FFF (signed 15)", g_one_s16, g_both_s16},
    };

    std::cout << "Tier (genuine IMUL_R only)                 one count    one %   both count    both %\n";
    std::cout << std::string(79, '-') << "\n";
    for (const auto& t : tiers) {
        std::cout << std::left << std::setw(39) << t.name
                  << std::right
                  << std::setw(13) << static_cast<unsigned long long>(t.one)
                  << std::setw(9) << std::fixed << std::setprecision(2) << (100.0 * t.one / denom)
                  << std::setw(13) << static_cast<unsigned long long>(t.both)
                  << std::setw(9) << std::fixed << std::setprecision(2) << (100.0 * t.both / denom)
                  << "\n";
    }

    // --- Per-seed variance (one-operand <= 2^32) ---
    std::cout << "\n--- Per-seed variance (one-operand <= 2^32) ---\n";
    std::cout << std::left << std::setw(6) << "seed"
              << std::right
              << std::setw(15) << "imul_total"
              << std::setw(14) << "one<=2^32 %"
              << "\n";
    double min_frac = 101.0;
    double max_frac = -1.0;
    double sum_frac = 0.0;
    for (int i = 0; i < num_seeds; ++i) {
        const double frac = 100.0 * per_seed[i].one_u32 /
                            std::max<std::uint64_t>(per_seed[i].imul_total, 1);
        min_frac = std::min(min_frac, frac);
        max_frac = std::max(max_frac, frac);
        sum_frac += frac;
    }
    const int shown = std::min(num_seeds, 40);
    for (int i = 0; i < shown; ++i) {
        const double frac = 100.0 * per_seed[i].one_u32 /
                            std::max<std::uint64_t>(per_seed[i].imul_total, 1);
        std::cout << std::left << std::setw(6) << i
                  << std::right
                  << std::setw(15) << static_cast<unsigned long long>(per_seed[i].imul_total)
                  << std::setw(14) << std::fixed << std::setprecision(2) << frac
                  << "\n";
    }
    if (num_seeds > 40) std::cout << "...\n";
    std::cout << "min: " << std::fixed << std::setprecision(2) << min_frac
              << "% | max: " << std::fixed << std::setprecision(2) << max_frac
              << "% | mean: " << std::fixed << std::setprecision(2) << (sum_frac / num_seeds)
              << "%\n";

    // --- Gate verdict ---
    const double one_u32_pct = 100.0 * g_one_u32 / denom;
    std::cout << "\n" << (one_u32_pct >= 30.0 ? "GATE: PASS" : "GATE: FAIL")
              << " (" << std::fixed << std::setprecision(2) << one_u32_pct
              << "% of " << g_imul_total << " IMUL_R have one operand <= 2^32)\n";
    std::cout << "BOTH-OPERANDS <= 2^32: " << std::fixed << std::setprecision(2)
              << (100.0 * g_both_u32 / denom) << "%\n";

    return 0;
}
