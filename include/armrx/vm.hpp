#pragma once

#include "armrx/program.hpp"
#include "armrx/aes_hash.hpp"
#include "armrx/argon2.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#ifdef ARMRX_HAVE_JIT
#include "armrx/jit_compiler_a64.hpp"
#endif

namespace armrx {

struct FloatRegister {
    double lo;
    double hi;
};

struct alignas(16) RegisterFile {
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
inline constexpr std::uint32_t kRandOMXFlagJit     = 4U;

class VirtualMachine {
public:
    explicit VirtualMachine(std::uint32_t flags);
    ~VirtualMachine();

    // Disable copy
    VirtualMachine(const VirtualMachine&) = delete;
    VirtualMachine& operator=(const VirtualMachine&) = delete;

    void set_cache(const Argon2dCache* cache);
    [[nodiscard]] bool set_dataset(std::span<const std::byte> dataset);

    void allocate();
    void init_scratchpad(void* seed);
    void reset_rounding_mode();

    void run(const void* seed);
    void hash_and_fill(void* out, void* fill_state);
    void get_final_result(void* out);

    [[nodiscard]] const RegisterFile& get_register_file() const { return reg_; }
    [[nodiscard]] const std::byte* get_scratchpad() const { return scratchpad_data_; }

#ifdef ARMRX_JIT_PROFILE
    [[nodiscard]] std::uint64_t get_jit_compile_time_ns() const { return jit_compile_time_ns_; }
    [[nodiscard]] std::uint64_t get_jit_execute_time_ns() const { return jit_execute_time_ns_; }
    [[nodiscard]] std::uint64_t get_jit_total_runs() const { return jit_total_runs_; }
    void reset_jit_timers() {
        jit_compile_time_ns_ = 0;
        jit_execute_time_ns_ = 0;
        jit_total_runs_ = 0;
    }
#else
    [[nodiscard]] std::uint64_t get_jit_compile_time_ns() const { return 0; }
    [[nodiscard]] std::uint64_t get_jit_execute_time_ns() const { return 0; }
    [[nodiscard]] std::uint64_t get_jit_total_runs() const { return 0; }
    void reset_jit_timers() {}
#endif

private:
    void initialize_vm_state();
    void compile_program();
    void compile_instruction(const Instruction& instr, int instr_index, InstructionByteCode& ibc);
    void execute_bytecode();

    // Fast-mode helpers
    [[nodiscard]] bool is_fast_mode() const { return (flags_ & kRandOMXFlagFullMem) != 0; }

    // Split run paths
#ifdef ARMRX_HAVE_JIT
    void run_jit();
#endif
    void run_interpreted();

    // Instruction compiler dispatch table (replaces the 24-block opcode ladder)
    using CompileHandler = void(VirtualMachine::*)(const Instruction&, int, InstructionByteCode&);
    static const CompileHandler kCompileHandlers[256];

    void h_IADD_RS(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IADD_M(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_ISUB_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_ISUB_M(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IMUL_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IMUL_M(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IMULH_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IMULH_M(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_ISMULH_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_ISMULH_M(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IMUL_RCP(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_INEG_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IXOR_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IXOR_M(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IROR_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_IROL_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_ISWAP_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_FSWAP_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_FADD_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_FADD_M(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_FSUB_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_FSUB_M(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_FSCAL_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_FMUL_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_FDIV_M(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_FSQRT_R(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_CBRANCH(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_CFROUND(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_ISTORE(const Instruction& instr, int i, InstructionByteCode& ibc);
    void h_NOP(const Instruction& /*instr*/, int /*i*/, InstructionByteCode& ibc);

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
    Program program_{};
    std::array<std::uint64_t, 16> entropy_{};
    std::array<InstructionByteCode, 256> bytecode_{};
    int register_usage_[8] = {-1};

    // Scratchpad (2 MiB) — mmap-allocated with huge page hint
    std::byte* scratchpad_data_ = nullptr;
    std::size_t scratchpad_size_ = 0;

    // Temporary storage for hashing pipeline
    AesState temp_hash_{};

#ifdef ARMRX_HAVE_JIT
    std::unique_ptr<JitCompilerA64> jit_;
#endif

#ifdef ARMRX_JIT_PROFILE
    std::uint64_t jit_compile_time_ns_ = 0;
    std::uint64_t jit_execute_time_ns_ = 0;
    std::uint64_t jit_total_runs_ = 0;
#endif
};

// Top-level public interface for hash calculations
void randomx_calculate_hash(VirtualMachine* machine, const void* input, std::size_t input_size, void* output);

} // namespace armrx
