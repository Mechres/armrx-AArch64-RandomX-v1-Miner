#include "armrx/aes.hpp"
#include "armrx/aes_generator.hpp"
#include "armrx/aes_hash.hpp"
#include "armrx/blake2b.hpp"
#include "armrx/argon2.hpp"
#include "armrx/vm.hpp"
#include "armrx/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <span>

namespace {

std::string hex_bytes(std::span<const std::byte> bytes) {
    std::ostringstream result;
    for (const auto byte : bytes) {
        result << std::hex << std::setw(2) << std::setfill('0')
               << std::to_integer<unsigned>(byte);
    }
    return result.str();
}

std::string hex_u64(std::uint64_t v) {
    std::ostringstream result;
    result << std::hex << std::setw(16) << std::setfill('0') << v;
    return result.str();
}

void dump_state(const std::string& label, std::span<const std::byte> bytes) {
    std::cout << "[DEBUG] " << label << " (" << bytes.size() << " bytes): " << hex_bytes(bytes) << std::endl;
}

void dump_entropy(const std::string& label, const std::uint64_t* entropy, int count) {
    std::cout << "[DEBUG] " << label << ":";
    for (int i = 0; i < count; ++i) {
        std::cout << " " << hex_u64(entropy[i]);
    }
    std::cout << std::endl;
}

} // namespace

int main() {
    // Set up the reference cache like test_blake2b does
    constexpr std::array<std::byte, 12> reference_key{
        std::byte{'t'}, std::byte{'e'}, std::byte{'s'}, std::byte{'t'},
        std::byte{' '}, std::byte{'k'}, std::byte{'e'}, std::byte{'y'},
        std::byte{' '}, std::byte{'0'}, std::byte{'0'}, std::byte{'0'}
    };
    armrx::Argon2dCache reference_cache;
    reference_cache.initialize(reference_key);

    // Step 1: Compute tempHash from blake2b("This is a test")
    const char input1[] = "This is a test";
    alignas(16) std::array<std::byte, 64> temp_hash{};
    std::span<const std::byte> input_span(reinterpret_cast<const std::byte*>(input1), sizeof(input1) - 1);
    armrx::blake2b(input_span, temp_hash.data(), 64);
    dump_state("tempHash after blake2b", temp_hash);
    // Also print as 8 uint64_t values
    std::cout << "[DEBUG] tempHash u64:";
    for (int i = 0; i < 8; ++i) {
        std::uint64_t v;
        std::memcpy(&v, temp_hash.data() + i * 8, 8);
        std::cout << " " << hex_u64(v);
    }
    std::cout << std::endl;

    // Step 2: Run AesGenerator4R with tempHash as seed
    armrx::AesState seed;
    std::memcpy(seed.data(), temp_hash.data(), 64);
    armrx::AesGenerator4R gen{seed};

    // Generate entropy (16 uint64_t = 128 bytes) + program (4096 bytes)
    std::array<std::byte, 128 + 4096> prog_bytes{};
    gen.fill(prog_bytes);

    // Print entropy as 16 uint64_t values
    std::uint64_t entropy[16];
    std::memcpy(entropy, prog_bytes.data(), 128);
    dump_entropy("entropy", entropy, 16);

    // Compute expected mx_, ma_, read_regs, dataset_offset from entropy
    std::uint32_t ma_expected = static_cast<std::uint32_t>(entropy[8] & 0x7fffffc0ULL);
    std::uint32_t mx_expected = static_cast<std::uint32_t>(entropy[10]);
    auto addressRegisters = entropy[12];
    std::uint32_t read_reg0 = 0 + (addressRegisters & 1);
    addressRegisters >>= 1;
    std::uint32_t read_reg1 = 2 + (addressRegisters & 1);
    addressRegisters >>= 1;
    std::uint32_t read_reg2 = 4 + (addressRegisters & 1);
    addressRegisters >>= 1;
    std::uint32_t read_reg3 = 6 + (addressRegisters & 1);
    std::uint64_t dataset_offset = (entropy[13] % 524288ULL) * 64;

    std::cout << "[DEBUG] -> mx_= " << hex_u64(mx_expected)
              << " ma_= " << hex_u64(ma_expected)
              << " read_reg0= " << read_reg0
              << " read_reg1= " << read_reg1
              << " read_reg2= " << read_reg2
              << " read_reg3= " << read_reg3
              << " dataset_offset= " << hex_u64(dataset_offset)
              << std::endl;

    // Step 3: Run init_scratchpad and then run VM
    armrx::VirtualMachine test_vm{armrx::kRandOMXFlagDefault};
    test_vm.set_cache(&reference_cache);

    // Init scratchpad
    test_vm.init_scratchpad(temp_hash.data());
    test_vm.reset_rounding_mode();

    // Run first chain
    alignas(16) std::array<std::byte, 64> chain_hash{};
    std::memcpy(chain_hash.data(), temp_hash.data(), 64);

    // We'll run just one loop iteration to see first chain state
    test_vm.run(chain_hash.data());

    // Print registers
    const auto& reg = test_vm.get_register_file();
    std::cout << "[DEBUG] After first chain run reg_r:";
    for (int i = 0; i < 8; ++i) {
        std::cout << " " << hex_u64(reg.r[i]);
    }
    std::cout << std::endl;
    std::cout << "[DEBUG] Group A floats:";
    for (int i = 0; i < 4; ++i) {
        std::uint64_t lo, hi;
        std::memcpy(&lo, &reg.a[i].lo, 8);
        std::memcpy(&hi, &reg.a[i].hi, 8);
        std::cout << " [a" << i << " lo=" << hex_u64(lo) << " hi=" << hex_u64(hi) << "]";
    }
    std::cout << std::endl;

    // Compute final hash for comparison
    alignas(16) std::array<std::byte, 32> output_hash{};
    armrx::randomx_calculate_hash(&test_vm, input1, sizeof(input1) - 1, output_hash.data());
    std::cout << "[DEBUG] Final hash: " << hex_bytes(output_hash) << std::endl;
    std::cout << "[DEBUG] Expected:    639183aae1bf4c9a35884cb46b09cad9175f04efd7684e7262a0ac1c2f0b4e3f" << std::endl;

    return 0;
}
