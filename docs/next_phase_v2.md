# armrx — Next-Phase Improvement & Evolution Plan (v2)

> **v2 status: post-review.** This is a refinement of `docs/next_phase.md` incorporating
> feedback from an independent review. The structural changes vs. v1 are:
>
> 1. **`generateProgram`/`generateProgramLight` dedup moved from Phase 2.2 → Phase 1.6.**
>    The duplicated 24-line v2 AES-tweak block is a silent correctness risk on v2-mode
>    programs and is cheaper to fix than to debug.
> 2. **JIT encapsulation (privatize `code`/`emit32`) deferred to Phase 3.** It conflicts
>    with the active peephole perf work in Phase 2 and would create merge conflicts on
>    every instruction handler.
> 3. **PGO added as a Phase 2 "free uplift" item** with the GCC 15 + musl `__gcov_*`
>    crash as the unblock task. Was missing from v1 despite being in the ROADMAP.
> 4. **`last_mode` race reclassified** — keep the fix, but impact is an extra
>    `fesetround` per mode switch, not a crash. No longer a Phase 1 blocker; bundled
>    with the other concurrency fixes for cleanliness.
> 5. **ARMV8A encoding consolidation → Phase 3** (zero correctness impact today).
> 6. **`ceil_*` deletion folded into general debt** rather than taking a Phase 1 slot.
> 7. **`blake2b` NEON XOR change gated behind measurement** — don't commit without
>    `perf stat` proof the compiler isn't already CSE'ing it.
> 8. **`kRandOMXFlag` rename dropped** — cosmetic churn across every call site. Keep
>    only a documenting comment about the value divergence from upstream.
> 9. **§2.1 JIT encapsulation claim corrected against source (post-review verification).**
>    `code`, `emit32`, and `emit64` are already `private` in the current
>    `jit_compiler_a64.hpp` — they are not the globally-reachable leak v1/v2 originally
>    described. The only remaining public leak is the unused `getCode()` accessor. The
>    Phase 3 refactor scope in §2.1 and P3.1 has been shrunk accordingly.
>
> See **Appendix A** for the full review-delta rationale.

---

## 1. Current State Assessment & Differential Analysis

### 1.1 What is solid (do not break)

Cross-checked `ROADMAP.md` against the actual source. The completed claims hold:

- **Crypto core** — `src/blake2b.cpp` (NEON + scalar), `src/argon2.cpp` (NEON G-function via `vmull_u32`+`vmovn_u64`), `src/aes_hash.cpp` (`vaeseq`/`vaesmcq` chains) all produce reference KAT vectors. NEON paths are idiomatic, not hand-rolled ASM. `tests/test_blake2b.cpp:194,200` asserts both KAT inputs in interpreted mode and lines `209,213` re-assert under `#ifdef ARMRX_HAVE_JIT`.
- **JIT backend** — `src/jit_compiler_a64.cpp` uses a frequency-weighted 256-entry dispatch table (`engine[256]`, header line 73) with `REPN(..., WT(x))`. Static template `jit_compiler_a64_static.S` reserves 6144 slots. W^X is honored via `setPagesRW→emit→setPagesRX` unless `rwx_` is set.
- **Dispatch-table refactor (P1)** — `kCompileHandlers[256]` in `vm.cpp:539–612` plus a clean enum switch in `execute_bytecode` (`vm.cpp:621–758`). Two-table design (raw opcode → bytecode → enum) is sound.
- **Branchless CBRANCH (O11)** — `jit_compiler_a64.cpp:1156–1174` deploys the `bne .Lskip; b target` form with `imm19=2`. The postmortem in `docs/branchless-cbranch.md` correctly identifies the prior hang as an off-by-one in the B.cond immediate.
- **Pool/network surface area** exists end-to-end: subscribe/authorize/notify/submit in `stratum_client.cpp`, CryptoNote + Stratum V1 fallback, exponential backoff, multi-pool failover in `pool_manager.cpp`.
- **Tooling** — `bench_armrx.cpp` registered in CTest; ASan/UBSan/PGO/LTO CMake options all wired in `CMakeLists.txt:7–13, 104–136`; `.clang-format` / `.clang-tidy` present.

### 1.2 ROADMAP items — current relevance

| ROADMAP item | Status | Relevance now |
|---|---|---|
| **S7 — `emit32` UB** | Marked ✅ (memcpy fix) | **Done and verified** at `jit_compiler_a64.hpp:81–91`. Close it. |
| **S8 — Dangling pointer contract** | Marked ⏸️ (docs deferred) | Still **relevant** — see §2.1, the `getCode()` encapsulation gap is the dangling-pointer entry point. Deferred to Phase 3 with the rest of the JIT encapsulation. |
| **O12 — Newton-Raphson FDIV/FSQRT** | Marked ⏸️ Frozen (segfault) | **Correctly frozen.** `OPTIMIZATION_REFERENCE.md:46` documents x29 corruption on Cortex-A53 in-order. Keep frozen; the `h_FDIV_M` Markstein loop at `jit_compiler_a64.cpp:1045–1079` emits **17 instructions** that a different optimization (drop iterations 2–3) can shrink without touching NR. |
| **P3 — Peephole JIT coalescing** | 🔴 Remaining | **Primary performance lever.** 33% instruction-count gap is the whole story. Plan in `docs/peephole-jit-plan.md` is sound — Phase 1.2 frequency data is the prerequisite. |
| **PGO** | Not listed in ROADMAP "remaining", but `OPTIMIZATION_REFERENCE.md:47` documents a blocker | **Missing from v1, added here as Phase 2.6.** Could give a "free" 5–10% uplift without instruction-level work once the GCC 15 + musl `__gcov_*` linker crash is solved. |
| **Stratum V2** | Remaining | **Low priority.** No Monero pool requires it today; existing Stratum V1 / CryptoNote stack works. Defer until a target pool mandates it. |
| **Prometheus endpoint** | Remaining | **Medium priority** — but should be blocked *behind* the structured-logger refactor (§2.5), otherwise it duplicates the ad-hoc `std::cerr` problem in a second sink. |
| **Cross-compile CI** | Remaining | **Medium** — only valuable if paired with the determinism test from `peephole-jit-plan.md:1.5`. |

### 1.3 Foundations that are weaker than the ROADMAP implies

Three claims in the README/ROADMAP do not survive scrutiny against source:

1. **"JSON injection protection ✅"** — TX-side is *mostly* protected (`armrx::json::escape` on wallet/password/job_id), but `session_id_` is concatenated **unescaped** at `stratum_client.cpp:315` and `:683`. A hostile pool returning a crafted `id` field injects into the submit frame. This is a regression of the S1 fix.
2. **"Auto-reconnect with backoff ✅"** — The backoff works, but `PoolManager::stratum_` is **read by worker threads** (`pool_manager.cpp:85`) **while the main thread reassigns it** during failover (`:51`). This is a use-after-free waiting to happen, not a finished feature.
3. **"TLS peer verification ✅"** — Chain verification is on (`SSL_VERIFY_PEER`), but **hostname verification is not** (`tls_client.cpp:59–61` sets SNI only). Any CA-signed cert for any domain MITMs the connection. `--no-verify-tls` is currently a no-op in practice.

These belong in Phase 1 below.

---

## 2. Advanced Code Architecture & Refactoring (Next-Gen)

> **Refactoring philosophy for v2:** structure changes that touch the JIT instruction
> handlers are deferred until *after* the peephole perf program (Phase 2.1) lands, to
> avoid merge conflicts during active perf iteration. Pure-correctness dedup that is
> orthogonal to instruction emission (the v2 AES-tweak block) stays in Phase 1.

### 2.1 JIT compiler encapsulation (DEFERRED to Phase 3)

**Corrected against source (v1/v2 both mis-stated this — see Appendix A.2).** The original claim was that `code`, `emit32`, and `emit64` were all public and globally reachable. Re-checking `include/armrx/jit_compiler_a64.hpp:50–91` against the current source shows that's only half true:

```cpp
class JitCompilerA64 {
public:
    ...
    ProgramFunc* getProgramFunc() { return reinterpret_cast<ProgramFunc*>(code); } // used once: vm.cpp:842
    uint8_t*     getCode() { return code; }                                        // public, but zero callers in src/
    ...
private:
    uint8_t* code;                                                                  // already private
    static void emit32(uint32_t val, uint8_t* code, uint32_t& codePos) { ... }      // already private
    static void emit64(uint64_t val, uint8_t* code, uint32_t& codePos) { ... }      // already private
    ...
};
```

`code`, `emit32`, and `emit64` are **already private**. `emit32`/`emit64` are called unqualified from inside instruction-handler bodies, which is legal because they're private members of the same class — not because they're globally reachable. The actual remaining leak is narrower: `getCode()` is a public accessor that hands back the raw mutable `uint8_t*` to the executable-memory buffer, bypassing the `enableWriting()`/`enableExecution()` W^X discipline. Grepping `src/` for `getCode()` turns up zero call sites — it's dead code today, not an active leak. `getProgramFunc()` is the one legitimate public accessor (it's the callable entry point, used once at `vm.cpp:842` to invoke the compiled program) and is fine as-is.

**Deferred because:** even with the smaller scope, `emit32`/`emit64` are still called unqualified from ~40+ instruction-handler bodies. Moving them into the anonymous namespace of the `.cpp` is mechanically low-risk (unqualified name lookup still resolves to a free function in the enclosing translation unit, so call sites don't need to change), but it still touches `jit_compiler_a64.cpp` while Phase 2.1's peephole program is actively rewriting the same handler bodies. Land the peephole work first to avoid diff noise, even though this refactor is now smaller than originally scoped.

When Phase 3 arrives, the refactor is:
- Delete `getCode()` (or replace with a read-only `std::span<const std::byte> code() const noexcept` for inspection) — it has no current callers and exists only as a dormant capability leak.
- Move `emit32`/`emit64` into the anonymous namespace of the `.cpp` — mechanical; no handler-body call sites need to change.
- Wrap the `getProgramFunc()` pointer in an `ExecutableRegion` RAII type whose destructor calls `setPagesRX` — this makes W^X a property of the type system, not a discipline.

### 2.2 Deduplicate `generateProgram` / `generateProgramLight` (Phase 1.6 — moved up)

`jit_compiler_a64.cpp:171–259` and `:261–355` share ~70% of their body byte-for-byte: the opcode dispatch loop, the `ubfx x19/x20` scratchpad-mix emits, the entire v2 AES-tweak block (`:219–242` ≡ `:317–340`), and the `v2_FE_mix` memcpy. Extract:

```cpp
// private helpers in jit_compiler_a64.cpp
uint32_t emitPrologueMix(ProgramConfiguration const& cfg, uint32_t codePos);
void emitV2AesTweak(uint32_t& codePos);   // ~24 lines, currently duplicated
void emitSpMix2(uint32_t& codePos);       // the eor at 191/281
```

**Why this moved from Phase 2 → Phase 1 (per review):** the v2 AES-tweak block is a 24-line sequence of raw `emit32` calls duplicated verbatim across the two generate paths. Silent drift there produces wrong hashes *only* on v2-mode programs — a bug class that is cheap to fix once and expensive to debug after the fact. The dedup is orthogonal to instruction-handler bodies (it touches the prologue/epilogue only), so it does not conflict with the peephole work. It is the single highest-leverage maintainability change in the JIT and it carries a correctness argument, so it goes in Phase 1.

### 2.3 Consolidate `ARMV8A::` encodings (DEFERRED to Phase 3)

The `ARMV8A` namespace (`jit_compiler_a64.cpp:80–104`) covers ~25 mnemonics. The other ~15 raw hex opcodes (`0x121A0000` and-immediate, `0xD3400000` ubfx, `0xf8606840` ldr-reg, `0x0C407800` ld1, `0x6E004400` rev64, `0x54000000 | (2 << 5) | 1` B.cond) are inlined as bare literals at the call sites.

**Deferred because (per review):** zero correctness impact today. Every literal is working correctly. This is pure maintainability and belongs in the Phase 3 "long-term sustainability" bucket alongside the JIT encapsulation, not blocking Phase 1/2. When done, extend `ARMV8A` with `UBFX`, `AND_IMM`, `LDR_REG`, `LD1`, `BFI`, `B_COND(cond, imm19)`, `MSR_FPCR`, `RBIT` so the branchless-CBRANCH `static_assert` at `:1153` (which currently hard-codes `0xF2781C1F`) can derive the `tst` encoding symbolically and actually validate it.

### 2.4 Fix the misleading VM-side state (selected items only)

Of the six v1 items, two are deferred/dropped per review and the rest stay:

| Item | Action | Rationale |
|---|---|---|
| **`rx_set_rounding_mode` static cache** (`vm.cpp:70`) | Make per-instance member. | Real UB but **low-impact** (review correctly notes: effect is an extra `fesetround` per mode switch, not a crash). Bundled with the other concurrency fixes in Phase 1.2 for cleanliness — not a blocker on its own. |
| **Dead `ceil_*` constants** (`vm.cpp:113–142`) | Delete. | Trivial. Folded into general Phase 1.4 debt cleanup — not its own slot. |
| **`register_usage_[8] = {-1}`** (`vm.hpp:171`) | **No change (per review).** | Review is correct: `compile_program` at `vm.cpp:615` runs `std::fill(begin, end, -1)` before any CBRANCH, so the misleading initializer has zero runtime impact. Leave it; not worth a Phase 1 slot. Optional: add a one-line comment noting the `std::fill` makes the initializer moot. |
| **`VirtualMachine::allocate()`** (`vm.cpp:210–212`) | Delete or fix the stale `std::vector` comment. | Trivial. Phase 1.4. |
| **Wasted work in JIT path** (`vm.cpp:234–241` then overwritten at `:840`) | Gate the first init behind `if (!jit_)`. | Phase 2.4 — pure perf, not correctness. |
| **`kRandOMXFlag*` typo** (`vm.hpp:53–56`) | **Add a comment only; do NOT rename (per review).** | The rename churns every call site (e.g. `bench_armrx.cpp:94`, `vm.cpp:150–158`, `test_blake2b.cpp:185,204`) for cosmetic gain. The real issue is the value divergence from upstream (`Jit=4` vs upstream `FULL_MEM=4`) — document that, leave the names. |

### 2.5 Structured logger (cross-cutting enabler)

There is no logger. `src/stratum_client.cpp:427` writes `std::cout` from the **reader thread**, `src/tui.cpp:47–86` writes `std::cout` from the **main thread** at 1 Hz, and `src/main.cpp:379–380` still ships a `[DEBUG] Connecting to pool...` line in production. Under `--tui` these interleave and corrupt the ANSI cursor math (`tui.cpp:83–86` uses `prev_lines_` to walk the cursor up; stray lines permanently break the layout).

This is also a prerequisite for the Prometheus endpoint on the roadmap — otherwise metrics become a second ad-hoc sink.

Action: introduce `include/armrx/log.hpp` with levels (`trace/debug/info/warn/error`), a single `std::mutex`-guarded sink, and a TUI-mode switch that suppresses console output (routing to a ring buffer the TUI reads from). Replace every `std::cerr <<`/`std::cout <<` in `stratum_client.cpp`, `tls_client.cpp`, `pool_manager.cpp`, `mining_engine.cpp`.

---

## 3. Deep Performance, Edge-Case & Resource Optimization

### 3.1 The headline opportunity: instruction-count gap (P3)

From `OPTIMIZATION_REFERENCE.md:67–73`:

| Metric | armrx | XMRig | Gap |
|---|---|---|---|
| Instructions | 64.3B | 48.2B | **+33%** |
| Cycles | 78.5B | 75.0B | +5% |
| Branch misses | 152M | 11M | **+13×** |

Cycles are within 5% — the CPU is doing comparable work. The gap is *pure emitted-code volume* plus branch misprediction. The path forward is `docs/peephole-jit-plan.md`, with the highest-leverage specific targets already identified by the audit:

- **`h_FDIV_M` Markstein loop** (`jit_compiler_a64.cpp:1045–1079`) emits **17 instructions** with 3 NR-style iteration pairs. RandomX does not require IEEE-correct rounding on FDIV_M. If the KAT suite passes with 2 iterations, **drop the third pair → −8 instructions per FDIV_M, 4× per program = −32 instructions/program**. This is a single-opcode win bigger than most of Phase 2 combined. **Must** be KAT-vetoed per the plan's principle #1.
- **`h_IMUL_RCP` immediate materialization** (`jit_compiler_a64.cpp:500–526`): `emitMovImmediate` long-path emits `movz`+`movk` for any imm ≥ 2¹⁶, but `movz` with `hw=0..3` encodes any single-halfword immediate in one instruction. Add the single-halfword fast path.
- **`emitMemLoad` mask redundancy** (`:567–598`): `emitAddImmediate` may emit up to 3 instructions, then the result is AND-masked. Fold the AND into the addressing when the add fits the low 12 bits.
- **`h_IROR_R` / `h_IROL_R`** (`:901–955`): armrx uses `rotr`/`rotl`; `extr` (rotate-insert) may save an instruction and is the form XMRig uses for these.

**Branch misses (+13×)** is the second-order problem. The branchless CBRANCH fix addressed one site; the BTB-aliasing caveat in `docs/branchless-cbranch.md:71–79` (per-program JIT regeneration means fixed code addresses see rotating branch behavior) is the larger contributor and has no cheap fix — it's a property of regenerating the JIT buffer per seed. Worth measuring with `perf stat -e branch-misses` after Phase 1.1 lands.

### 3.2 Memory: huge pages, cold-start faults, and mprotect visibility

- **No `MAP_HUGETLB` anywhere it matters.** `Argon2dCache` (`argon2.cpp:242–247`), `MappedMemory` (`mining_engine.hpp:28–31`), and the VM scratchpad (`vm.cpp:167–174`) all use plain `MAP_ANONYMOUS` + `madvise(MADV_HUGEPAGE)`. This depends on THP being enabled (`/sys/kernel/mm/transparent_hugepage/enabled`), which is not guaranteed on mining rigs. `virtual_memory.c::allocLargePagesMemory` (`:206`, uses `MAP_HUGETLB | MAP_POPULATE`) exists but is **not called by any of these callers**. For the 2 GiB dataset and 256 MiB cache this is the biggest available TLB win. Wire it with fallback.
- **Scratchpad cold-start faults**: `vm.cpp:167–174` mmaps the 2 MiB scratchpad with no `MAP_POPULATE`. First iteration faults in 2 MiB pages. Add `MADV_POPULATE_WRITE` (Linux 5.14+) on a warmup hook, or one-time `memset`.
- **`mprotect` failures are silently swallowed** (`virtual_memory.c:172–199`): `setPagesRW`/`setPagesRX` are `void` and discard the `pageProtect` errno. For a W^X-hardened JIT this is a security hole — a failed RX transition leaves pages RW or RWX invisibly. Make these return `int` (like `setPagesRWX` does) and have the JIT throw on failure.

### 3.3 Per-hash hot-path cost reductions

- **Per-iteration full vector copy** (`mining_engine.cpp:296`): `block_input = local_job.block_template;` copies the entire blob on every hash. The comment claims "only reallocates on job change" — that's wrong; `operator=` copies contents even when capacity is sufficient. For 76-byte Monero blobs it's cheap, but the fix is free: `resize`+`memcpy` once per job, then `std::memcpy` only the nonce bytes per hash. This also closes the silent-swallow bug at `:337–344` (a bad `nonce_offset` currently leaves the worker hashing an unmodified template forever).
- **`generate_superscalar` heap churn** (`superscalar.cpp:428,441`): allocates `std::vector<int> availableRegisters` per `selectRegister`/`selectDestination` call. Cache-init only, but 8 programs × N selections each. Replace with `std::array<int,8>` + count.
- **`blake2b` NEON round XOR redundancy** (`blake2b.cpp:92,94,96,98` etc.): `veorq_u64(vd, va)` appears to be computed twice per round. **Gate behind measurement (per review):** GCC/Clang may already CSE this. Run `perf stat` on `bench_armrx`'s blake2b micro-bench before committing a change — do not commit on a guess. If the disassembly shows redundant XORs, introduce a temporary.
- **`AesGenerator4R::next()` misses hardware AES** (`aes_generator.cpp:86–93`): always uses the T-table round functions from `aes.cpp`, even though `aes_hash.cpp::fill_aes_4r_x4` implements the same algorithm with `vaeseq`/`vaesdq`. 16 scalar round calls per `next()`. Check call sites — if it's only used at VM init, low priority; if it touches the hash path, high priority.
- **`blake2b` overload duplication** (`:217–238` vs `:240–261`): the two public `blake2b` overloads share everything except the output write. Factor a shared core.

### 3.4 Edge-case bugs in parsing/IO

These are not perf, but they are hot-path correctness:

- **`json::get_array_first` dead-code bug** (`json.cpp:126–129`): the first `if (json[pos] == '"')` always returns with a 512-byte-truncated input and a comment admitting it's "hacky but works." The correct second `if` (`:131`) is unreachable. This feeds `mining.set_target`, `mining.set_difficulty`, `mining.set_extranonce`. A target string near a 512-byte boundary is silently mis-parsed → wrong difficulty → rejected shares or **false share accepts**. Delete the first branch.
- **`json::find_key` substring matching** (`json.cpp:64–77`): `json.find("\"key\"")` matches the first occurrence anywhere, including inside a string *value*. A pool returning `{"error":"\"method\" bad","method":"login"}` returns garbage for `method`. Scope the search to object-key positions (require the preceding non-ws char to be `{` or `,`).
- **`json::escape` incomplete** (`json.cpp:43–57`): only escapes `\ " \n \r \t`. JSON requires all U+0000–U+001F escaped. A wallet or password with a literal control byte produces invalid JSON.
- **Unbounded `read_buf_`** (`stratum_client.cpp:391`): `read_buf_.append(tmp, n)` with no cap. A malicious pool streaming without `\n` grows the buffer to OOM. Cap at, e.g., 1 MiB and drop the connection.
- **cgroup v1 sentinel heuristic** (`memory.cpp:46`): `if (*limit < (1ULL << 60U))` misclassifies any cgroup limit between 1 EiB and 9.2 EiB. The kernel sentinel is `LLONG_MAX ≈ 2^63`. Use `*limit >= (1ULL << 62)`.

---

## 4. Robustness: Testing, Security & Error Resilience

### 4.1 Concurrency bugs

Per review, these are re-ranked by impact. All are still UB and should be fixed; the ranking determines sequencing within Phase 1.

| Bug | Site | Impact | Fix |
|---|---|---|---|
| **`stratum_` use-after-free** | Worker threads call `pool_mgr->submit_share` → `stratum_->...` (`pool_manager.cpp:85`) while main thread reassigns `stratum_` during failover (`:51`). | **Crash / freed-pointer deref.** Highest impact. | Add `std::mutex stratum_mutex_` to `PoolManager`, or move share submission off worker threads via a lock-free queue that the main thread drains. Phase 1.2. |
| **Handshake/session members raced** | `session_id_`, `extra_nonce1_`, `subscribe_ok_`, `protocol_`, `handshake_req_id_`, `authorize_req_id_` written by reader thread, read by submit/keepalive/main. | Wrong session id / nonce in submit frame under race; intermittent rejected shares. | Make these `std::atomic` (where scalar) or guard with a `session_mutex_`. Phase 1.2. |
| **`reconnect_attempts_` non-atomic** | Plain `unsigned`, written in `reconnect_loop` (`stratum_client.cpp:701`), read by `PoolManager::tick`. | Torn read; benign in practice (worst case one extra/missed retry). | `std::atomic<unsigned>`. Phase 1.2. |
| **`rx_set_rounding_mode` static cache** | `vm.cpp:70` shared across all VMs/threads. | **Low (per review):** effect is an extra `fesetround` per mode switch on cache miss, not a crash — CFROUND is freq-1 and the cache rarely misses. Still UB. | Per-instance member. Phase 1.2 (bundled with the above for a single TSAN-clean checkpoint). |
| **`std::cout` race** | Reader thread + main TUI thread + worker share callbacks. | Corrupted TUI layout under `--tui`. | Resolved by §2.5 logger. Phase 2.3. |

Add a TSAN CI build (`-DARMRX_ENABLE_TSAN=ON`) wired to a short mining+pool integration test.

### 4.2 Security

1. **TLS hostname verification missing** (`tls_client.cpp:59–61`). Add `X509_VERIFY_PARAM_set1_host(param, host.c_str(), host.size())` on the `SSL_CTX`'s param, or post-handshake `X509_check_host(cert, host.c_str(), host.size(), 0, nullptr)`. Without this, `--tls` is encryption-without-authentication — any CA-signed cert MITMs.
2. **Unescaped `session_id_`** in submit (`stratum_client.cpp:315`) and keepalive (`:683`) frames. The S1 fix is incomplete here. Wrap in `armrx::json::escape`.
3. **`SSL_shutdown` on half-initialized SSL** (`tls_client.cpp:85–89`): called even if `SSL_connect` never succeeded. Can spin reading a dead socket. Guard with a handshake-state flag.
4. **`EAGAIN` dead code in TLS** (`tls_client.cpp:104–108, 118–122`): sets `errno = EAGAIN` on `WANT_READ/WRITE` but still returns ≤0, which `write_all` treats as fatal. Currently harmless (sockets are blocking) but is a latent bug if anyone flips non-blocking. Either handle the retry or delete the misleading `errno` set.
5. **JIT W^X bypass default** (`jit_compiler_a64.cpp:148`): `rwx_` stays on for the compiler's lifetime when `setPagesRWX` succeeds, defeating W^X. Document explicitly in the threat model, or make W^X the default and `rwx_` opt-in only under `--unsafe-jit-rwx`.
6. **`randomx_reciprocal` plain `assert`** (`superscalar.cpp:703`) compiles out under `NDEBUG`; a zero divisor would divide by zero in release. Use `ARMRX_ASSERT`.
7. **`update_nonce_in_template` silent swallow** (`mining_engine.cpp:337–344`): on bad `nonce_offset`/`nonce_size`, returns silently, worker hashes an unmodified template forever. Log + deactivate the worker.
8. **CLI numeric args unvalidated** (`main.cpp:136,156,161,176`): `std::stoul`/`std::stoull` throw uncaught → `std::terminate`. Wrap with try/catch + usage message.

### 4.3 `ARMRX_ASSERT` release-mode hole

`assert.hpp:30–37` only *logs* in release and continues. Used at `aes_hash.cpp:74,130,207,281` to guard `output.size() % 64 == 0` — a bad span in release then walks off the end at `:88` (`offset += 64`). Used at `jit_compiler_a64.cpp:185,275` to guard `engine[opcode] != nullptr` — release continues into a null PTMF call. Policy decision needed:

- For **invariant** asserts (PTMF dispatch, divisor≠0): hard abort regardless of `NDEBUG`.
- For **input-validation** asserts (span sizes): replace with `throw std::invalid_argument` matching `aes_generator.cpp:71`.

### 4.4 Testing strategy for next-phase features

The current suite is two files (`test_blake2b.cpp` KAT + `test_mining.cpp` lifecycle, plus `bench_armrx.cpp`). Targets:

- **`tests/test_jit_encodings.cpp`** (peephole plan §1.4) — decode the emitted `bne`/`b` bytes for CBRANCH and assert the computed target == `reg_changed_offset[dst]`. Turns the 120s-timeout failure mode of the imm19 bug into an instant assertion. Add cases for `extr` (IROR/IROL) and any load-pair coalescing.
- **`tests/test_jit_determinism.cpp`** (peephole plan §1.5) — compile the same program twice, assert byte-identical JIT output. Catches non-determinism introduced by peephole passes.
- **`tests/test_json_parser.cpp`** — property tests for the parser bugs in §3.4: crafted `{"error":"\"method\"..."}` input, control-char escape, array-first near 512-byte boundary, oversized line rejection. This is the highest-value test addition because the parser is the untrusted-input boundary.
- **`tests/test_stratum_handshake.cpp`** — HIL (hardware-in-the-loop) against a mock pool: feed canned subscribe/notify/set_difficulty frames, assert correct `session_id_`/`extra_nonce1_`/target extraction. Catches the §4.1 races when run under TSAN.
- **`tests/test_tls_verification.cpp`** — requires OpenSSL; assert that a cert with wrong CN fails handshake, correct CN passes. Catches §4.2 #1.
- **Add `ARMRX_ENABLE_TSAN` to `CMakeLists.txt`** mirroring the ASan/UBSan options at `:126–136`.

---

## 5. The Next Horizon Roadmap (Phased Action Plan)

### Phase 1 — Immediate Stabilization & Debt Clearance (1–2 weeks)

Goal: close correctness regressions and the ROADMAP's open "soft" items. No new features. **No changes to instruction-handler bodies** (those are reserved for Phase 2 peephole work).

**P1.1 Security hardening (close the S1/S6 regressions)**
- `src/tls_client.cpp`: add `X509_VERIFY_PARAM_set1_host` (`§4.2 #1`).
- `src/stratum_client.cpp:315,683`: wrap `session_id_` in `armrx::json::escape` (`§4.2 #2`).
- `src/stratum_client.cpp:391`: cap `read_buf_` at 1 MiB, drop connection on overflow (`§3.4`).
- `src/virtual_memory.c:172–199`: make `setPagesRW`/`setPagesRX` return `int`; JIT throws on failure (`§3.2`).
- `src/mining_engine.cpp:337–344`: log + deactivate worker on bad nonce offset (`§4.2 #7`).

**P1.2 Concurrency fixes (close the UB)**
- `src/pool_manager.cpp`: add `stratum_mutex_` (or queue share submissions) (`§4.1`, highest impact).
- `src/stratum_client.cpp`: make handshake/session members atomic or guarded (`§4.1`).
- `src/stratum_client.cpp`: `reconnect_attempts_` → `std::atomic<unsigned>`.
- `src/vm.cpp:70`: move `last_mode` to per-instance member (low impact, bundled here for a single TSAN-clean checkpoint).
- Add `ARMRX_ENABLE_TSAN` CMake option + TSAN CI run (`§4.4`).

**P1.3 Parser correctness**
- `src/json.cpp:126–129`: delete the dead first branch in `get_array_first` (`§3.4`).
- `src/json.cpp:64–77`: scope `find_key` to object-key positions (`§3.4`).
- `src/json.cpp:43–57`: escape all U+0000–U+001F (`§3.4`).
- Add `tests/test_json_parser.cpp` covering all three (`§4.4`).

**P1.4 Trivial debt cleanup (folded, no individual slots)**
- `src/vm.cpp`: delete dead `ceil_*` constants, fix `allocate()` comment, gate redundant `reg_.a` init behind `if (!jit_)`.
- `include/armrx/vm.hpp:171`: add a one-line comment noting the `std::fill` makes the `register_usage_ = {-1}` initializer moot (no code change per review).
- `include/armrx/vm.hpp:53–56`: add a comment documenting the `kRandOMXFlag*` value divergence from upstream — **do not rename** (per review, churn not worth it).
- `src/jit_compiler_a64_static.S:273`: refresh stale comment (12→18 instructions for FSQRT_R fast path); `src/jit_compiler_a64.cpp:744`: fix `sub`→`mul` comment.
- `src/main.cpp:379–380`: remove `[DEBUG]` log; add SIGTERM handler + numeric-arg validation (`§4.2 #8`).

**P1.5 ROADMAP bookkeeping**
- Mark S7 (emit32 UB) and the pool-connection item as ✅ closed in `ROADMAP.md`. Move S8 (dangling pointer contract) into Phase 3.1 (JIT encapsulation).
- Add **PGO** to the ROADMAP "remaining" table pointing at Phase 2.6 (was missing).

**P1.6 `generateProgram`/`generateProgramLight` dedup (moved up from Phase 2 per review)**
- Extract `emitPrologueMix` / `emitV2AesTweak` / `emitSpMix2` helpers in `src/jit_compiler_a64.cpp`.
- Collapse `:171–259` and `:261–355` onto a shared path; the only divergence is the light-mode-specific emit at `:283–293`.
- **KAT parity required before and after** — this touches the v2 AES-tweak block which is a silent-hash-wrong risk if it drifts.
- Orthogonal to instruction-handler bodies → does not conflict with Phase 2 peephole work.

**Exit criteria for Phase 1**: full KAT suite green; TSAN run on a 30-second mock-pool mining test clean; the three parser bugs each have a red-to-green test; v2-mode KAT still passing after P1.6.

### Phase 2 — Architectural Evolution & Scaling (3–6 weeks)

Goal: close the performance gap and modularize for maintainability.

**P2.1 JIT peephole program (the ROADMAP's P3) — execute `docs/peephole-jit-plan.md`**
- Phase 1.1: `--jit-dump` flag with opcode boundary markers in `src/main.cpp`.
- Phase 1.2: `tests/bench_opcodes.cpp` with frequency histograms from real RandomX programs.
- Phase 1.3: spot-check whether register allocation dominates the gap (informs Phase 3.1).
- Phase 1.4 + 1.5: encoding + determinism tests (`§4.4`).
- Phase 2 opcode audit, informed by frequency data. **First target: `h_FDIV_M` Markstein iteration drop** (`§3.1`) — single biggest expected win, KAT-vetoed.

**P2.2 (was P2.4 in v1) Test coverage expansion**
- `tests/test_jit_encodings.cpp`, `tests/test_jit_determinism.cpp`, `tests/test_stratum_handshake.cpp`, `tests/test_tls_verification.cpp`.
- Property tests for `meets_target` boundary conditions (currently 6 hand-written asserts in `test_mining.cpp:11–44`).

**P2.3 Structured logger + W^X default-on (`§2.5`, `§4.2 #5`)**
- `include/armrx/log.hpp` with leveled, mutex-guarded sink; TUI-mode ring buffer.
- Replace all `std::cerr`/`std::cout` in stratum/tls/pool/mining.
- Flip `rwx_` to opt-in (`--unsafe-jit-rwx`), W^X default.

**P2.4 Memory tier upgrade (`§3.2`) + JIT-path init dedup**
- Route `Argon2dCache`, `MappedMemory`, and VM scratchpad through `allocLargePagesMemory` (`MAP_HUGETLB`) with mmap+madvise fallback.
- Add `MADV_POPULATE_WRITE` warmup on the scratchpad.
- Gate `reg_.a[0..1]` init behind `if (!jit_)` (`vm.cpp:234–241` vs `:840`).

**P2.5 Per-hash hot-path reductions (`§3.3`)**
- `mining_engine.cpp:296`: per-job `resize`+`memcpy`, per-hash nonce-only patch.
- `superscalar.cpp:428,441`: `std::vector<int>` → `std::array<int,8>` + count.
- `blake2b` overload dedup; **measure before** touching the NEON XOR redundancy.
- `AesGenerator4R::next()` → route through `fill_aes_4r_x4` if call-site analysis shows it on a hot path.

**P2.6 PGO unblock (added per review — was missing from v1)**
- Reproduce the GCC 15 + LTO + musl `__gcov_*` linker crash from `OPTIMIZATION_REFERENCE.md:47`.
- Try, in order: (a) PGO without LTO (`-fprofile-use -fno-lto` — already the CMake path at `CMakeLists.txt:112–113`), (b) a static libgcov link, (c) an Alpine/musl patch for `__gcov_*` exports, (d) a different profile-generation runtime.
- If solved: run a `--mine` benchmark in GENERATE mode, then rebuild in USE mode and measure. Expected free uplift 5–10% with no instruction-level work.
- If unsolvable after one day of effort: document the blocker in `OPTIMIZATION_REFERENCE.md` and move on.

**Exit criteria for Phase 2**: instruction-count gap to XMRig ≤ 10% on `perf stat`; TSAN clean on the full pool-mining integration test; no `std::cerr`/`std::cout` outside `log.hpp` and `tui.cpp`; PGO either landed with measured uplift or documented-blocked.

### Phase 3 — Long-term Sustainability (ongoing)

**P3.1 JIT encapsulation (deferred from Phase 2 per review; scope corrected against source, §2.1)**
- Land *after* the peephole program stabilizes, so the encapsulation diff is against a frozen instruction-handler surface rather than a churning one.
- `code`/`emit32`/`emit64` are already private — no action needed there. Delete the dead, unused `getCode()` accessor; relocate `emit32`/`emit64` into the `.cpp`'s anonymous namespace (mechanical, no handler-body changes); introduce `ExecutableRegion` RAII around the `getProgramFunc()` pointer (`§2.1`).
- Consolidate `ARMV8A::` encodings (`§2.3`); derive CBRANCH `static_assert` symbolically.
- Closes S8 (dangling pointer contract) from the ROADMAP.

**P3.2 CI + automation**
- GitHub Actions matrix: x86_64-interpreted (qemu-user), AArch64-native if a Graviton runner is available.
- Two pipelines per commit: `Release+LTO+KAT` and `TSan+ASan+tests`.
- PGO pipeline as a separate workflow (only if P2.6 unblocked it).

**P3.3 Observability (Prometheus + dashboard)**
- `--metrics-host=127.0.0.1:9100` HTTP endpoint exposing: per-worker H/s, total hashes, accepted/rejected shares, reconnect count, current pool, JIT/dataset init time, branch-miss rate (via `perf_event_open` if permitted).
- **Blocked behind P2.3** (logger) so metrics share the same sink abstraction.

**P3.4 Documentation refresh**
- Single source of truth for RandomX constants (`aes_generator.cpp:9–45` and `aes_hash.cpp:29–41` duplicate the 1R/4R keys in two encodings — consolidate to `armrx/aes_keys.hpp`).
- Single source of truth for opcode dispatch: derive `kCompileHandlers[256]` from `instruction_weights.hpp` instead of the manual 73-line table at `vm.cpp:539–612` (pairs with the `ceil_*` deletion in P1.4).
- Single source of truth for scratchpad masks: the JIT's `Log2(RANDOMX_SCRATCHPAD_L3)-1` and the interpreter's `kScratchpadL3Mask64 = 2097088U` encode the same mask via different constants (`vm.cpp:105` vs `jit_compiler_a64.cpp:211`).

**P3.5 Optional, defer until a forcing function appears**
- **Stratum V2** — only when a target pool mandates it.
- **Newton-Raphson FDIV/FSQRT revisit** — only with a clean KAT proof and XMRig source study, per the ROADMAP's frozen status. The Markstein-iteration drop in P2.1 is the safer unblock for the same opcode.
- **Register allocator overhaul** — only if P2.1's Phase 1.3 spot-check shows allocation (not peephole) dominates the gap.

---

## Quick-reference: highest-leverage changes by ROI (updated for v2)

| Change | Effort | Impact | Risk | Phase |
|---|---|---|---|---|
| `get_array_first` dead-code fix (`json.cpp:126`) | XS | Correctness (wrong difficulty) | None | P1.3 |
| TLS hostname verification (`tls_client.cpp`) | S | Security (MITM prevention) | None | P1.1 |
| `stratum_` mutex (`pool_manager.cpp`) | S | Removes UAF | Low | P1.2 |
| `generateProgram` dedup (v2 AES-tweak block) | M | **Correctness + maintainability** | Low (KAT-gated) | P1.6 (moved up) |
| FDIV_M Markstein iteration drop | S | **−32 inst/program** | KAT-veto required | P2.1 |
| `MAP_HUGETLB` for dataset/cache | S | TLB win on 2 GiB | Fallback needed | P2.4 |
| PGO unblock + enable | M | **Free 5–10% uplift** | musl/gcov blocker | P2.6 (added) |
| Structured logger (`log.hpp`) | M | Unblocks `--tui`, Prometheus | Low | P2.3 |
| Peephole Phase 1 tooling (`--jit-dump`, bench_opcodes) | M | Unblocks P3 | None | P2.1 |
| Full peephole Phase 2 audit | L | **Closes 33% gap** | KAT + hashrate veto per change | P2.1 |
| JIT encapsulation (remove dead `getCode()`, relocate `emit32`/`emit64`) | S | Maintainability + W^X | Low — `code`/`emit32`/`emit64` already private; scope corrected (§2.1) | P3.1 (moved back) |
| ARMV8A encoding consolidation | M | Maintainability only | Low | P3.1 (moved back) |
| Register allocator overhaul | XL | Unknown | High — only if P2.1 Phase 1.3 mandates | P3.5 |

The two non-negotiables before any new feature work: **(a)** the Phase 1 security + concurrency fixes (S1/S6 regressions and the `stratum_` race are correctness regressions against already-shipped claims), and **(b)** the peephole Phase 1 tooling, because every subsequent perf decision depends on the frequency data it produces. **New in v2:** the v2 AES-tweak dedup (P1.6) is also non-negotiable before Phase 2, because it is both a correctness risk and a structural prerequisite for clean peephole diffs.

---

## Appendix A — Review feedback incorporated

This section records every change made between v1 (`next_phase.md`) and v2 in response to the independent review, so the next agent can see the reasoning rather than just the result.

### A.1 Structural moves

| v1 location | v2 location | Review rationale |
|---|---|---|
| §2.2 / Phase 2.2 — `generateProgram` dedup | **§2.2 / Phase 1.6** | "The duplicated v2 AES-tweak block is a real correctness risk that's cheaper to fix than to debug." Duplicated 24-line `emit32` sequences drift silently and produce wrong hashes only on v2-mode programs. Orthogonal to instruction-handler bodies, so it doesn't conflict with peephole work. |
| §2.1 / Phase 2.2 — JIT encapsulation | **§2.1 / Phase 3.1** | "Privatizing `code`/`emit32`/`emit64` requires touching every instruction handler. Better to defer until after the peephole program lands, since touching the JIT surface during active perf work creates merge conflicts." **⚠ Premise corrected post-review, see A.2** — `code`/`emit32`/`emit64` were already private in the source; the deferral is kept for Phase 2.1 diff-cleanliness reasons, but the remaining scope is much smaller than this rationale implies. |
| §2.3 — ARMV8A encoding consolidation | **§2.3 / Phase 3.1** | "Nice for maintainability, but zero correctness impact. Phase 3 at earliest." Folded into the JIT encapsulation batch since both touch the same files. |
| (missing) | **Phase 2.6 — PGO unblock** | "The ROADMAP recommended PGO. Worth adding to Phase 2 as a 'free' 5–10% uplift once the GCC 15 + musl `__gcov_*` crash is solved." Genuinely missed in v1 despite `OPTIMIZATION_REFERENCE.md:47` documenting the blocker. |

### A.2 Severity / scope corrections

| v1 claim | v2 correction | Review rationale |
|---|---|---|
| `last_mode` data race listed as a Phase 1 blocker alongside the `stratum_` UAF | Reclassified as **low impact** (extra `fesetround` per mode switch, not a crash). Still fixed in Phase 1.2, but bundled for a single TSAN-clean checkpoint rather than flagged as a blocker. | "Technically UB (non-atomic read of non-atomic), but impact is a single extra `fesetround` per mode switch — not a crash. Worth fixing but not Phase 1 blocker." |
| `register_usage_[8] = {-1}` listed as a fix item | **Dropped.** Comment-only at most. | "`{-1}` only sets element 0 in C++, true — but `compile_program` immediately `std::fill`s all 8 before use. Harmless and well-understood. Low priority." |
| `kRandOMXFlag` rename to fix the "OMX" typo | **Dropped.** Comment-only. | "Renaming OMX → OMX is a cosmetic change that breaks every usage. Not worth the churn unless you're renaming anyway." v2 keeps a comment documenting the value divergence from upstream, which is the actual issue. |
| `ceil_*` deletion as its own Phase 1 bullet | **Folded** into Phase 1.4 general debt. | "Deleting documentation-only constants. Trivial, but also trivial to ignore. Not worth a Phase 1 slot." |
| `blake2b` NEON XOR redundancy listed as an optimization | **Gated behind measurement.** | "Guessing without perf stat. Needs measurement before committing." v2 requires disassembly proof the compiler isn't already CSE'ing before any change. |
| §2.1 JIT encapsulation described `code`/`emit32`/`emit64` as public and "globally reachable" | **Corrected.** Re-checked against `jit_compiler_a64.hpp:50–91`: `code`, `emit32`, and `emit64` are already `private`. The only public leak is the dead, zero-caller `getCode()` accessor. `getProgramFunc()` is a legitimate, necessary public accessor (used once, `vm.cpp:842`). | Direct source read during a later verification pass (not the original review) — the v1/v2 code snippet quoting a public `emit32`/`emit64` did not match the actual header. Phase 3.1 scope shrunk accordingly: no handler-body changes are needed to fix this, just deleting `getCode()` and relocating two free functions. |

### A.3 What was kept as-is

The review explicitly endorsed these v1 items, so they are unchanged in v2:

- §1.3 — the three "weaker than ROADMAP implies" findings (JSON `session_id_` injection, `stratum_` race, TLS hostname verification).
- §3.4 — `get_array_first` dead-code bug ("a genuine correctness issue — a target string near 512 bytes would produce wrong difficulty silently").
- §4.2 — the security list (TLS hostname, unescaped `session_id_`, `SSL_shutdown` on half-init, etc.).
- The Phase 1 → Phase 2 → Phase 3 ordering ("security + concurrency fixes first, performance second, sustainability third").
- The peephole program sequencing and KAT/hashrate veto discipline.

### A.4 Review items not incorporated (and why)

None. Every actionable point in the review is reflected above. The two "could be cut" suggestions (ARMV8A consolidation, blake2b NEON XOR) were not deleted from the document — they were *deferred* (ARMV8A → Phase 3) and *gated* (blake2b → measurement-required) so the work isn't lost, just correctly prioritized.
