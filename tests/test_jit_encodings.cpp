// CBRANCH JIT encoding unit test.
// Decodes the actual emitted `bne`/`b` bytes for every CBRANCH sequence and
// asserts the computed branch target is a real, backward, in-bounds address.
//
// This replaces an earlier, much weaker version of this test that only
// checked each CBRANCH's emitted size (>=4 bytes) despite this file's own
// original comment claiming to verify branch targets — it never actually
// did. See docs/branchless-cbranch.md's "Unit test recommendation" section:
// a prior CBRANCH encoding bug (imm19 off-by-one) caused an infinite loop
// that only showed up as a 120s test *hang*, not a fast, localized failure.
// Decoding the real bytes turns that into an instant assertion.

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <cstring>
#include <iostream>
#include <iomanip>
#include <cstdint>
#include <array>

namespace {

// Opcode frequency weights (from instruction_weights.hpp)
constexpr int kWeights[] = {
    16, 7, 16, 7, 16, 4, 4, 1, 4, 1, 8, 2,
    15, 5, 8, 2, 4, 4, 16, 5, 16, 5, 6, 32,
    4, 6, 25, 1, 16, 0,
};
constexpr int kNumHandlers = sizeof(kWeights) / sizeof(kWeights[0]);
constexpr int kCbranchHandlerIdx = 26; // CBRANCH's slot in kWeights/kHandlerNames

int g_opcode_to_idx[256];
void build_map() {
    static bool done = false;
    if (done) return;
    int slot = 0;
    for (int h = 0; h < kNumHandlers; ++h) {
        for (int w = 0; w < kWeights[h]; ++w) {
            if (slot < 256) g_opcode_to_idx[slot++] = h;
        }
    }
    while (slot < 256) g_opcode_to_idx[slot++] = kNumHandlers - 1; // NOP
    done = true;
}

std::uint32_t read_u32(std::span<const std::uint8_t> code, std::size_t offset) {
    std::uint32_t v;
    std::memcpy(&v, code.data() + offset, sizeof(v));
    return v;
}

} // namespace

int main() {
    build_map();

    constexpr int kNumSeeds = 5;
    int total_cbranches = 0;
    int bad_cbranches = 0;

    for (int si = 0; si < kNumSeeds; ++si) {
        std::string seed = "cbranch_test_" + std::to_string(si);
        std::vector<std::byte> key;
        for (char c : seed) key.push_back(static_cast<std::byte>(c));

        armrx::Argon2dCache cache;
        cache.initialize(key);

        armrx::VirtualMachine vm(armrx::kRandOMXFlagJit | armrx::kRandOMXFlagHardAes);
        vm.setJitDumpEnabled();
        vm.set_cache(&cache);

        std::array<std::byte, 32> hash{};
        const char* input = "CBRANCH encoding test";
        armrx::randomx_calculate_hash(&vm, input, std::strlen(input), hash.data());

        const auto& dump = vm.getJitDump();
        const auto code = vm.getJitCodeBytes();

        // randomx_calculate_hash() JIT-compiles and executes several chained
        // internal programs per call, reusing the same buffer each time
        // (emitPrologueMix resets codePos to a fixed start on every
        // compile). getJitDump() accumulates entries from every round
        // (enableJitDump() only clears once, at the start), but the buffer
        // returned by getJitCodeBytes() only holds the *last* round's bytes
        // — entries from earlier rounds share the same offset numbers but
        // point to memory a later round has since overwritten. Only the
        // final contiguous run of entries (after the last offset reset)
        // corresponds to what's actually in the buffer right now.
        std::size_t last_round_start = 0;
        for (std::size_t i = 1; i < dump.size(); ++i) {
            if (dump[i].offset < dump[i - 1].offset) last_round_start = i;
        }

        for (std::size_t i = last_round_start; i < dump.size(); ++i) {
            const auto& e = dump[i];
            const int idx = g_opcode_to_idx[e.opcode % 256];
            if (idx != kCbranchHandlerIdx) continue;
            ++total_cbranches;

            if (e.size < 12 || e.offset + e.size > code.size()) {
                std::cerr << "FAIL: CBRANCH at offset " << e.offset
                          << " size=" << e.size << " (min 12, out of bounds)\n";
                ++bad_cbranches;
                continue;
            }

            // Locate the bne by its exact bit pattern rather than assuming a
            // fixed position: emitAddImmediate's length varies (1-3 words,
            // sometimes via a NEON smov/umov literal-pool reference emitted
            // by emitMovImmediate), so the tst/bne/b tail isn't always at a
            // fixed offset from the end of the recorded entry. The bne word
            // itself is a fixed, unique bit pattern (nothing else in the JIT
            // compiler emits it — CFROUND's own bne, gated behind
            // RANDOMX_FLAG_V2, uses a different encoding), so scanning for it
            // and reading the next word as `b` is robust regardless of
            // exactly where the variable-length prefix lands.
            constexpr std::uint32_t kExpectedBne = 0x54000000 | (2U << 5) | 1U;
            int bne_matches = 0;
            std::size_t bne_pos = 0;
            for (std::size_t o = e.offset; o + 8 <= e.offset + e.size; o += 4) {
                if (read_u32(code, o) == kExpectedBne) {
                    ++bne_matches;
                    bne_pos = o;
                }
            }
            if (bne_matches != 1) {
                std::cerr << "FAIL: CBRANCH at offset " << e.offset << " size=" << e.size
                          << " opcode=" << e.opcode << " idx=" << idx
                          << " found " << bne_matches << " bne matches (expected exactly 1)\n";
                ++bad_cbranches;
                continue;
            }

            const std::uint32_t b_word = read_u32(code, bne_pos + 4);
            // b is an unconditional branch (top 6 bits 000101) with a 26-bit
            // signed, word-granular PC-relative offset.
            if ((b_word >> 26) != 0b000101) {
                std::cerr << "FAIL: CBRANCH at offset " << e.offset
                          << " b instruction has wrong opcode bits: 0x"
                          << std::hex << b_word << std::dec << "\n";
                ++bad_cbranches;
                continue;
            }
            std::int32_t imm26 = static_cast<std::int32_t>(b_word << 6) >> 6; // sign-extend
            const std::int64_t pc_of_b = static_cast<std::int64_t>(bne_pos) + 4;
            const std::int64_t target = pc_of_b + (static_cast<std::int64_t>(imm26) * 4);

            // CBRANCH only ever jumps backward within the buffer (RandomX
            // spec §2.7 — it re-executes earlier instructions), never
            // forward and never out of bounds.
            if (target < 0 || target >= static_cast<std::int64_t>(code.size()) || target > pc_of_b) {
                std::cerr << "FAIL: CBRANCH at offset " << e.offset
                          << " computed target=" << target
                          << " (buffer size=" << code.size() << ", b at " << pc_of_b << ")\n";
                ++bad_cbranches;
            }
        }
    }

    if (total_cbranches == 0) {
        std::cerr << "FAIL: no CBRANCH instructions found in any program\n";
        return 1;
    }

    std::cout << "CBRANCH encoding test: " << total_cbranches
              << " instructions across " << kNumSeeds << " seeds, ";
    if (bad_cbranches) {
        std::cout << "FAILURES=" << bad_cbranches << "\n";
    } else {
        std::cout << "all OK\n";
    }

    return bad_cbranches ? 1 : 0;
}
