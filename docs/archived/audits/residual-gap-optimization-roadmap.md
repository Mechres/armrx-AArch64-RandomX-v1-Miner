# armrx — Residual-Gap Optimization Roadmap (post-W4, read-only analysis)

**Scope:** static analysis + host-side verification only (qemu-aarch64-static). No device runs, no code changes.

---

## 0. Three corrections to the premise — the picture has materially changed

Before any roadmap, three findings from this analysis rewrite the situation. They are all verified against the code and, where noted, against a live `--jit-dump` run under qemu.

### 0.1 The reference does NOT use a NEON register pool in the dataset-item path

The premise that the reference's C-pool is "a NEON vector register pool (UMOV/SMOV from v16–v31, loaded ONCE at the dataset-item prologue)" is **incorrect**. The reference's `generateSuperscalarHash` (`scratch_vm_study/upstream_rx/src/jit_compiler_a64.cpp:315`) is structurally identical to armrx's pre-W4 code, including **`num32bitLiterals = 64` pinned at line 324** — the exact same pin armrx has at `src/jit_compiler_a64.cpp:1114`. Its superscalar `IADD_C*`/`IXOR_C*` go through `emitAddImmediate`/`emitMovImmediate` → MOVZ/MOVN+MOVK+ALU, **3 A64 instructions each**, just like armrx pre-W4. There is no NEON pool, no prologue `ldr qN` in `randomx_calc_dataset_item_aarch64` on either side.

The NEON pool exists only in the **main-VM program path** (reference `jit_compiler_a64.cpp:139,230` set `num32bitLiterals = 0`; pool slots live at `ImulRcpLiteralsEnd`, loaded into v0–v15 by the main-program prologue's 16× `ldr qN, literal_vN` — this is the "~line 221 ldr qN" you saw, but it's the **main-VM prologue, executed 8×/hash**, not the dataset-item prologue, executed 16,384×/hash). **armrx already has full parity here**: `src/jit_compiler_a64.cpp:780` resets `num32bitLiterals = 0` in `generateProgram`, and armrx's own `.S:206–221` loads v0–v15 identically.

Moreover, a NEON pool in the superscalar function is **register-infeasible by construction**, which is almost certainly why the reference pins it off too: across the 2,048-iteration main loop, **all 32 vector registers are live** — v0–v15 = the main-VM literal pool itself (UMOV/SMOV consumers in the program body every iteration), v16–v23 = f/e registers, v24–v27 = a-regs (`ldp q24,q25/q26,q27` at `.S:169–170`), v28 = AES zero-key, v29–v31 = spMix masks (`.S:174–183,300–303`). A `bl rx_calc_dataset_item` that clobbered any of them would silently corrupt the main VM. Save/restore + pool reload per call would cost ~0.75M instr/hash + 0.26M loads/hash to save **zero** instructions (LDR and UMOV are both 1 instruction) — negative ROI by construction. This closes deliverable (a): **the register-pool C approach is not the reference's advantage, and W4's inline LDR pool is the correct terminal design for superscalar C\*.**

### 0.2 W4's true saving is ~11.7M instr/hash (~9.8%), not ~0.8% — the instruction gap is already ~closed

The quoted "703 pooled ops × ~1.5 ≈ 1,050 instr/hash" misses the ×16,384 execution multiplier (703 is the static per-compile count; the function runs 16,384×/hash). Verified decisively on the host (qemu, `armrx --jit-dump`, seed default, a1ea83c):

```
Superscalar/dataset-derivation dump: Total instructions: 3563  Total bytes: 18064
  IADD_C7 135 @8B | IXOR_C7 137 @8B | IADD_C8 92 @8B | IXOR_C8 128 @8B | IADD_C9 100 @8B | IXOR_C9 122 @8B
```

Every one of the **714 C\* ops is pooled at 8 bytes (LDR+ALU)** vs the pre-W4 12 bytes (w23's 20,916 B/call for the same 3,563-op shape). Saving = 713 words/call × 16,384 calls = **11.68M instr/hash ≈ 9.8% of the 118.96M clean total**. This also means:

| | instr/hash (clean) |
|---|---:|
| armrx pre-W4 (W11 census) | 118.96M |
| **armrx post-W4 (derived: 118.96 − 11.68)** | **≈ 107.3M** |
| BSD reference (W1-4) | 104.8M |
| **residual instruction gap** | **≈ +2.4%** |

The "132.7 → 132.6M" figure is an artifact: W14's window was ungated (whole process ÷ 500 hashes), so it carries ~13.7M/hash of init contamination (Argon2 256 MiB fill + superscalar JIT compile + ramp) that swamps a real delta at ±1M noise. The dump arithmetic above is exact and the W11 census model validated this attribution method to ≪1%. The user's own H/s data corroborates a large real saving: ~+1–3% H/s at ~−9.8% instructions implies IPC fell ~7% (C\* LDR latency partially exposed) — i.e., W4 traded instruction count for latency roughly 3:1, which is a *good* trade, and is this project's 4th confirmation that instruction count and cycles decouple on this core.

### 0.3 The real remaining structural delta is hardware AES — and the "AESE incompatible" verdict is a misdiagnosis

The reference ran W1-4 with `RANDOMX_FLAG_HARD_AES` (default; `--softAes` not passed; built `-march=armv8-a+crypto`; `randomx.cpp:58`). Its scratchpad fill/hash uses the AArch64 crypto path (`intrin_portable.h:476–484`):

```c
rx_aesenc_vec_i128(a, key) = vaesmcq_u8(vaeseq_u8(a, zero)) ^ key;   // AESE with ZERO key → AESMC → EOR real key
rx_aesdec_vec_i128(a, key) = vaesimcq_u8(vaesdq_u8(a, zero)) ^ key;
```

This is precisely the "compensating transformation" the armrx postmortem (`docs/postmortems/aes-ttable-bug-postmortem.md`) named and then dismissed: AESE applies AddRoundKey at the *start*, so you feed it a **zero key** (making it a no-op), let AESMC do MixColumns, and apply the real key with a trailing EOR — reproducing the standard RandomX round order (AddRoundKey at end) exactly. The upstream `--verify` run on this exact device in W1-4 validated hashes through this path. **armrx's own template already contains this exact construction**: `src/jit_compiler_a64_static.S:425–489` (the v2 FE_mix hard-AES block: `aese vN, v28` + `aesmc` + `eor vN, vN, key` with `movi v28.4s, 0` emitted by `emitV2AesTweak`, `jit_compiler_a64.cpp:212`). The historical attempt must have used `aese(state, key)` directly (AddRoundKey-first = wrong) and the correct conclusion — "AESE is incompatible *unless zero-keyed*" — was recorded as "incompatible" full stop.

The cost of the misdiagnosis, from the W11 census: armrx's AES lives in C++ T-tables (`hash_aes_1r_x4` + `fill_aes_1r_x4` + `hash_and_fill*`, bucket 4) at **~10.7M instr/hash (9.0%), ~11.3M cycles/hash, IPC ~1.0**. Per hash = 2 full 2 MiB passes (fill via `init_scratchpad`, hash via `get_final_result`; `vm.cpp:978,963`) = 65,536 × 64-byte blocks at ~163 instr/block. Hardware AES: 3 NEON ops per 16 B + streaming ldp/stp ≈ ~17 instr/block → **~1.1M instr/hash. Saving ≈ 9.6M instr (8.1%) and ~8M cycles (≈5% of 160.4M) per hash**, plus ~4.2M T-table loads/hash removed from L1D.

---

## 1. Where the gap actually is now (decomposition)

| Region | armrx post-W4 (derived) | reference (est., same-spec) | gap |
|---|---:|---:|---:|
| Superscalar region (dataset derivation) | ~84.1M (C\* at 2 instr) | ~95.8M (C\* at 3 instr) | **armrx 12% leaner** |
| AES fill/hash (2× 2 MiB passes) | 10.7M (T-table) | ~1.1–1.8M (hardware) | **+9M — the gap** |
| Main-VM JIT region (256-op programs + glue) | 11.79M | ~10–11M (NEON-pool immediates both sides; minor per-op deltas, CBRANCH strategy) | ~1–2M |
| Blake2b / JIT-compile / glue | ~0.7M | ~0.7M | parity |
| **Total** | **≈ 107.3M** | **104.8M** | **≈ +2.4%** |

The +26.6% figure decomposes as: ~13.7M ungated-protocol contamination (armrx side) + ~9M AES + ~1–2M main-VM emission detail, *minus* the ~11.7M W4 already banked. **There is no remaining superscalar density gap — post-W4 armrx is leaner than the reference in the dominant region.**

IPC: the 1.369 vs 1.540 comparison is largely (i) the same ungated contamination (init phases are high-IPC) and (ii) region mix (the reference's tiny hardware-AES region vs armrx's 10.7M-instr IPC-1.0 T-table region). On gated numbers armrx is 0.742; after hardware AES armrx's *cycle-weighted* mix improves even though average IPC will arithmetically *drop* (removing an above-average-IPC region) — IPC is the wrong target; cycles are.

---

## 2. Roadmap, ranked by expected ROI on the in-order A53

### Item 0 — Clean gated re-baseline (near-zero cost, do first; diagnostic)
Re-run `bench_armrx --full-hash-only --perf-ready` on-device post-W4 with the W11 protocol (`perf stat -e cycles,instructions`, 500-hash gated window, taskset/core-3). Expected: **~107M instr/hash**, cycles ~152–158M, superscalar-region IPC tells you the C\*-LDR exposure. Also re-run the reference with a symmetric protocol (same gated attachment, or at minimum identical nonce counts both sides). This replaces the corrupted 132.7/104.8 comparison with the true residual (est. +2–5% instr, ~0–8% cycles) and gates everything below.

### Item 1 — Hardware AES for all scratchpad fill/hash paths (THE lever; est. −8–9.6M instr, −5–8M cycles, +4–6% single-core H/s)
Adopt the upstream zero-key decomposition in `aes_hash.cpp`/`aes_generator.cpp`. Correctness pattern is triple-proven (upstream on this silicon in W1-4; armrx's own `.S:425–489`; FIPS-197 KAT harness exists). Full plan in §3. This is projected to be the largest code-level win in project history (previous best +0.885% IPC), roughly 1.5–2× Track G — and it *replaces* Track G on crypto silicon. It is also the only remaining item that is both multi-percent and low-risk, because it touches no JIT emission, no scheduler, no memory layout — only four self-contained C++ functions with byte-exact oracles.

### Item 2 — (conditional, small) C\* LDR latency rotation
Only if Item 0 shows superscalar-region IPC materially below the 0.800 census figure. The C\* LDR→ALU pair is emitted adjacently, so L1 load-use latency (~3–4 cyc) can be exposed ~714×/call. Fix shape: software-pipeline the constant load — emit C\* op N+1's LDR (into the alternate temp: IXOR uses x12, IADD uses x13) alongside op N, one C\* op ahead in `emit_order`. Watch IMUL_RCP (shares x12). Expected +0.5–1.5% cycles if exposed, null otherwise. Gate behind the superscalar scheduler stress test; do not touch `*_M` hazard rules (W3-2 stands).

### Item 3 — (diagnostic, low value) main-VM per-op emission diff vs reference
~1–2M instr/hash. Candidates: armrx's branchless CBRANCH (~4–5 instr, adopted Jul 17) vs the reference's branchy form (~2–3 instr at ~2.4% real miss rate — the original CSEL revert measured a *regression*, so this is likely already correct to keep); per-opcode emission table diff. Do only after Item 1 lands, and only if Item 0 shows a residual worth chasing.

### Explicitly closed (do not reopen)
- **Superscalar NEON register pool** — register-infeasible (all 32 v-regs live across the calling loop; §0.1). The reference doesn't do it either.
- **Superscalar density** — post-W4 the body is at the A64 ISA floor: 1 instr/op except constant-carrying ops at 2 (1 load + 1 ALU; IMUL_RCP's 64-bit reciprocal cannot be an operand). w23's proposal list is exhausted.
- **Memory-op scheduler extension** (W3-2: deterministic divergence), **PRFM** (T2-1), **dual-issue padding** (T2-2), **Track C wrapper** (≤0.5%), **CBRANCH CSEL**, **D1/D3 interleaves**, **PGO**.
- **Main-VM 2.2× IPC penalty** — 94% architectural; the reference pays the same penalty (it's the RandomX light-mode memory-latency signature), so it is not a *competitive* gap.

---

## 3. Implementation plan — Item 1 (hardware AES)

### Design
For each of the four fill/hash loops, add an `__aarch64__ && __ARM_FEATURE_CRYPTO` branch *above* the existing Track-G branch, keeping state in `uint8x16_t` registers across the whole loop (the current Track-G path round-trips `AesBlock` through memory every block — that goes away too):

```c
// include/armrx/aes.hpp (new inline helpers, guarded)
static inline uint8x16_t aes_enc_round_hw(uint8x16_t s, uint8x16_t key) {
    const uint8x16_t z = vdupq_n_u8(0);
    return veorq_u8(vaesmcq_u8(vaeseq_u8(s, z)), key);
}
static inline uint8x16_t aes_dec_round_hw(uint8x16_t s, uint8x16_t key) {
    const uint8x16_t z = vdupq_n_u8(0);
    return veorq_u8(vaesimcq_u8(vaesdq_u8(s, z)), key);
}
```

Semantics check against the spec: `aes_encrypt_round` = SubBytes→ShiftRows→MixColumns→AddRoundKey = `aes_enc_round_hw` exactly (upstream `intrin_portable.h:476` is the authority); same keys, same enc/dec lane assignment as the existing T-table calls (s0/s2 decrypt, s1/s3 encrypt for fill; mirrored for hash — copy the existing call sites' pairing verbatim, only swap the primitive).

### Files / functions
| File | Function | Notes |
|---|---|---|
| `include/armrx/aes.hpp` | add `aes_enc_round_hw`/`aes_dec_round_hw` | guarded `#if defined(__aarch64__) && defined(__ARM_FEATURE_CRYPTO)` |
| `src/aes_hash.cpp` | `fill_aes_1r_x4`, `fill_aes_4r_x4`, `hash_aes_1r_x4`, `hash_and_fill_aes_1r_x4`, `hash_and_fill_aes_interleaved_x4` | per-hash: hash+hash_and_fill (2 MiB each); per-seed: fills; mining: interleaved |
| `src/aes_generator.cpp` | `AesGenerator1R/4R::next` | minor (~0.4M) but same pattern |
| `tools/aes_kat_check.cpp` | extend: hw path vs existing T-table path, byte-identical over ≥10k random blocks + FIPS-197 vector | mechanical oracle, no spec reasoning needed |
| `CMakeLists.txt` | none | `-march=armv8-a+crypto` already on `armrx_core` (line 67) |

Keep the T-table path for non-crypto/x86 builds (host test suite must keep passing on x86_64) and keep Track G under its existing macro as a documented fallback. No JIT, no scheduler, no `.S`, no vm.cpp call-site changes.

### Gate / test plan (host → device)
1. **Host x86_64:** full `ctest` — must be 100% unchanged (the hw branch is compiled out).
2. **Cross-build → device, correctness (fast gates first):** `aes_kat_check` (hw==T-table bytes), `test_aes_hash` (**golden pins unchanged — do not edit expected values**), `test_blake2b` (spec KATs), `test_mining` (end-to-end known-hash KATs), `test_jit_equivalence` 16/16, `test_jit_determinism`, `test_jit_dataset_2way`.
   ⚠️ **Critical caveat:** the differential tests (JIT vs interpreter) will NOT catch a broken AES — both sides share `aes_hash.cpp` and would be wrong *together*. The golden-pin/known-hash tests are the only real oracles; plus one **cross-implementation KAT**: hash the upstream benchmark's fixed blockTemplate and compare against its published v1 vector (`10b649a3f15c…cfa1`, `benchmark.cpp:404`) — or simpler, mine one share on-device (share validation is the ultimate oracle, already exercised).
3. **Perf A/B (device, T2-2 harness discipline):** B-M-B-M, `taskset -c 3 perf stat -e cycles,instructions` on `bench_armrx --full-hash-only --perf-ready`, per-run md5 of the executed binary. Adopt if cycles improve ≥2% in both modified runs (expect ~5%). The slow scheduler stress tests are orthogonal (no scheduler contact) — human re-runs tomorrow as already planned.

### Risks
- **R1 (correctness, LOW):** AESD/AESIMC lane pairing wrong → caught by `aes_kat_check` hw-vs-T-table byte comparison and `test_aes_hash` golden pins, deterministically, before any mining. Upstream's `--verify` pass on this exact silicon in W1-4 de-risks the silicon itself.
- **R2 (perf, LOW):** hardware AES somehow not faster → fallback is one `#if` away; Track G code retained.
- **R3 (host drift, LOW):** guarded branch; x86_64 build untouched.
- **R4 (institutional, MEDIUM):** the repo's own docs (AGENTS.md, postmortem) currently assert AESE incompatibility as fact. If adopted, those docs must be amended — the postmortem's own "not interchangeable *without compensating transformations*" sentence is the hook: the compensation is the zero key, and armrx's `.S:425–489` already embodies it.
- **R5 (measurement, LOW):** ungated windows will hide the win (Item 0 first).

---

## 4. Honest ceiling assessment

**The residual gap is not architectural — it is closable, and mostly already closed.**

- The dominant region (superscalar, ~88% of instructions) is instruction-identical by construction between armrx and the reference (same 14-opcode switch, same emission); post-W4 armrx is **leaner** there than the reference. There is no codegen ceiling in that region — armrx already beats the BSD reference's emission density, with a scheduler the reference lacks.
- The only remaining multi-percent item is hardware AES — a *porting* gap, not an architecture gap. After Item 1: **armrx ≈ 97–98M instr/hash** vs reference 104.8M and XMRig 94.5M (W1-4) — armrx lands *between* the two, ~+3% vs XMRig instructions, with projected single-core ~5.0–5.2 H/s vs XMRig's 5.05 — **parity within noise**.
- What genuinely is architectural (in-order A53, 765 MHz, light-mode memory latency) caps *absolute* H/s at ~5–5.5/core and applies identically to every implementation — including XMRig, which reaches the same wall from the other side (its 94.5M instr at 0.648 IPC lands at 5.05 H/s on the same silicon).
- Beyond parity: XMRig's residual edge is years of micro-tuning (custom Blake2b, batch-mode fusion, pipeline-specific tricks) worth maybe 1–3% — diminishing returns, not a ceiling. The two-cluster interconnect penalty (~50% on cores 4–7 under contention) remains the largest *system-level* gap and is untouched by any of this.

**Bottom line:** pursue Item 0 (re-baseline, ~30 min) then Item 1 (hardware AES, ~200 lines, low risk, est. +4–6%). Everything else is either closed on evidence, register-infeasible, or sub-percent. The reference's 1.54 IPC is not a meaningful target — cycles are — and on cycles, armrx post-Item-1 should meet or beat the reference per-core on this device.

---

