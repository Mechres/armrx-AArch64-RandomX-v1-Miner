#pragma once

#include "armrx/instruction.hpp"
#include "armrx/aes_hash.hpp"
#include "armrx/argon2.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace armrx {

struct FloatRegister {
    double lo;
    double hi;
};

struct RegisterFile {
    std::uint64_t r[8];
    FloatRegister f[4];
    FloatRegister e[4];
    FloatRegister a[4];
};

struct InstructionByteCode {
    union {
        std::uint64_t* idst;
        FloatRegister* fdst;
    };
    union {
        const std::uint64_t* isrc;
        const FloatRegister* fsrc;
    };
    union {
        std::uint64_t imm;
        std::int64_t simm;
    };
    InstructionType type;
    union {
        std::int16_t target;
        std::uint16_t shift;
    };
    std::uint32_t memMask;
};

// Flags matching the RandomX VM features
inline constexpr std::uint32_t kRandOMXFlagDefault = 0U;
inline constexpr std::uint32_t kRandOMXFlagFullMem = 32U;
inline constexpr std::uint32_t kRandOMXFlagHardAes = 16U;

class VirtualMachine {
public:
    explicit VirtualMachine(std::uint32_t flags);
    ~VirtualMachine();

    // Disable copy
    VirtualMachine(const VirtualMachine&) = delete;
    VirtualMachine& operator=(const VirtualMachine&) = delete;

    void set_cache(const Argon2dCache* cache);
    void set_dataset(std::span<const std::byte> dataset);

    void allocate();
    void init_scratchpad(void* seed);
    void reset_rounding_mode();

    void run(const void* seed);
    void hash_and_fill(void* out, void* fill_state);
    void get_final_result(void* out);

    [[nodiscard]] const RegisterFile& get_register_file() const { return reg_; }
    [[nodiscard]] const std::byte* get_scratchpad() const { return scratchpad_.data(); }

private:
    void initialize_vm_state();
    void compile_program();
    void compile_instruction(const Instruction& instr, int instr_index, InstructionByteCode& ibc);
    void execute_bytecode();

    // Dataset read logic helper (interprets light vs fast mode)
    void dataset_read(std::uint64_t address, std::uint64_t (&r)[8]);

    std::uint32_t flags_;
    const Argon2dCache* cache_ = nullptr;
    std::span<const std::byte> dataset_;

    std::uint64_t dataset_offset_ = 0;
    std::uint32_t mx_ = 0;
    std::uint32_t ma_ = 0;

    std::uint64_t e_mask_[2] = {0};
    std::uint32_t read_reg0_ = 0;
    std::uint32_t read_reg1_ = 0;
    std::uint32_t read_reg2_ = 0;
    std::uint32_t read_reg3_ = 0;

    // Registers
    RegisterFile reg_{};

    // Compiled Bytecode & Program
    std::array<Instruction, 256> program_{};
    std::array<std::uint64_t, 16> entropy_{};
    std::array<InstructionByteCode, 256> bytecode_{};
    int register_usage_[8] = {-1};

    // Scratchpad (2 MiB)
    std::vector<std::byte> scratchpad_;

    // Temporary storage for hashing pipeline
    AesState temp_hash_{};
};

// Top-level public interface for hash calculations
void randomx_calculate_hash(VirtualMachine* machine, const void* input, std::size_t input_size, void* output);

} // namespace armrx
