# armrx — Next-Phase Improvement & Evolution Plan

> A consolidated master plan derived from a full audit of the codebase against `ROADMAP.md`. armrx has reached a strong baseline: clean-room RandomX v1, JIT + interpreted paths both KAT-passing, pool mining with failover, TLS, and an 18% gap to XMRig driven entirely by distributed codegen. The next frontier is closing that gap safely, hardening the network path (which has real races today), and graduating the project from "works on the dev device" to "maintainable and correct under hostile input."

---

## 1. Current State Assessment & Differential Analysis

### 1.1 What is solid (do not break)

Cross-checked `ROADMAP.md` against the actual source. The completed claims hold:

- **Crypto core** — `src/blake2b.cpp` (NEON + scalar), `src/argon2.cpp` (NEON G-function via `vmull_u32`+`vmovn_u64`), `src/aes_hash.cpp` (`vaeseq`/`vaesmcq` chains) all produce reference KAT vectors. NEON paths are idiomatic, not hand-rolled ASM. `tests/test_blake2b.cpp:194,200` asserts both KAT inputs in interpreted mode and lines `209,213` re-assert under `#ifdef ARMRX_HAVE_JIT`.
- **JIT backend** — `src/jit_compiler_a64.cpp` uses a frequency-weighted 256-entry dispatch table (`engine[256]`, header line 73) with `REPN(..., WT(x))`. Static template `jit_compiler_a64_static.S` reserves 6144 slots. W^X is honored via `setPagesRW→emit→setPagesRX` unless `rwx_` is set.
- **Dispatch-table refactor (P1)** — `kCompileHandlers[256]` in `vm.cpp:539–612` plus a clean enum switch in `execute_bytecode` (`vm.cpp:621–758`). Two-table design (raw opcode → bytecode → enum) is sound.
- **Branchless CBRANCH (O11)** — `jit_compiler_a64.cpp:1156–1174` deploys the `bne .Lskip; b target` form with `imm19=2`. The postmortem in `docs/experiments/branchless-cbranch.md` correctly identifies the prior hang as an off-by-one in the B.cond immediate.
- **Pool/network surface area** exists end-to-end: subscribe/authorize/notify/submit in `stratum_client.cpp`, CryptoNote + Stratum V1 fallback, exponential backoff, multi-pool failover in `pool_manager.cpp`.
- **Tooling** — `bench_armrx.cpp` registered in CTest; ASan/UBSan/PGO/LTO CMake options all wired in `CMakeLists.txt:7–13, 104–136`; `.clang-format` / `.clang-tidy` present.

### 1.2 ROADMAP items — current relevance

| ROADMAP item | Status | Relevance now |
|---|---|---|
| **S7 — `emit32` UB** | Marked ✅ (memcpy fix) | **Done and verified** at `jit_compiler_a64.hpp:81–91`. Close it. |
| **S8 — Dangling pointer contract** | Marked ⏸️ (docs deferred) | Still **relevant** — see §2.1, the `getCode()` encapsulation gap is the dangling-pointer entry point. |
| **O12 — Newton-Raphson FDIV/FSQRT** | Marked ⏸️ Frozen (segfault) | **Correctly frozen.** `OPTIMIZATION_REFERENCE.md:46` documents x29 corruption on Cortex-A53 in-order. Keep frozen; the `h_FDIV_M` Markstein loop at `jit_compiler_a64.cpp:1045–1079` emits **17 instructions** that a different optimization (drop iterations 2–3) can shrink without touching NR. |
| **P3 — Peephole JIT coalescing** | 🔴 Remaining | **Primary performance lever.** 33% instruction-count gap is the whole story. Plan in `docs/plans/peephole-jit-plan.md` is sound — Phase 1.2 frequency data is the prerequisite. |
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

### 2.1 JIT compiler encapsulation (high-value refactor)

The `JitCompilerA64` public surface leaks executable-memory capability:

```cpp
// include/armrx/jit_compiler_a64.hpp:62–91
uint8_t* getCode() { return code + (((uint32_t)CodeSize) * 4); }
uint8_t* getProgramFunc() { return code + CodeSize * 4; }
static void emit32(uint32_t instr, uint8_t* code, uint32_t& codePos);
static void emit64(uint64_t instr, uint8_t* code, uint32_t& codePos);
```

Any caller can write into RX memory bypassing `enableWriting()`, and `emit32`/`emit64` are globally reachable. Refactor:

- Make `code` private and expose only `std::span<const std::byte> code() const noexcept` for inspection and a `ProgramFunc` typedef for the entry-point pointer.
- Move `emit32`/`emit64` into the anonymous namespace of the `.cpp` (they take `code`/`codePos` as params already, so no member access is needed).
- Gate the executable pointer behind an `ExecutableRegion` RAII type whose destructor calls `setPagesRX` — this makes W^X a property of the type system, not a discipline.

### 2.2 Deduplicate `generateProgram` / `generateProgramLight`

`jit_compiler_a64.cpp:171–259` and `:261–355` share ~70% of their body byte-for-byte: the opcode dispatch loop, the `ubfx x19/x20` scratchpad-mix emits, the entire v2 AES-tweak block (`:219–242` ≡ `:317–340`), and the `v2_FE_mix` memcpy. Extract:

```cpp
// private helpers in jit_compiler_a64.cpp
uint32_t emitPrologueMix(ProgramConfiguration const& cfg, uint32_t codePos);
void emitV2AesTweak(uint32_t& codePos);   // ~24 lines, currently duplicated
void emitSpMix2(uint32_t& codePos);       // the eor at 191/281
```

This is the single biggest maintainability win in the JIT. The v2 AES-tweak block in particular is a 24-line sequence of raw `emit32` calls — duplicated drift there is a silent correctness bug class.

### 2.3 Consolidate `ARMV8A::` encodings

The `ARMV8A` namespace (`jit_compiler_a64.cpp:80–104`) covers ~25 mnemonics. The other ~15 raw hex opcodes (`0x121A0000` and-immediate, `0xD3400000` ubfx, `0xf8606840` ldr-reg, `0x0C407800` ld1, `0x6E004400` rev64, `0x54000000 | (2 << 5) | 1` B.cond) are inlined as bare literals at the call sites. Every literal is a future "what does this mean?" question for a maintainer and a typo-class bug waiting (a single wrong nibble in `0xD3400000` produces a different instruction silently).

Action: extend `ARMV8A` with `UBFX`, `AND_IMM`, `LDR_REG`, `LD1`, `BFI`, `B_COND(cond, imm19)`, `MSR_FPCR`, `RBIT`. Then the branchless-CBRANCH `static_assert` at `:1153` (which currently hard-codes `0xF2781C1F`) can derive the `tst` encoding symbolically and actually validate it.

### 2.4 Fix the misleading VM-side state

Several `vm.cpp` items actively confuse future edits:

- **`rx_set_rounding_mode` static cache is a data race** (`vm.cpp:70`). `static std::uint32_t last_mode = 0xFF;` is shared across all VM instances in all threads. Two workers setting different rounding modes corrupt each other's cache. Make it a per-instance member or delete the cache (CFROUND is freq-1, cache hit-rate ≈ 1).
- **Dead `ceil_*` constants** (`vm.cpp:113–142`) — unreferenced documentation-only block that looks authoritative. Delete or use them to auto-generate `kCompileHandlers[256]` (eliminating the 73-line manual table at `:539–612`).
- **Misleading `register_usage_[8] = {-1}`** (`vm.hpp:171`) only initializes element 0; `compile_program` at `:615` masks this with `std::fill(..., -1)`. The in-class initializer lies. Replace with `{-1,-1,-1,-1,-1,-1,-1,-1}` or delete it.
- **`VirtualMachine::allocate()`** (`vm.cpp:210–212`) is empty with a comment referencing `std::vector` — the ctor actually uses `mmap` (`:167–169`). Delete the function or fix the comment.
- **Wasted work in JIT path**: `initialize_vm_state` (`vm.cpp:234–241`) writes `reg_.a[0..1]`, then `run_jit` at `:840` overwrites them with `config.eMask`. Gate the first behind `if (!jit_)`.
- **`kRandOMXFlag*` typo** (`vm.hpp:53–56`) — "OMX" mid-word capitalization. Also: flag values (`Jit=4`, `FullMem=32`) **conflict** with upstream RandomX (`FULL_MEM=4`). Add a comment documenting the deliberate divergence or align.

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

Cycles are within 5% — the CPU is doing comparable work. The gap is *pure emitted-code volume* plus branch misprediction. The path forward is `docs/plans/peephole-jit-plan.md`, with the highest-leverage specific targets already identified by the audit:

- **`h_FDIV_M` Markstein loop** (`jit_compiler_a64.cpp:1045–1079`) emits **17 instructions** with 3 NR-style iteration pairs. RandomX does not require IEEE-correct rounding on FDIV_M. If the KAT suite passes with 2 iterations, **drop the third pair → −8 instructions per FDIV_M, 4× per program = −32 instructions/program**. This is a single-opcode win bigger than most of Phase 2 combined. **Must** be KAT-vetoed per the plan's principle #1.
- **`h_IMUL_RCP` immediate materialization** (`jit_compiler_a64.cpp:500–526`): `emitMovImmediate` long-path emits `movz`+`movk` for any imm ≥ 2¹⁶, but `movz` with `hw=0..3` encodes any single-halfword immediate in one instruction. Add the single-halfword fast path.
- **`emitMemLoad` mask redundancy** (`:567–598`): `emitAddImmediate` may emit up to 3 instructions, then the result is AND-masked. Fold the AND into the addressing when the add fits the low 12 bits.
- **`h_IROR_R` / `h_IROL_R`** (`:901–955`): armrx uses `rotr`/`rotl`; `extr` (rotate-insert) may save an instruction and is the form XMRig uses for these.

**Branch misses (+13×)** is the second-order problem. The branchless CBRANCH fix addressed one site; the BTB-aliasing caveat in `docs/experiments/branchless-cbranch.md:71–79` (per-program JIT regeneration means fixed code addresses see rotating branch behavior) is the larger contributor and has no cheap fix — it's a property of regenerating the JIT buffer per seed. Worth measuring with `perf stat -e branch-misses` after Phase 1.1 lands.

### 3.2 Memory: huge pages, cold-start faults, and mprotect visibility

- **No `MAP_HUGETLB` anywhere it matters.** `Argon2dCache` (`argon2.cpp:242–247`), `MappedMemory` (`mining_engine.hpp:28–31`), and the VM scratchpad (`vm.cpp:167–174`) all use plain `MAP_ANONYMOUS` + `madvise(MADV_HUGEPAGE)`. This depends on THP being enabled (`/sys/kernel/mm/transparent_hugepage/enabled`), which is not guaranteed on mining rigs. `virtual_memory.c::allocLargePagesMemory` (`:206`, uses `MAP_HUGETLB | MAP_POPULATE`) exists but is **not called by any of these callers**. For the 2 GiB dataset and 256 MiB cache this is the biggest available TLB win. Wire it with fallback.
- **Scratchpad cold-start faults**: `vm.cpp:167–174` mmaps the 2 MiB scratchpad with no `MAP_POPULATE`. First iteration faults in 2 MiB pages. Add `MADV_POPULATE_WRITE` (Linux 5.14+) on a warmup hook, or one-time `memset`.
- **`mprotect` failures are silently swallowed** (`virtual_memory.c:172–199`): `setPagesRW`/`setPagesRX` are `void` and discard the `pageProtect` errno. For a W^X-hardened JIT this is a security hole — a failed RX transition leaves pages RW or RWX invisibly. Make these return `int` (like `setPagesRWX` does) and have the JIT throw on failure.

### 3.3 Per-hash hot-path cost reductions

- **Per-iteration full vector copy** (`mining_engine.cpp:296`): `block_input = local_job.block_template;` copies the entire blob on every hash. The comment claims "only reallocates on job change" — that's wrong; `operator=` copies contents even when capacity is sufficient. For 76-byte Monero blobs it's cheap, but the fix is free: `resize`+`memcpy` once per job, then `std::memcpy` only the nonce bytes per hash. This also closes the silent-swallow bug at `:337–344` (a bad `nonce_offset` currently leaves the worker hashing an unmodified template forever).
- **`generate_superscalar` heap churn** (`superscalar.cpp:428,441`): allocates `std::vector<int> availableRegisters` per `selectRegister`/`selectDestination` call. Cache-init only, but 8 programs × N selections each. Replace with `std::array<int,8>` + count.
- **`blake2b` NEON round**: `veorq_u64(vd, va)` is computed twice per round at `blake2b.cpp:92,94,96,98` (and 116, 141, 167). A single temporary eliminates ~8 redundant XORs per round × 12 rounds. Compiler *may* CSE this — verify in codegen.
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

### 4.1 Concurrency bugs (these are UB, not theoretical)

| Bug | Site | Fix |
|---|---|---|
| **`stratum_` use-after-free** | Worker threads call `pool_mgr->submit_share` → `stratum_->...` (`pool_manager.cpp:85`) while main thread reassigns `stratum_` during failover (`:51`). | Add `std::mutex stratum_mutex_` to `PoolManager`, or move share submission off worker threads via a lock-free queue that the main thread drains. |
| **Handshake/session members raced** | `session_id_`, `extra_nonce1_`, `subscribe_ok_`, `protocol_`, `handshake_req_id_`, `authorize_req_id_` written by reader thread, read by submit/keepalive/main. | Make these `std::atomic` (where scalar) or guard with a `session_mutex_`. |
| **`reconnect_attempts_` non-atomic** | Plain `unsigned`, written in `reconnect_loop` (`stratum_client.cpp:701`), read by `PoolManager::tick`. | `std::atomic<unsigned>`. |
| **`rx_set_rounding_mode` static cache** | `vm.cpp:70` shared across all VMs/threads. | Per-instance member. |
| **`std::cout` race** | Reader thread + main TUI thread + worker share callbacks. | Resolved by §2.5 logger. |

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

### Phase 1 — Immediate Stabilization & Debt Clearance

Goal: close correctness regressions and the ROADMAP's open "soft" items. No new features.

**P1.1 Security hardening (close the S1/S6 regressions)**
- `src/tls_client.cpp`: add `X509_VERIFY_PARAM_set1_host` (`§4.2 #1`).
- `src/stratum_client.cpp:315,683`: wrap `session_id_` in `armrx::json::escape` (`§4.2 #2`).
- `src/stratum_client.cpp:391`: cap `read_buf_` at 1 MiB, drop connection on overflow (`§3.4`).
- `src/virtual_memory.c:172–199`: make `setPagesRW`/`setPagesRX` return `int`; JIT throws on failure (`§3.2`).
- `src/mining_engine.cpp:337–344`: log + deactivate worker on bad nonce offset (`§4.2 #7`).

**P1.2 Concurrency fixes (close the UB)**
- `src/vm.cpp:70`: move `last_mode` to per-instance member (`§4.1`).
- `src/pool_manager.cpp`: add `stratum_mutex_` (or queue share submissions) (`§4.1`).
- `src/stratum_client.cpp`: make handshake/session members atomic or guarded (`§4.1`).
- Add `ARMRX_ENABLE_TSAN` CMake option + TSAN CI run (`§4.4`).

**P1.3 Parser correctness**
- `src/json.cpp:126–129`: delete the dead first branch in `get_array_first` (`§3.4`).
- `src/json.cpp:64–77`: scope `find_key` to object-key positions (`§3.4`).
- `src/json.cpp:43–57`: escape all U+0000–U+001F (`§3.4`).
- Add `tests/test_json_parser.cpp` covering all three (`§4.4`).

**P1.4 Misleading-state cleanup**
- `src/vm.cpp`: delete dead `ceil_*`, fix `allocate()` comment, gate redundant `reg_.a` init behind `if (!jit_)` (`§2.4`).
- `include/armrx/vm.hpp:171`: fix `register_usage_` initializer; `:53–56` fix `kRandOMXFlag*` typo + add divergence comment (`§2.4`).
- `src/jit_compiler_a64_static.S:273`: refresh stale comment (12→18 instructions for FSQRT_R fast path); `src/jit_compiler_a64.cpp:744`: fix `sub`→`mul` comment (`§2`).
- `src/main.cpp:379–380`: remove `[DEBUG]` log; add SIGTERM handler + numeric-arg validation (`§4.2 #8`).

**P1.5 ROADMAP bookkeeping**
- Mark S7 (emit32 UB) and the pool-connection item as ✅ closed in `ROADMAP.md`. Move S8 (dangling pointer contract) into Phase 2.1 (JIT encapsulation).

**Exit criteria for Phase 1**: full KAT suite green; TSAN run on a 30-second mock-pool mining test clean; the three parser bugs each have a red-to-green test.

### Phase 2 — Architectural Evolution & Scaling 

Goal: close the performance gap and modularize for maintainability.

**P2.1 JIT peephole program (the ROADMAP's P3) — execute `docs/plans/peephole-jit-plan.md`**
- Phase 1.1: `--jit-dump` flag with opcode boundary markers in `src/main.cpp`.
- Phase 1.2: `tests/bench_opcodes.cpp` with frequency histograms from real RandomX programs.
- Phase 1.3: spot-check whether register allocation dominates the gap (informs Phase 3.1).
- Phase 1.4 + 1.5: encoding + determinism tests (`§4.4`).
- Phase 2 opcode audit, informed by frequency data. **First target: `h_FDIV_M` Markstein iteration drop** (`§3.1`) — single biggest expected win, KAT-vetoed.

**P2.2 JIT compiler refactor (`§2.1–§2.3`)**
- Privatize `code`/`emit32`/`emit64`; introduce `ExecutableRegion` RAII.
- Extract `emitPrologueMix`/`emitV2AesTweak`/`emitSpMix2` helpers; collapse the `generateProgram`/`generateProgramLight` duplication.
- Extend `ARMV8A::` namespace; derive the CBRANCH `static_assert` symbolically.

**P2.3 Structured logger + W^X default-on (`§2.5`, `§4.2 #5`)**
- `include/armrx/log.hpp` with leveled, mutex-guarded sink; TUI-mode ring buffer.
- Replace all `std::cerr`/`std::cout` in stratum/tls/pool/mining.
- Flip `rwx_` to opt-in (`--unsafe-jit-rwx`), W^X default.

**P2.4 Memory tier upgrade (`§3.2`)**
- Route `Argon2dCache`, `MappedMemory`, and VM scratchpad through `allocLargePagesMemory` (`MAP_HUGETLB`) with mmap+madvise fallback.
- Add `MADV_POPULATE_WRITE` warmup on the scratchpad.

**P2.5 Test coverage expansion**
- `tests/test_jit_encodings.cpp`, `tests/test_jit_determinism.cpp`, `tests/test_stratum_handshake.cpp`, `tests/test_tls_verification.cpp`.
- Property tests for `meets_target` boundary conditions (currently 6 hand-written asserts in `test_mining.cpp:11–44`).

**Exit criteria for Phase 2**: instruction-count gap to XMRig ≤ 10% on `perf stat`; TSAN clean on the full pool-mining integration test; no `std::cerr`/`std::cout` outside `log.hpp` and `tui.cpp`.

### Phase 3 — Long-term Sustainability (ongoing)

**P3.1 CI + automation**
- GitHub Actions matrix: x86_64-interpreted (qemu-user), AArch64-native if a Graviton runner is available.
- Two pipelines per commit: `Release+LTO+KAT` and `TSan+ASan+tests`.
- PGO pipeline as a separate workflow (the GCC 15 + LTO + musl `__gcov_*` crash in `OPTIMIZATION_REFERENCE.md:47` needs a non-LTO PGO path or a CI image with a working gcov).

**P3.2 Observability (Prometheus + dashboard)**
- `--metrics-host=127.0.0.1:9100` HTTP endpoint exposing: per-worker H/s, total hashes, accepted/rejected shares, reconnect count, current pool, JIT/dataset init time, branch-miss rate (via `perf_event_open` if permitted).
- **Blocked behind P2.3** (logger) so metrics share the same sink abstraction.

**P3.3 Documentation refresh**
- Single source of truth for RandomX constants (`aes_generator.cpp:9–45` and `aes_hash.cpp:29–41` duplicate the 1R/4R keys in two encodings — consolidate to `armrx/aes_keys.hpp`).
- Single source of truth for opcode dispatch: derive `kCompileHandlers[256]` from `instruction_weights.hpp` instead of the manual 73-line table at `vm.cpp:539–612`.
- Single source of truth for scratchpad masks: the JIT's `Log2(RANDOMX_SCRATCHPAD_L3)-1` and the interpreter's `kScratchpadL3Mask64 = 2097088U` encode the same mask via different constants (`vm.cpp:105` vs `jit_compiler_a64.cpp:211`).

**P3.4 Optional, defer until a forcing function appears**
- **Stratum V2** — only when a target pool mandates it.
- **Newton-Raphson FDIV/FSQRT revisit** — only with a clean KAT proof and XMRig source study, per the ROADMAP's frozen status. The Markstein-iteration drop in P2.1 is the safer unblock for the same opcode.
- **Register allocator overhaul** — only if P2.1's Phase 1.3 spot-check shows allocation (not peephole) dominates the gap.

---

## Quick-reference: highest-leverage changes by ROI

| Change | Effort | Impact | Risk |
|---|---|---|---|
| `get_array_first` dead-code fix (`json.cpp:126`) | XS | Correctness (wrong difficulty) | None |
| TLS hostname verification (`tls_client.cpp`) | S | Security (MITM prevention) | None |
| `stratum_` mutex (`pool_manager.cpp`) | S | Removes UAF | Low |
| FDIV_M Markstein iteration drop | S | **−32 inst/program** | KAT-veto required |
| `MAP_HUGETLB` for dataset/cache | S | TLB win on 2 GiB | Fallback needed |
| Structured logger (`log.hpp`) | M | Unblocks `--tui`, Prometheus | Low |
| JIT `generateProgram` dedup | M | Maintainability | Low |
| Peephole Phase 1 tooling (`--jit-dump`, bench_opcodes) | M | Unblocks P3 | None |
| Full peephole Phase 2 audit | L | **Closes 33% gap** | KAT + hashrate veto per change |
| Register allocator overhaul | XL | Unknown | High — only if Phase 1.3 mandates |

The two non-negotiables before any new feature work: **(a)** the Phase 1 security + concurrency fixes (S1/S6 regressions and the `stratum_` race are correctness regressions against already-shipped claims), and **(b)** the peephole Phase 1 tooling, because every subsequent perf decision depends on the frequency data it produces.