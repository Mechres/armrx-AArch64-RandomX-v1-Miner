#pragma once

#include "armrx/instruction.hpp"
#include "armrx/randomx_config.hpp"
#include "armrx/blake2_generator.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace armrx {

enum class SuperscalarInstructionType {
    ISUB_R = 0,
    IXOR_R = 1,
    IADD_RS = 2,
    IMUL_R = 3,
    IROR_C = 4,
    IADD_C7 = 5,
    IXOR_C7 = 6,
    IADD_C8 = 7,
    IXOR_C8 = 8,
    IADD_C9 = 9,
    IXOR_C9 = 10,
    IMULH_R = 11,
    ISMULH_R = 12,
    IMUL_RCP = 13,

    COUNT = 14,
    INVALID = -1
};

class SuperscalarProgram {
public:
    [[nodiscard]] Instruction& operator()(std::size_t pc) {
        return program_buffer_[pc];
    }

    [[nodiscard]] const Instruction& operator()(std::size_t pc) const {
        return program_buffer_[pc];
    }

    [[nodiscard]] std::uint32_t size() const {
        return size_;
    }

    void set_size(std::uint32_t val) {
        size_ = val;
    }

    [[nodiscard]] int address_register() const {
        return addr_reg_;
    }

    void set_address_register(int val) {
        addr_reg_ = val;
    }

    std::array<Instruction, kSuperscalarMaxSize> program_buffer_{};
    std::uint32_t size_{0};
    int addr_reg_{0};
    double ipc{0.0};
    int code_size{0};
    int macro_ops{0};
    int decode_cycles{0};
    int cpu_latency{0};
    int asic_latency{0};
    int mul_count{0};
    std::array<int, 8> cpu_latencies{};
    std::array<int, 8> asic_latencies{};
};

std::uint64_t randomx_reciprocal(std::uint32_t divisor);

void generate_superscalar(SuperscalarProgram& prog, Blake2Generator& gen);

void execute_superscalar(std::array<std::uint64_t, 8>& r, const SuperscalarProgram& prog,
                         const std::vector<std::uint64_t>* reciprocals = nullptr);

} // namespace armrx
