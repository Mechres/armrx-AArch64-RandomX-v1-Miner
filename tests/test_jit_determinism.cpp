// JIT determinism test.
// Compiles the same program twice with the same seed and asserts
// byte-identical JIT output and identical hash output.

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <cstdint>
#include <array>
#include <vector>

int main() {
    std::string seed = "determinism_test_seed";
    std::vector<std::byte> key;
    for (char c : seed) key.push_back(static_cast<std::byte>(c));

    armrx::Argon2dCache cache;
    cache.initialize(key);

    const char* input = "determinism test input";
    std::array<std::byte, 32> hash1{};
    std::array<std::byte, 32> hash2{};

    // Run 1
    std::vector<armrx::VirtualMachine::JitDumpEntry> dump1;
    {
        armrx::VirtualMachine vm(armrx::kRandOMXFlagJit | armrx::kRandOMXFlagHardAes);
        vm.setJitDumpEnabled();
        vm.set_cache(&cache);
        armrx::randomx_calculate_hash(&vm, input, std::strlen(input), hash1.data());
        dump1 = vm.getJitDump(); // copy
    }

    // Run 2
    std::vector<armrx::VirtualMachine::JitDumpEntry> dump2;
    {
        armrx::VirtualMachine vm(armrx::kRandOMXFlagJit | armrx::kRandOMXFlagHardAes);
        vm.setJitDumpEnabled();
        vm.set_cache(&cache);
        armrx::randomx_calculate_hash(&vm, input, std::strlen(input), hash2.data());
        dump2 = vm.getJitDump(); // copy
    }

    // Compare hashes
    if (hash1 != hash2) {
        std::cerr << "FAIL: hash mismatch between runs\n";
        return 1;
    }

    // Compare JIT dump entries
    if (dump1.size() != dump2.size()) {
        std::cerr << "FAIL: JIT dump size mismatch: " << dump1.size()
                  << " vs " << dump2.size() << "\n";
        return 1;
    }

    for (size_t i = 0; i < dump1.size(); ++i) {
        const auto& a = dump1[i];
        const auto& b = dump2[i];
        if (a.opcode != b.opcode || a.offset != b.offset || a.size != b.size) {
            std::cerr << "FAIL: JIT entry " << i << " mismatch: "
                      << "(" << a.opcode << "," << a.offset << "," << a.size << ") vs "
                      << "(" << b.opcode << "," << b.offset << "," << b.size << ")\n";
            return 1;
        }
    }

    std::cout << "JIT determinism test: " << dump1.size()
              << " instructions, seed \"" << seed << "\", "
              << "hash=";
    for (auto b : hash1) std::cout << std::hex << std::setw(2) << std::setfill('0')
                                   << static_cast<int>(b);
    std::cout << std::dec << "\n"
              << "Result: deterministic (hash and JIT output match)\n";
    return 0;
}
