# armrx — Next-Phase Improvement & Evolution Plan (v3)

This document outlines the current outstanding engineering, optimization, and polish tasks for the `armrx` project following the completion of the Phase 2 optimizations (PGO, JIT loop refactoring, Software AES inlining, and big.LITTLE scheduling).

---

## 1. Outstanding Tasks Matrix

### 1.1 Core Engineering & Optimization

| ID | Action | Est. Gain | Rationale |
|---|---|---|---|
| **O3.1** | **Peephole JIT Phase 2 Opcodes Audit** | +3–7% hashrate | Execute the per-opcode audit from `docs/peephole-jit-plan.md`. Start by reducing the JIT `FDIV_M` Markstein iteration steps from 17 instructions down to 8 instructions, and audit remaining high-weight arithmetic opcodes. |
| **O3.2** | **Re-measure CBRANCH Miss Rate** | Diagnostic / Saved effort | Re-measure hardware PMU counters with `perf record -e branch-misses`. If 94%+ of branch misses still reside in the dataset generation (`execute_superscalar`) and not the JIT execution path, keep JIT `CBRANCH` branchless rewriting (CSEL/CINC) deprioritized. |
| **O3.3** | **Stratum V2 Protocol Implementation** | Reliability / Efficiency | Implement native Stratum V2 protocol support to reduce data transfer sizes and improve communication efficiency with modern mining pools. |
| **O3.4** | **Cross-Compile CI Pipeline** | Maintainability | Construct a GitHub Actions CI pipeline to verify cross-compilation for AArch64 and run interpreted-mode unit tests on x86_64 runners. |

---

## 2. Maintenance, Cleanliness & Fuzzing

### 2.1 Refactoring & Code Consolidations

*   **Consolidate AES Key Constants:** Currently, `src/aes_generator.cpp` and `src/aes_hash.cpp` duplicate the RandomX 1R and 4R AES round keys in different encodings. Extract these into a single shared header `include/armrx/aes_keys.hpp`.
*   **Derive `kCompileHandlers[256]`:** Instead of a static 73-line dispatch mapping table at `src/vm.cpp`, programmatically generate this array mapping using the weight constants from `include/armrx/instruction_weights.hpp`.
*   **Consolidate Scratchpad Mask Constants:** Standardize the interpreter mask `kScratchpadL3Mask64 = 2097088U` and the JIT `Log2(RANDOMX_SCRATCHPAD_L3) - 1` definitions into a single source of truth.

### 2.2 Testing Coverage Gaps

*   **JSON Injection Fuzzing:** Write direct fuzzing harnesses for `stratum_client`'s `handle_reply()` to verify validation robustness against invalid control sequences, escaped quotes, and backslashes in Stratum server responses.
*   **Direct Software AES Unit Tests:** Currently, `fill_aes_1r_x4` and `hash_aes_1r_x4` are only verified indirectly via end-to-end hash tests. Add explicit unit tests with known-reference byte vectors to directly verify the software AES transforms.
