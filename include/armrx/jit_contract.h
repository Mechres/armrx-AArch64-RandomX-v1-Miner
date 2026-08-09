// Copyright (c) 2026 armrx contributors. Licensed under GPLv3.
//
// JIT register/label contract — single source of truth shared by the C++ JIT
// emitter and (as documentation) the hand-written AArch64 assembly in
// `src/jit_compiler_a64_static.S`.
//
// Historically the register allocation lived only as a 49-line comment block in
// the .S and the C++ side independently recomputed template sizes from linker
// symbols. When the two drifted (an asm edit without a matching C++ constant
// change) the emitter silently produced wrong-size / overlapping code. This
// header makes the contract *mechanical*:
//   * `IntRegMap` is the authoritative integer-register allocation, checked by
//     static_assert for the invariants the .S comment promises.
//   * `kExpected*` are the known-good label deltas; `jit_compiler_a64.cpp`
//     asserts the actual computed deltas equal these at startup, so a future
//     asm/C++ drift fails loudly instead of corrupting the JIT buffer.
//
// Neither the table nor the expected offsets change any emitted instruction, so
// hashing behavior is byte-identical to the pre-contract code.

#ifndef ARMRX_JIT_CONTRACT_H
#define ARMRX_JIT_CONTRACT_H

#include <cstddef>
#include <cstdint>

namespace armrx {

// Integer-register allocation (mirrors jit_compiler_a64_static.S:83-98).
// Indexed by RandomX register id 0..7 -> AArch64 x-register number.
// The set {4,5,6,7,12,13,14,15} must be distinct, in range, and must NOT
// collide with the reserved/callee-saved registers the prologue preserves
// (x16,x17,x19..x30) or the platform register x18.
constexpr uint32_t IntRegMap[8] = {4, 5, 6, 7, 12, 13, 14, 15};

static_assert(sizeof(IntRegMap) / sizeof(IntRegMap[0]) == 8,
              "IntRegMap must cover RandomX registers 0..7");

// Distinctness + range. A duplicate or out-of-range entry would silently
// alias two RandomX registers onto one physical register (the historical
// register-clobber hazard this contract exists to prevent).
namespace detail {
constexpr bool IntRegMapInvariants() {
    for (size_t i = 0; i < 8; ++i) {
        if (IntRegMap[i] > 30) return false;          // x0..x30 only
        for (size_t j = 0; j < i; ++j)
            if (IntRegMap[i] == IntRegMap[j]) return false;
    }
    return true;
}
} // namespace detail

static_assert(detail::IntRegMapInvariants(),
              "IntRegMap entries must be distinct and in x0..x30");

// Known-good label deltas (bytes) from the linked JIT template, measured
// against the cross/native build at the time this contract was introduced.
// `jit_compiler_a64.cpp` asserts the runtime-computed deltas match these.
// If a future asm edit changes the template layout, the assert fires instead
// of the emitter silently using a stale size.
constexpr size_t kExpectedCodeSize              = 51516; // program + dataset templates
constexpr size_t kExpectedPrologueSize          = 480;   // up to first vm instruction
constexpr size_t kExpectedMainLoopBegin         = 320;   // main_loop label offset
constexpr size_t kExpectedImulRcpLiteralsEnd    = 49728; // IMUL_RCP literal pool end

// CalcDatasetItem template sub-offsets (informational; checked by the emitter
// budget assert in jit_compiler_a64.cpp, not here).
constexpr size_t kCalcDatasetItemPrologue       = 176;   // base -> prefetch
constexpr size_t kCalcDatasetItemMixDelta       = 12;    // prefetch -> mix
constexpr size_t kCalcDatasetItemStoreDelta     = 48;    // mix -> store_result
constexpr size_t kCalcDatasetItemEpilogue       = 52;    // store_result -> end

} // namespace armrx

#endif // ARMRX_JIT_CONTRACT_H
