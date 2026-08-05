// Copyright (c) 2026, armrx contributors
//
// Dual-hash premise microbench (experiment tool — NOT shipped in the miner).
//
// Premise under test: on the in-order Cortex-A53 the integer-multiply (MAC) port
// has 4-cycle result latency but is only used ~17% of the time during a single
// RandomX hash (estimated from instructions/hash vs cycles/hash). If that estimate
// is right, folding a SECOND independent hash's multiply stream into the same issue
// window should (a) raise hashes/second per thread and (b) LOWER other_interlock_stall
// per hash (the idle MAC cycles get filled by the other stream's independent muls).
//
// This harness does NOT implement dual-hash in the JIT. It measures the premise at
// the VirtualMachine level: two independent VMs (independent seed/cache/scratchpad)
// running in ONE thread, either serially (single) or alternated (interleaved). The
// real gate is on-device, wrapped in:
//
//   perf stat -e cycles:u,instructions:u,other_interlock_stall:u \
//       -- ./bench_dualhash_premise single   <iters> <seedA> <seedB>
//   perf stat -e cycles:u,instructions:u,other_interlock_stall:u \
//       -- ./bench_dualhash_premise interleaved <iters> <seedA> <seedB>
//
// Decision rule:
//   - interleaved H/s-per-thread > single AND other_interlock_stall/hash lower
//     -> MAC port has spare throughput; dual-hash is WORTH implementing in the JIT.
//   - interleaved H/s <= single (more instr, no throughput gain) -> port-bound,
//     no slack; dual-hash is DEAD as a lever.
//
// Usage: bench_dualhash_premise <single|interleaved> [iters=2000] [seedA=0x00] [seedB=0x11]

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
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

using Clock = std::chrono::steady_clock;

std::vector<std::byte> parse_seed(const char* s) {
    std::vector<std::byte> out;
    const std::string str = s;
    // Accept hex (0x...) or raw bytes; keep it simple: use the raw string bytes
    // as the seed key (Argon2 key can be any length).
    if (str.rfind("0x", 0) == 0) {
        for (size_t i = 2; i + 1 < str.size(); i += 2) {
            unsigned v = 0;
            std::sscanf(str.substr(i, 2).c_str(), "%02x", &v);
            out.push_back(static_cast<std::byte>(v));
        }
    } else {
        for (char c : str) out.push_back(static_cast<std::byte>(c));
    }
    if (out.empty()) out = {std::byte{0x00}};
    return out;
}

struct Hw {
    armrx::Argon2dCache cache;
    armrx::VirtualMachine vm;
    std::array<std::byte, 76> block;
    alignas(16) std::array<std::byte, 32> hash_out{};
    Hw(const std::vector<std::byte>& seed)
        : vm(armrx::kRandOMXFlagHardAes | armrx::kRandOMXFlagJit) {
        cache.initialize(seed);
        vm.set_cache(&cache);
        for (size_t i = 0; i < block.size(); ++i)
            block[i] = static_cast<std::byte>(static_cast<unsigned>(i) & 0xff);
        // Prime so the JIT program is compiled before timing.
        armrx::randomx_calculate_hash(&vm, block.data(), block.size(), hash_out.data());
    }
    void hash_once(std::uint64_t nonce) {
        // Vary the leading bytes of the block by nonce so the two streams differ.
        std::memcpy(block.data(), &nonce, sizeof(nonce));
        armrx::randomx_calculate_hash(&vm, block.data(), block.size(), hash_out.data());
    }
};

double run_single(Hw& a, unsigned iters) {
    auto t0 = Clock::now();
    for (unsigned i = 0; i < iters; ++i) a.hash_once(static_cast<std::uint64_t>(i) * 2 + 1);
    auto t1 = Clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

double run_interleaved(Hw& a, Hw& b, unsigned iters) {
    const unsigned half = iters / 2;
    auto t0 = Clock::now();
    for (unsigned i = 0; i < half; ++i) {
        a.hash_once(static_cast<std::uint64_t>(i) * 2 + 1);
        b.hash_once(static_cast<std::uint64_t>(i) * 2 + 2);
    }
    auto t1 = Clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <single|interleaved> [iters=2000] [seedA=0x00] [seedB=0x11]\n";
        return 2;
    }
    const std::string mode = argv[1];
    const unsigned iters = (argc > 2) ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 2000;
    const std::vector<std::byte> seedA = (argc > 3) ? parse_seed(argv[3]) : std::vector<std::byte>{std::byte{0x00}};
    const std::vector<std::byte> seedB = (argc > 4) ? parse_seed(argv[4]) : std::vector<std::byte>{std::byte{0x11}};

    Hw a(seedA), b(seedB);

    std::cout << std::fixed << std::setprecision(3);
    if (mode == "single") {
        const double secs = run_single(a, iters);
        const double hps = iters / secs;
        std::cout << "MODE=single iters=" << iters
                  << " time=" << secs << "s"
                  << " hashes_per_s=" << hps << '\n';
        std::cout << "  wrap in: perf stat -e cycles:u,instructions:u,other_interlock_stall:u -- "
                  << argv[0] << " single " << iters << '\n';
    } else if (mode == "interleaved") {
        const double secs = run_interleaved(a, b, iters);
        const double hps = iters / secs; // total hashes across both streams
        std::cout << "MODE=interleaved iters=" << iters
                  << " time=" << secs << "s"
                  << " total_hashes_per_s=" << hps << '\n';
        std::cout << "  wrap in: perf stat -e cycles:u,instructions:u,other_interlock_stall:u -- "
                  << argv[0] << " interleaved " << iters << '\n';
        std::cout << "  (compare total_hashes_per_s and other_interlock_stall/iter vs single)\n";
    } else {
        std::cerr << "unknown mode: " << mode << '\n';
        return 2;
    }
    return 0;
}
