#pragma once

#include "armrx/instruction.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace armrx {

struct ProgramConfiguration {
    std::uint64_t eMask[2];
    std::uint32_t readReg0, readReg1, readReg2, readReg3;
};

struct MemoryRegisters {
    std::uint32_t mx, ma;
    const std::uint8_t* memory = nullptr;
    const std::uint8_t* partial_dataset_ = nullptr;
    std::size_t partial_dataset_items_ = 0;
};

class Program {
public:
    [[nodiscard]] Instruction& operator()(std::size_t i) {
        return program_buffer_[i];
    }

    [[nodiscard]] const Instruction& operator()(std::size_t i) const {
        return program_buffer_[i];
    }

    [[nodiscard]] static constexpr std::size_t getSize(std::uint32_t) {
        return 256;
    }

    std::array<Instruction, 256> program_buffer_{};
};

} // namespace armrx
