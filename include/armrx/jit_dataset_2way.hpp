/*
Track D1 (docs/plans/track-d1-superscalar-interleave-plan-20260728.md):
2-way interleaved superscalar dataset-item derivation.

This is a standalone experiment/benchmark module, deliberately kept
separate from JitCompilerA64: it allocates its own JIT buffer, does not
touch JitCompilerA64::code or its constructor's allocation size, and is
not wired into any production call site (generateSuperscalarHash(),
generateProgramLight(), or the mining path). Track D1's plan explicitly
scopes production wiring as a separate follow-on gated on this module's
own L1 I-cache measurement -- see the plan's Step 5.

The interleaved function this module builds derives TWO independent
RandomX dataset items (same cache, arbitrary/independent item numbers) in
one call, emitting each superscalar instruction twice -- once per stream's
disjoint physical-register set -- in the SAME schedule order
JitCompilerA64::computeSuperscalarEmitOrder() already produces and
verifies for the existing single-stream path. No new reordering, hence no
new hazard analysis: the only new correctness question is "do the two
streams' register footprints stay disjoint," which is checkable by
construction (see the register-contract comment at
randomx_calc_dataset_item_aarch64_2way in jit_compiler_a64_static.S).
*/

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "armrx/randomx_config.hpp"
#include "armrx/superscalar.hpp"

namespace armrx {

using SuperscalarProgramList2Way = std::array<SuperscalarProgram, kRandomXCacheAccesses>;

// Function signature of the generated 2-way derivation entry point
// (randomx_calc_dataset_item_aarch64_2way): derives two 64-byte dataset
// items from the same cache in one call.
using Dataset2WayFunc = void (*)(const void* cache, void* outA, std::uint64_t itemA, void* outB,
                                  std::uint64_t itemB);

// Caller-supplied scheduler: must return the same emission order
// JitCompilerA64::computeSuperscalarEmitOrder() would produce for this
// exact program (both streams execute the identical per-seed program, so
// one schedule call per round covers both streams -- see the plan's "no
// hazard analysis" argument). Kept as an injected callback rather than a
// direct dependency on JitCompilerA64, so this module doesn't need to
// know about (or risk destabilizing) the production JIT compiler class.
using SuperscalarScheduler = std::function<std::vector<std::uint32_t>(const SuperscalarProgram&)>;

class JitDataset2Way {
public:
    JitDataset2Way();
    ~JitDataset2Way();

    JitDataset2Way(const JitDataset2Way&) = delete;
    JitDataset2Way& operator=(const JitDataset2Way&) = delete;

    // Builds the interleaved 2-way function for one seed's superscalar
    // program list. Safe to call repeatedly (e.g. once per seed rotation
    // in a benchmark loop) -- each call re-emits into the same buffer.
    void generate(const SuperscalarProgramList2Way& programs, const std::vector<std::uint64_t>& reciprocalCache,
                  const SuperscalarScheduler& scheduler);

    [[nodiscard]] Dataset2WayFunc getFunc() const;
    [[nodiscard]] std::size_t getCodeSize() const { return codeSize_; }

private:
    std::uint8_t* code_ = nullptr;
    std::size_t bufferBytes_ = 0;
    std::uint32_t codeSize_ = 0;
};

} // namespace armrx
