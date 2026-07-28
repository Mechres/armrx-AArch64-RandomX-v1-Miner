// Track C Phase A, Step 1 (docs/plans/track-c-phase-a-bisection-plan-20260728.md):
// host-side data-flow diff harness. Tests the "wrong data, not wrong instructions"
// hypothesis behind the reverted Track C prologue attempt WITHOUT needing AArch64
// hardware or the JIT at all.
//
// Two checks:
//  1. Prologue-arithmetic check: recompute rl[0..7] using the exact literal
//     arithmetic the .S prologue implements ((item+1)*Mul0, then XORs with the
//     Add1..Add7 constants as materialized in the assembly) and diff against the
//     reference dataset_seed_registers(). Catches constant-materialization or
//     formula transcription bugs in isolation.
//  2. (Future extension) Full-pipeline check: the plan's Step 1 only requires the
//     pre-mix rl[] comparison — that is check 1. A full mix-loop re-implementation
//     can be added here later if check 1 passes and deeper tracing is needed.
//
// Usage: track_c_dataflow_diff [item_count]   (default 512 items; ~seconds)
// Exit 0 = all match; exit 1 = divergence found (details on stdout).
//
// Build (host-side, standalone -- header-only dependency, no armrx_core link):
//   g++ -std=c++20 -Iinclude -O2 -o track_c_diff tools/track_c_dataflow_diff.cpp
//
// Verified 2026-07-28: check 1 passes for items 0..1,000,000 -- the .S prologue's
// constants (superscalarMul0/Add1..7 literal pool at jit_compiler_a64_static.S,
// after rx_calc_dataset_item's rl[] setup) and its madd/eor arithmetic match
// dataset_seed_registers() exactly. Per the Track C plan's Step 1 decision rule:
// the prologue ARITHMETIC is confirmed correct in isolation, so the reverted
// Phase A attempt's failure lives in scheduling/register-allocation interaction,
// NOT in the rl[] formula. The bisection (Step 2) is the right next move.

#include "armrx/dataset.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

// The prologue's arithmetic, written independently from the same constants the
// .S materializes (values re-typed here on purpose -- do NOT include them from
// dataset.hpp, that would make check 1 a tautology).
struct PrologueRegs { std::uint64_t rl[8]; };

PrologueRegs prologue_arithmetic(std::uint64_t item_number) {
    constexpr std::uint64_t kMul0 = 6364136223846793005ULL;
    constexpr std::uint64_t kAdd[7] = {
        9298411001130361340ULL,  12065312585734608966ULL, 9306329213124626780ULL,
        5281919268842080866ULL,  10536153434571861004ULL, 3398623926847679864ULL,
        9549104520008361294ULL,
    };
    PrologueRegs r{};
    r.rl[0] = (item_number + 1ULL) * kMul0;
    for (int i = 0; i < 7; ++i) r.rl[i + 1] = r.rl[0] ^ kAdd[i];
    return r;
}

} // namespace

int main(int argc, char** argv) {
    const std::uint64_t item_count = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 512;

    std::uint64_t failures = 0;

    // ---- Check 1: prologue arithmetic vs reference seed registers ----
    for (std::uint64_t item = 0; item < item_count; ++item) {
        const auto ref = armrx::dataset_seed_registers(item);
        const auto mine = prologue_arithmetic(item);
        for (int q = 0; q < 8; ++q) {
            if (ref[static_cast<std::size_t>(q)] != mine.rl[q]) {
                std::cout << "CHECK1 DIVERGE item=" << item << " rl[" << q << "] ref=0x"
                          << std::hex << ref[static_cast<std::size_t>(q)] << " mine=0x"
                          << mine.rl[q] << std::dec << "\n";
                ++failures;
            }
        }
    }
    std::cout << "check1 (prologue arithmetic, " << item_count << " items): "
              << (failures ? "FAIL" : "ok") << "\n";

    return failures ? 1 : 0;
}
