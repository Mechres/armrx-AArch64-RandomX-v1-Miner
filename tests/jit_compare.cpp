#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include "armrx/aes_hash.hpp"
#include "armrx/dataset.hpp"
#include "armrx/blake2b.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <array>

std::string hex(std::span<const std::byte> bytes) {
    std::ostringstream os;
    for (auto b : bytes) os << std::hex << std::setw(2) << std::setfill('0')
                            << std::to_integer<unsigned>(b);
    return os.str();
}

int main() {
    constexpr std::array<std::byte, 12> key{
        std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'},
        std::byte{' '}, std::byte{'k'}, std::byte{'e'}, std::byte{'y'},
        std::byte{' '}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}
    };
    armrx::Argon2dCache cache;
    cache.initialize(key);

    alignas(16) std::array<std::byte, 32> h1_int, h1_jit, h2_int, h2_jit;

    // Interpreted VM
    armrx::VirtualMachine vm_int{armrx::kRandOMXFlagDefault};
    vm_int.set_cache(&cache);
    armrx::randomx_calculate_hash(&vm_int, "This is a test", 14, h1_int.data());
    armrx::randomx_calculate_hash(&vm_int, "Lorem ipsum dolor sit amet", 26, h2_int.data());

    // JIT VM
    armrx::VirtualMachine vm_jit{armrx::kRandOMXFlagHardAes | armrx::kRandOMXFlagJit};
    vm_jit.set_cache(&cache);
    armrx::randomx_calculate_hash(&vm_jit, "This is a test", 14, h1_jit.data());
    armrx::randomx_calculate_hash(&vm_jit, "Lorem ipsum dolor sit amet", 26, h2_jit.data());

    std::cout << "Input1 interpreted: " << hex(h1_int) << std::endl;
    std::cout << "Input1 JIT:         " << hex(h1_jit) << std::endl;
    std::cout << "Match: " << (h1_int == h1_jit ? "YES" : "NO") << std::endl;
    std::cout << std::endl;
    std::cout << "Input2 interpreted: " << hex(h2_int) << std::endl;
    std::cout << "Input2 JIT:         " << hex(h2_jit) << std::endl;
    std::cout << "Match: " << (h2_int == h2_jit ? "YES" : "NO") << std::endl;

    return (h1_int == h1_jit && h2_int == h2_jit) ? 0 : 1;
}
