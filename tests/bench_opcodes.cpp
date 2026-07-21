// Copyright (c) 2026, armrx contributors
//
// Opcode frequency histogram benchmark.
// Runs RandomX programs from many random seeds and records opcode
// frequency distributions plus per-opcode emitted byte-count statistics.

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include "armrx/randomx_config.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <array>
#include <string>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <chrono>

namespace {

// Opcode names matching the frequency weight table order
constexpr const char* kOpcodeNames[] = {
    "IADD_RS",   "IADD_M",   "ISUB_R",   "ISUB_M",
    "IMUL_R",    "IMUL_M",   "IMULH_R",  "IMULH_M",
    "ISMULH_R",  "ISMULH_M", "IMUL_RCP", "INEG_R",
    "IXOR_R",    "IXOR_M",   "IROR_R",   "IROL_R",
    "ISWAP_R",   "FSWAP_R",  "FADD_R",   "FADD_M",
    "FSUB_R",    "FSUB_M",   "FSCAL_R",  "FMUL_R",
    "FDIV_M",    "FSQRT_R",  "CBRANCH",  "CFROUND",
    "ISTORE",    "NOP",
};
constexpr int kNumOpcodes = sizeof(kOpcodeNames) / sizeof(kOpcodeNames[0]);

// Opcode frequencies (from instruction_weights.hpp)
constexpr int kOpcodeWeights[kNumOpcodes] = {
    16, 7, 16, 7,
    16, 4, 4, 1,
    4, 1, 8, 2,
    15, 5, 8, 2,
    4, 4, 16, 5,
    16, 5, 6, 32,
    4, 6, 25, 1,
    16, 0,
};
constexpr int kTotalWeight = 256;

// Build raw-opcode-byte -> effective opcode index mapping (0-29)
// using the same weight-table expansion as the JIT engine[].
int g_opcode_to_idx[256];

void build_opcode_map() {
    static bool built = false;
    if (built) return;
    int slot = 0;
    for (int i = 0; i < kNumOpcodes; ++i) {
        for (int w = 0; w < kOpcodeWeights[i]; ++w) {
            if (slot < 256) g_opcode_to_idx[slot++] = i;
        }
    }
    while (slot < 256) g_opcode_to_idx[slot++] = 29; // NOP
    built = true;
}

} // anonymous namespace

int main(int argc, char** argv) {
    build_opcode_map();

    int num_seeds = 20;
    if (argc > 1) num_seeds = std::stoi(argv[1]);
    if (num_seeds < 1) num_seeds = 1;

    // Aggregate: total count per opcode, total bytes per opcode
    std::uint64_t total_counts[kNumOpcodes] = {};
    std::uint64_t total_bytes[kNumOpcodes] = {};
    int total_programs = 0;
    int total_instructions = 0;

    auto start_time = std::chrono::steady_clock::now();

    for (int seed_idx = 0; seed_idx < num_seeds; ++seed_idx) {
        // Build seed key
        std::string seed_str = "bench_seed_" + std::to_string(seed_idx);
        std::vector<std::byte> key_bytes;
        key_bytes.reserve(seed_str.size());
        for (char c : seed_str) key_bytes.push_back(static_cast<std::byte>(c));

        // Initialize cache
        armrx::Argon2dCache cache;
        cache.initialize(key_bytes);

        // Light-mode VM with JIT and hardware AES
        const uint32_t vm_flags = armrx::kRandOMXFlagJit | armrx::kRandOMXFlagHardAes;
        armrx::VirtualMachine vm(vm_flags);
        vm.setJitDumpEnabled();
        vm.set_cache(&cache);

        // Run a single hash — JIT dump is captured during generateProgram/generateProgramLight
        alignas(16) std::array<std::byte, 32> hash{};
        const char* input = "opcode benchmark input";
        armrx::randomx_calculate_hash(&vm, input, std::strlen(input), hash.data());

        // Collect per-opcode data from the JIT dump
        const auto& dump = vm.getJitDump();
        for (const auto& entry : dump) {
            int idx = g_opcode_to_idx[entry.opcode % 256];
            ++total_counts[idx];
            total_bytes[idx] += entry.size;
            ++total_instructions;
        }
        ++total_programs;
    }

    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_time).count();

    std::cout << "=== Opcode Frequency & Byte-Cost Benchmark ===\n";
    std::cout << "Seeds: " << num_seeds
              << " | Programs: " << total_programs
              << " | Instructions: " << total_instructions
              << " | Elapsed: " << elapsed << " s\n\n";

    // Print header
    std::cout << std::left << std::setw(12) << "Opcode"
              << std::right
              << std::setw(10) << "Count"
              << std::setw(8) << "%"
              << std::setw(12) << "Exp%"
              << std::setw(12) << "Bytes"
              << std::setw(10) << "AvgB"
              << "   Notes\n";
    std::cout << std::string(70, '-') << "\n";

    // Sort by frequency descending
    int order[kNumOpcodes];
    for (int i = 0; i < kNumOpcodes; ++i) order[i] = i;
    std::sort(order, order + kNumOpcodes, [&](int a, int b) {
        return total_counts[a] > total_counts[b];
    });

    std::uint64_t grand_total_bytes = 0;
    for (int i = 0; i < kNumOpcodes; ++i) grand_total_bytes += total_bytes[i];

    for (int oi = 0; oi < kNumOpcodes; ++oi) {
        int i = order[oi];
        if (total_counts[i] == 0 && kOpcodeWeights[i] == 0) continue;

        double actual_pct = 100.0 * total_counts[i] / std::max<uint64_t>(total_instructions, 1);
        double expected_pct = 100.0 * kOpcodeWeights[i] / kTotalWeight;
        double avg_bytes = static_cast<double>(total_bytes[i]) / std::max<uint64_t>(total_counts[i], 1);
        double byte_pct = 100.0 * total_bytes[i] / std::max<uint64_t>(grand_total_bytes, 1);

        std::cout << std::left << std::setw(12) << kOpcodeNames[i]
                  << std::right
                  << std::setw(10) << static_cast<unsigned long long>(total_counts[i])
                  << std::setw(8) << std::fixed << std::setprecision(2) << actual_pct
                  << std::setw(12) << std::fixed << std::setprecision(2) << expected_pct
                  << std::setw(12) << static_cast<unsigned long long>(total_bytes[i])
                  << std::setw(9) << std::fixed << std::setprecision(1) << avg_bytes;

        // Flag opcodes worth optimizing
        if (actual_pct > 5.0) std::cout << "   *** HIGH FREQ";
        else if (avg_bytes > 20.0 && actual_pct > 1.0) std::cout << "   ** FAT (avg " << avg_bytes << "B)";
        std::cout << "\n";
    }

    std::cout << "\n--- Totals ---\n";
    std::cout << "Average instructions per program: "
              << (total_instructions / std::max(total_programs, 1)) << "\n";
    std::cout << "Average JIT code bytes per program: "
              << (grand_total_bytes / std::max(total_programs, 1)) << "\n";

    return 0;
}
