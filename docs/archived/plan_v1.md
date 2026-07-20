armrx — Master Update & Improvement Plan
Executive Summary
armrx is a clean-room AArch64 RandomX v1 miner (~22 H/s on 8× Cortex-A53 vs XMRig's 27 H/s — a 15% gap). The codebase is 5,900 LOC of original C++20 plus vendored xmrig-dev/ reference and scratch_vm_study/. The architecture is sound, but analysis surfaced one critical memory-safety bug, one W^X security regression vs upstream, a hand-rolled JSON parser duplicated 3× with injection surface, no tests for TLS/Stratum/config/JIT/virtual_memory, no CI, and magic-number sprawl in the JIT.
Pillars below cite exact file:line locations and prioritize fixes by security-impact-first, then correctness, then performance.
1. Code Architecture & Structural Integrity
1.1 Architectural Bottlenecks & Tight Couplings
A1. God-functions in the VM core (highest maintainability debt)
- VirtualMachine::compile_instruction() — src/vm.cpp:251-642 — 392-line cascade of 24 if (opcode < ceil_X) { ...; return; } blocks. Each block duplicates auto dst = instr.dst % 8; auto src = instr.src % 8; (~20×). Linear-frequency ladder instead of an opcode-indexed dispatch table.
- VirtualMachine::run() — src/vm.cpp:807-953 — 147 lines mixing JIT path (:820-877) and 2048-iteration interpreter loop (:885-952) with no separation. Should split into runJit() / runInterp().
- VirtualMachine::execute_bytecode() — src/vm.cpp:651-788 — 138-line switch over 30 opcodes.
- generate_superscalar() — src/superscalar.cpp:547-687 — 141 lines, 6-level nesting, throwAwayCount retry logic.
A2. Two sources of truth for "fast vs light mode"
- Interpreter keys off flags_ & kRandOMXFlagFullMem (vm.cpp:791)
- JIT path keys off dataset_.empty() (vm.cpp:835, :850)
- A single is_fast_mode() accessor would prevent silent drift between paths.
A3. Hand-rolled JSON is duplicated in 3 modules with divergent behavior
- stratum_client.cpp:41-115 — json_get, json_get_array_first, json_rpc
- config.cpp:12-66 — json_str, json_bool, json_num, json_str_array
- These parsers do no structural validation and have at least one bug already: stratum_client.cpp:70 checks json[i-1] != '\\' to detect string boundaries, which mis-detects \\" (escaped backslash before quote). A malicious pool could break parsing.
- All three also build JSON by string concatenation (stratum_client.cpp:112-115, :358-398) — wallet_ / password_ / job.job_id are inserted unescaped, allowing JSON injection via wallet addresses containing " or \.
- Refactor target: a single armrx::json module (header-only ~150 LOC) with escaping on output and a real tokenizer on input.
A4. mining_common.hpp + stratum_client.hpp reach cyclical coupling via Job
StratumClient is defined in terms of Job from mining_common.hpp, and mining_common.hpp::meets_target is reused in tests, the mining engine, AND stratum. Job struct has no encapsulation (all public) — any change ripples across 3 modules. Promote Job to its own header with validation in setters.
A5. main.cpp is a 541-line monolith doing 5 jobs
Configuration loading + arg parsing + pool mining loop + benchmark loop + TUI rendering glue all live in main(). The stratum failover state machine (main.cpp:420-480) is inline-callback-based — there is no PoolManager abstraction. Extract:
- CliParser (use existing apply_cli_overrides already declared at config.hpp:37 but never called by main — dead code)
- PoolManager (owns StratumClient list, failover state, reconnect cooldown)
- BenchRunner (the should_mine block, main.cpp:298-360)
A6. Dead scaffolding in JIT headers
- include/armrx/jit_compiler.hpp:38-67 declares CodeBuffer and CompilerState (with emit<T>, emitAt, registerUsage[8], instructionOffsets[256]) — NEVER referenced by JitCompilerA64 or any src/ file. Either delete it or finish the refactor it was meant to enable.
- src/jit_compiler_a64.cpp:36-58 re-declares upstream RandomX constants (RANDOMX_FLAG_HARD_AES=2, RANDOMX_FLAG_FULL_MEM=4, etc.) even though it already includes randomx_config.hpp at :34. Two sources of truth — change one without the other and flags & RANDOMX_FLAG_V2 checks at :207, 233, 304 silently break.
A7. const_cast abuse in StratumClient message builders
build_subscribe_msg, build_login_msg, build_authorize_msg, build_submit_msg are declared const but mutate request_id_, handshake_req_id_, authorize_req_id_, subscribe_try_ via const_cast<StratumClient*>(this)->... (stratum_client.cpp:336-337, 351, 356-357, 371-372, 379). Methods should not be const — fix the constness, not the casts.
1.2 Recommended Refactor (Priority Order)

Priority	Refactor	Files
P0	Extract armrx::json module with proper escaping; remove duplication	new include/armrx/json.hpp, src/json.cpp; modify stratum_client.cpp, config.cpp
P1	Split VirtualMachine::run into runJit/runInterp; add is_fast_mode() helper	src/vm.cpp, include/armrx/vm.hpp
P1	Replace compile_instruction ladder with InstructionHandler[256] dispatch table	src/vm.cpp
P2	Introduce PoolManager class encapsulating failover/reconnect logic in main.cpp:420-480	new src/pool_manager.cpp, include/armrx/pool_manager.hpp
P2	Wire apply_cli_overrides (already declared, unused); delete dead CodeBuffer scaffolding	src/main.cpp, include/armrx/jit_compiler.hpp
P3	Template AES encrypt/decrypt (aes.cpp:65-113) and HW/fallback AES hash paths (aes_hash.cpp × 4) using if constexpr	src/aes.cpp, src/aes_hash.cpp


2. Performance & Resource Optimization
2.1 Critical Memory-Safety Bug (acts as silent perf & correctness hazard)
CRITICAL — vm.cpp:790-795: Fast-mode dataset_read computes datasetLine = dataset_.data() + address without any check that address + 64 ≤ dataset_.size(). set_dataset() (vm.cpp:197-199) accepts any span size without validation. With dataset_offset_ = (entropy_[13] % 524288) * 64 (vm.cpp:246, up to ~32 MiB) added (vm.cpp:921), an undersized dataset means out-of-bounds read at full speed. The only validation that exists is in dataset.cpp:68-77 (dataset_range_is_valid) — the runtime hot path skips it.
Fix: add [[nodiscard]] bool set_dataset(std::span<const std::byte>) that enforces size == kRandomXDatasetBytes and return false otherwise. Add a debug_assert (compiles-in under all builds, unlike plain assert which NDEBUG strips — see §3.3) to dataset_read.
2.2 Per-Hash Allocation Churn
improvement.txt:5-10 already quantifies this — 16 malloc/free pairs per hash (8 BLAKE2b + 7 chain + 1 final). Concrete sites:
- src/vm.cpp:978 — alignas(16) std::array<std::byte, 64> tempHash per call (small but per-hash)
- src/vm.cpp:985 — alignas(16) std::array<std::byte, sizeof(RegisterFile)> reg_bytes — 256 B per chain × 8 chains = 2 KiB copied per hash, only because RegisterFile is not alignas(16).
- src/vm.cpp:812 — std::array<std::byte, sizeof(entropy_) + sizeof(program_.program_buffer_)> prog_bytes{} zero-initialized every run() × 8/hash.
- src/mining_engine.cpp:231 — block_input = local_job.block_template re-allocates a vector every hash (should be thread-local aligned buffer with in-place nonce patch — see improvement.txt 1.6).
Concrete fixes (no spec risk):
1. Declare struct RegisterFile { ... } with alignas(16) (include/armrx/vm.hpp:24-29). Eliminates the input_bytes copy at vm.cpp:961-963 and :969-971 and the hash_and_fill/get_final_result copies at vm.cpp:955-972. Free perf win — blake2b is on the hot path.
2. Make prog_bytes a per-VM member (std::vector<std::byte> reserved once), not a per-run() stack array.
2.3 Wasted Work in Inner Loops
- vm.cpp:68-75 — rx_set_rounding_mode calls fesetround per CFROUND occurrence. Cache last mode, skip if unchanged. CFROUND freq is 1/256, so the fix shaves redundant libc env calls.
- vm.cpp:975-976, 997 — fegetenv/fesetenv wrap every hash. Move to RAII guard that only restores on scope exit (currently not restored if run() throws — minor).
- vm.cpp:97-100 — Scratchpad masks (16376U, 262136U, 2097144U, 2097088U) and 0x7fffffc0ULL dataset address mask (vm.cpp:234, 921 duplicated) should be named constexpr constants.
- mining_engine.cpp:245 — flush_interval = 64 (good — improvement.txt 1.5 was applied). Verified.
- mining_engine.cpp:108 — Dataset init already parallelizes across std::thread::hardware_concurrency() threads (good), but uses std::make_shared<MappedMemory> — MappedMemory does NOT use allocLargePagesMemory (virtual_memory.c:206) for the 2080 MiB dataset. Add huge-page path for fast mode (improvement.txt 2.1, unimplemented).
2.4 SIMD Misses
- src/aes.cpp:36-86 — Fallback AES computes the S-Box via gf_inverse at runtime (square-and-multiply, ~56 GF-muls per byte × 16 bytes/round = ~900 muls per round). Use the already-shipped randomx_aes_lut_enc T-tables at src/soft_aes.cpp:32 instead — ~100× speedup on non-crypto AArch64 and on any non-AArch64 build.
- src/argon2.cpp:66-108 — blamka_add/permute_block are scalar-only. The blamka_add is the heart of the Argon2 compression G-function; on a Cortex-A53 init, this is 786K compression calls × 128 rounds = ~100M scalar mul-adds. Add NEON vmlal_u32 path.
- src/dataset.cpp:118-122 — NEON initialize_dataset extracts cache lanes to scalar GPRs (vgetq_lane_u64), does math as scalar, rebuilds via vcombine_u64. Use vld1q_u64 + veorq_u64 directly.
- src/superscalar.cpp:771-843 (execute_superscalar_neon) — the "NEON" path doesn't actually use NEON for multiplies — every IMUL_R/IMULH_R/IMUL_RCP case extracts both lanes, computes scalar, recombines. Either commit to NEON or remove the duplication.
- include/armrx/jit_compiler_a64_static.S:280 comment is stale: claims FDIV_M worst case = 12 instructions, but the ARMRX_JIT_FAST_DIV_SQRT path at jit_compiler_a64.cpp:1037-1063 emits 14 (also segfaults per OPTIMIZATION_REFERENCE.md:32 — x29 corruption). Defer until verified against XMRig KAT vectors.
2.5 Magic Number Inventory (blocking maintainability)
The JIT is the worst offender. Examples (refer to the JIT analysis for the full list):

Site	Value	Should Be
vm.cpp:161	2097152U	kRandomXScratchpadSize
vm.cpp:864, 885	2048ULL / 2048	kRandomXProgramIterations
vm.cpp:646	256	kRandomXProgramSize
vm.cpp:986, 994	7 + magic 8th	kRandomXProgramCount = 8
vm.cpp:740-741	0x80F0000000000000ULL	kFscalMask
vm.cpp:97-100	scratchpad masks	kScratchpadL1Mask etc.
jit_compiler_a64.cpp:482	num32bitLiterals < 64 cap	kLiteralPoolSize (also static_assert)
jit_compiler_a64.cpp:353	num32bitLiterals = 64 sentinel	symbolic constant
jit_compiler_a64.cpp:69-93	ARMV8A:: namespace raw encodings	per-instruction comments citing ARMv8 ARM section

2.6 Concrete Optimization Targets (ordered by expected impact × effort)
#	Optimization	Site	Expected Impact	Effort
O1	alignas(16) on RegisterFile — eliminates per-hash 2 KiB copy	vm.hpp:24-29	+1–2%	1 line
O2	Thread-local block template reuse (no per-hash std::vector alloc)	mining_engine.cpp:231	+0.5–1%	small
O3	Cache rounding-mode in rx_set_rounding_mode	vm.cpp:68-75	+0.2%	small
O4	NEON Argon2 G-function	argon2.cpp:66-108	-20% cache init time	medium
O5	Use soft_aes.cpp T-tables in AES fallback instead of runtime gf_inverse	aes.cpp:36-86	100× on non-crypto builds	medium
O6	Huge pages for 2080 MiB dataset via allocLargePagesMemory	mining_engine.cpp:105	-30% dataset TLB misses in fast mode	small
O7	per jit_plan.md: A1 ubfx (1 instr saved), A3 NEON ld1+sxtl for FP mem loads, A4 next-iter prefetch — see jit_plan.md for risk	jit_compiler_a64.cpp:591-624, static.S:213-217	+3-7% combined	medium

3. Code Quality, Testing & Security
3.1 Test Coverage — Current State
Only two test executables are registered in CTest (CMakeLists.txt:103-115). bench_armrx is built but not registered with CTest — a regression in it would not fail CI even if CI existed.
Coverage matrix (from agent analysis):
Component	Status	Notes
blake2b.cpp	partial	only empty-input KATs; no multi-block (>128B), no output ≠ 32/64, no output_bytes==0 rejection at blake2b.cpp:218, 241
argon2.cpp	indirect	only via dataset item KATs at test_blake2b.cpp:102-105; no direct block KAT
dataset.cpp	good	4 reference KAT items + range rejection
vm.cpp	shallow	only 2 end-to-end hash KATs
aes.cpp	weak	only single AES-128 round (test_blake2b.cpp:147-163); no decrypt round KAT (comment notes it's the equivalent-inverse form)
jit_compiler_a64.cpp	none at unit level	only exercised via end-to-end hash
virtual_memory.c	none	no mmap/mprotect/huge-page fallback test
stratum_client.cpp	none	handshake, notify, set_target, submit, reconnect backoff, failover — zero tests
tls_client.cpp	none	conditional compile, zero tests
config.cpp	none	file parser, env-var path, fallback chain untested
tui.cpp	none	render()/shutdown() never exercised
cpu_features.cpp	none	detection never called from tests
superscalar.cpp	none	only indirect via dataset KATs
mining_engine.cpp	shallow	test_mining.cpp:47-75 does start/stop with 200ms sleep; no failover, no big.LITTLE pinning verification
3.2 Testing Strategy (Phased)
Phase 1 — Trap the regressions (≤1 week)
- Add unit tests for blake2b with: input > 128 bytes (multi-block), output lengths 1, 16, 33, 63 (mid-range), and output_bytes ∈ {0, 65} rejection throws.
- Add KAT test for aes_decrypt_round (currently uncovered).
- Add direct Blake2Generator::get_byte/get_uint32 tests including the data-refill branch (blake2_generator.cpp:56 — data_index + bytes_needed > data_.size()).
- Register bench_armrx as a烟雾 test in CTest (asserts it runs without crash, no perf assertion — perf belongs in a separate nightly job).
- Add stratum_client happy-path test using a loopback JSON-RPC fake pool (<127.0.0.1:0> listener thread, no real network). Covers: subscribe → authorize → notify → submit → accepted.
Phase 2 — Property & negative path tests
- Property test: forall seed in [0, 2^64): randomx_calculate_hash determinism (same seed → same hash) and bijection-of-prefix up to the nonce.
- Negative path: pool returns malformed JSON ({"id":1,"result":", {"id":1,"method":"mining.notify","params":[]} empty params) — confirm no crash, no UB.
- Pool returns {"error":{"code":-1,"message":"..."}} for submit — confirm error_callback_ invoked with non-empty reason.
- Config parser: missing ~/.config/armrx/config.json, nonexistent --config= path, malformed JSON (missing closing brace, unescaped quotes).
- cpu_features snapshot test (assert that on AArch64 Linux, aes flag matches getauxval(AT_HWCAP) & HWCAP_AES — gives early warning if toolchain dropping auxval).
Phase 3 — JIT-specific tests
- JitCompilerA64::getCodeSize() invariant (jit_compiler_a64.cpp:466 currently returns CodeSize only — fix to include superscalar region — then assert).
- static_assert(kRandomXProgramSize * kMaxInstrEncoding <= RANDOMX_PROGRAM_MAX_SIZE * 16) — currently no such invariant exists (configuration.h:2 vs program.hpp:30-32 vs static.S:281). Critical JIT safety net.
- assert(engine[instr.opcode] != nullptr) before dispatch (jit_compiler_a64.cpp:174, 263) — currently a null member-function pointer is UB if a malformed program arrives.
- W^X transition test: after enableExecution(), assert page is PROT_READ|PROT_EXEC only (via /proc/self/maps parse). Catches the regression at jit_compiler_a64.cpp:135.
- Run the full RandomX KAT suite ("This is a test" and "Lorem ipsum dolor sit amet" at test_blake2b.cpp:191-200) also in interpreted mode with flags & kRandOMXFlagJit = 0. Currently only JIT+hardware-AES path is exercised on AArch64.
Phase 4 — Continuous fuzzing
- libFuzzer target on randomx_calculate_hash(input, len, out) for len ∈ [0, 64] to catch malformed-input dispatch in compile/execute.
- libFuzzer target on json_get/json_str parsers with pool-supplied JSON corpus to surface escape-handling bugs.
3.3 Security Vulnerabilities
S1 (HIGH) — JSON injection in share submission and login messages.
- stratum_client.cpp:358-368, 394-398: wallet/password/job_id are concatenated raw into JSON strings. A wallet string containing ","admin":"1 could break Stratum semantics or, depending on the pool, escalate worker privileges.
- Fix: dedicated json_escape(string_view) -> string helper. Reuse in config.cpp output (currently no output, but planned per #4 of planned_improvements.md).
S2 (HIGH) — W^X violation on Linux AArch64 (the project's primary platform).
- jit_compiler_a64.cpp:135: rwx_ = (setPagesRWX(code, CodeSize + CalcDatasetItemSize) == 0);
- Once the kernel allows it, the JIT page lives RWX for the lifetime of the compiler. The intent of W^X is to forbid exactly this.
- RANDOMX_FORCE_SECURE is defined for OpenBSD/NetBSD/macOS at jit_compiler.hpp:72-74 — but JitCompilerA64 never checks it (confirmed by grep). Upstream XMRig's jit_compiler_a64.hpp:62-64 calls enableExecution() from inside getProgramFunc(); armrx does NOT — silent regression.
- Fix priority 1: Honor RANDOMX_FORCE_SECURE in JitCompilerA64 ctor. Skip the setPagesRWX call when set, accept per-hash setPagesRW/setPagesRX overhead (negligible per improvement.txt 1.3 — and if perf-critical, use pkey_mprotect / PR_SET_MDWE on Linux 6.3+ for hardware-enforced W^X without the syscall cost).
- Fix priority 2: Restore enableExecution() in getProgramFunc() (matches upstream contract).
S3 (CRITICAL) — Out-of-bounds read in fast-mode dataset_read — see §2.1.
S4 (MEDIUM) — setPagesRWX reachable from public API.
- JitCompilerA64::enableAll() at jit_compiler_a64.cpp:155-158 is public (jit_compiler_a64.hpp:69) and unconditionally calls setPagesRWX. Any library consumer can disable W^X post-construction. Make private or delete.
S5 (MEDIUM) — assert() compiles out under NDEBUG.
- aes_hash.cpp:74, 130, 207, 281 use plain assert(output.size() % 64 == 0). Release builds (CMakeLists.txt:13 sets Release) define NDEBUG, so the inputs are never validated in production. A pool-supplied malformed blob that doesn't meet the % 64 == 0 invariant would silently proceed. Use a custom ARMRX_ASSERT that always evaluates.
S6 (MEDIUM) — No TLS certificate verification.
- tls_client.cpp:32: SSL_CTX_set_verify(ctx_.get(), SSL_VERIFY_NONE, nullptr);
- The comment ("Monero pools typically use self-signed or public-CA certs; strict verification would block most") is half true. Modern XMR pools overwhelmingly use Let's Encrypt. Default should be SSL_VERIFY_PEER with --no-verify-tls opt-out for self-signed pools. Pinning hostname verification (SSL_set_verify_result check) prevents MITM.
- Additionally: no minimum TLS version is set. Add SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION).
S7 (LOW) — Unaligned 32-bit JIT store.
- jit_compiler_a64.hpp:84: *(uint32_t*)(code + codePos) = val; — technically UB; AArch64 tolerates it but kernels with SCTLR_EL1.A=1 would trap. The sibling emit64 at :88-92 uses memcpy. Make emit32 consistent.
S8 (LOW) — Dangling pointers across module boundaries.
- vm.cpp:852, 855 — JIT function receives raw cache_->blocks().data() and dataset_.data() + offset_. If the caller destroys Argon2dCache or frees the dataset while holding a VirtualMachine that already JIT-compiled, the next run() reads freed memory. set_cache (vm.cpp:186-195) does not invalidate the JIT. Document the contract in vm.hpp:67-68 and add a clear_cache_references() method, or hold a std::shared_ptr<const Argon2dCache> inside the VM.
3.4 Other Code Quality Issues (debt, not security)
- vm.cpp:200-203 — allocate() is an empty stub with a stale comment ("Scratchpad is already allocated via std::vector resizing in constructor"). The constructor uses mmap (vm.cpp:162-167). Either remove or fix the comment.
- vm.hpp:11 — #include <vector> is unused in the header.
- vm.cpp:1-16 — <iostream>, <iomanip> unused includes.
- vm.cpp:686-687 — case IMUL_RCP: break; is dead — compile path (vm.cpp:414-422) never sets InstructionType::IMUL_RCP.
- vm.cpp:705 — *const_cast<std::uint64_t*>(ibc.isrc) = *ibc.idst; in ISWAP_R. Brittle pattern: works only because isrc happens to point into reg_. Add a std::uint64_t* idst_writable variant or split the union.
- vm.cpp:280, 313, 346, 374, 402, 458 — Six separate static const std::uint64_t zero_val = 0; declarations in different branches. One file-scope static const std::uint64_t kZero = 0; suffices.
- vm.cpp:52-66 vs superscalar.cpp:689-700 — mulh/smulh/signExtend2sCompl duplicated. Lift to shared header.
- include/armrx/tui.hpp:34 — declares void clear_lines(int n); with no definition anywhere. Link error if any caller invoked it; dead API today.
- tui.cpp:74, 79, 88 — prev_lines_ cursor invariant breaks the moment JIT visibility toggles between frames (jit_compile_pct >= 0.0 only on some frames).
- tui.cpp — not thread-safe; enabled_/prev_lines_ plain members, raw std::cout. Document main-thread-only contract or add a mutex.
- superscalar.cpp:101-106 — Ror_rcl, Xor_self, Cmp_ri, Setcc_r, TestJz_fused explicitly labeled // Unused:. Delete.
3.5 Hardcoded Secrets / Unsafe Data Handling
- No hardcoded secrets found in src/ (good).
- changelogs.md and last_con.md are committed to the repo but .gitignore:6 ignores *.txt — inconsistent ignore policy. last_con.md (55 KB) appears to be a conversation log file: confirm it doesn't contain sensitive info and add to .gitignore.
- test_aarch64.cpp and test.bin at repo root appear to be scratch files — .gitignore covers test.bin but not test_aarch64.cpp.
4. Developer Experience (DX) & CI/CD Pipeline
4.1 Current State
- Build: single CMake 3.20+ root with 131 LOC. Builds on x86_64 (interpreted VM only) and AArch64 (JIT + hardware AES).
- No CI (no .github/, no .gitlab-ci.yml, no Makefile).
- No lint/format configured (per AGENTS.md:21).
- No sanitizer builds (ASan/UBSan/TSan) wired into the build system.
- No code coverage tooling.
- Docs are scattered: README.md, AGENTS.md, OPTIMIZATION_REFERENCE.md, planned_improvements.md, improvement.txt, jit_plan.md, REASONIX.md, changelogs.md, last_con.md. Three of these (planned_improvements.md, improvement.txt, jit_plan.md) overlap heavily and have drifted out of sync (e.g. improvement.txt 3.1 says "NEON BLAKE2b unimplemented" but blake2b.cpp:71-181 is the NEON path — task is already done).
- .gitignore:6 ignores *.txt — this hides improvement.txt from git status output even when it's already tracked; misleading.
4.2 Concrete DX Improvements
DX1 — Lint/format
- Add .clang-format (root, consistent with the existing 4-space indent, Allman-ish braces, BindingUtil: false to preserve the SIMD intrinsic line breaks).
- Add .clang-tidy with checks: bugprone-*, cert-*, cppcoreguidelines-pro-type-cstyle-cast, readability-magic-numbers, modernize-deprecated-ios-factory, performance-*.
- Add a format and lint CMake target. Document in AGENTS.md running cmake --build build --target format-check before commit.
DX2 — Sanitizers
- Add ARMRX_ENABLE_ASAN, ARMRX_ENABLE_UBSAN, ARMRX_ENABLE_TSAN CMake options. Pass -fsanitize=address,undefined -fno-omit-frame-pointer to armrx_core and tests. Use AddressSanitizer to immediately surface the S3 dataset_read OOB (vm.cpp:790-795) — currently invisible.
- Use ThreadSanitizer on MiningEngine::start to verify the job_generation_ acquire/release pattern in mining_engine.cpp:201-218 (currently believed correct but unverified under concurrency).
DX3 — CI (GitHub Actions)
.github/workflows/ci.yml:
  matrix:
    - { os: ubuntu-24.04, arch: x86_64,  sanitize: asan+ubsan }
    - { os: ubuntu-24.04, arch: aarch64, sanitize: none,  cross: true }
    - { os: ubuntu-24.04, arch: x86_64,  sanitize: tsan }
  steps:
    - checkout
    - (aarch64) install gcc-aarch64-linux-gnu qemu-user
    - cmake -S . -B build -DARMRX_ENABLE_NATIVE={ON if native else OFF} \
            -DARMRX_BUILD_TESTS=ON \
            -DCMAKE_CXX_FLAGS="$SANITIZE_FLAGS"
    - cmake --build build -j
    - (aarch64) qemu-aarch64 build/armrx_tests
    - ctest --test-dir build --output-on-failure
    - (nightly tag only) run bench_armrx, compare against committed baseline
Add a release.yml that on git tag v* cross-compiles AArch64 static binary, produces .tar.gz, attaches to GitHub Release.
DX4 — Doc consolidation
- Single source of truth: merge planned_improvements.md + improvement.txt + jit_plan.md into ROADMAP.md with three "Status" sections (Done, In Progress, Backlog). Delete the originals or symlink.
- OPTIMIZATION_REFERENCE.md is the only retrospective — keep.
- last_con.md (55 KB conversation log) should be removed from git history or gitignored — clarify with user.
DX5 — Local dev loop
- Add Justfile or Makefile shortcuts: make build, make test, make bench, make asan, make cross-aarch64.
- Add a dev build type preset in CMakePresets.json (CMake 3.21+) with -DARMRX_BUILD_TESTS=ON -DARMRX_ENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=Debug.
5. Future-Proof Roadmap (Step-by-Step Action Plan)
Phase 0 — Immediate (this week, security & correctness)
These are low-effort, high-criticality fixes. Rationale: an OOB read and a JSON injection in shipped code block any production deployment recommendation.
#	Task	Site	Outcome
0.1	Validate set_dataset span size against kRandomXDatasetBytes; refuse undersized	vm.cpp:197-199, vm.cpp:790-795	Closes S3 OOB read
0.2	Add JSON escape helper; escape wallet/password/job_id in TX messages	stratum_client.cpp:358-398, new json.hpp	Closes S1 injection
0.3	Honor RANDOMX_FORCE_SECURE in JitCompilerA64 ctor; restore enableExecution() in getProgramFunc() (upstream parity); make enableAll() private	jit_compiler_a64.cpp:120-158, jit_compiler_a64.hpp:62, 69	Closes S2 W^X
0.4	Add static_assert linking Program::getSize(), RANDOMX_PROGRAM_MAX_SIZE, and worst-case per-instruction emit size (static.S:280's claim enforced)	configuration.h, program.hpp, jit_compiler_a64.cpp	Prevents silent JIT buffer overflow
0.5	Replace assert calls with always-on ARMRX_ASSERT macro	aes_hash.cpp:74, 130, 207, 281; new util.hpp	Inputs validated in Release
0.6	Add assert(engine[instr.opcode] != nullptr) before JIT dispatch	jit_compiler_a64.cpp:174, 263	Prevents UB on malformed programs
Phase 1 — Short-term (next 2 weeks, perf & debt baseline)
#	Task	Site	Expected Outcome
1.1	alignas(16) on RegisterFile — eliminate per-hash 2 KiB copy	vm.hpp:24-29, vm.cpp:961-971	+1–2% h/s, free
1.2	Add JSON smoke test with fake loopback pool	new tests/test_stratum.cpp	First stratum test coverage
1.3	Register bench_armrx in CTest as a smoke test	CMakeLists.txt:113-114	Catches JIT regressions in CI
1.4	Add ASan/UBSan CMake options; build CI matrix on x86_64 with ASan	CMakeLists.txt, new ci.yml	Surfaces the dataset_read OOB and any future OOB
1.5	Replace runtime gf_inverse AES fallback with soft_aes.cpp T-tables	aes.cpp:36-130	100× on non-crypto builds; matches upstream
1.6	Huge pages for 2080 MiB dataset via allocLargePagesMemory	mining_engine.cpp:105, virtual_memory.c:206	-30% TLB pressure in fast mode
1.7	Cache rounding mode in rx_set_rounding_mode (skip if unchanged)	vm.cpp:68-75	Minor h/s, simpler hot loop
1.8	Extract CliParser and PoolManager; have main.cpp actually call apply_cli_overrides (declared at config.hpp:37 but unused)	src/main.cpp, new pool_manager.cpp	Shrinks main from 541 → ~200 lines
1.9	Add .clang-format, .clang-tidy; commit formatted tree	repo root	Consistency baseline for future CI
Phase 2 — Medium-term (next 1–2 months, deeper structural)
#	Task	Site	Expected Outcome
2.1	Refactor compile_instruction 392-line ladder → InstructionHandler[256] dispatch table	vm.cpp:251-642	-50 LOC maintainability, faster compile
2.2	Split VirtualMachine::run into runJit/runInterp; add is_fast_mode() helper	vm.cpp:807-953	Eliminates dual mode-detection source of truth
2.3	Single armrx::json module replacing 3 hand-rolled parsers	new src/json.cpp	Closes all JSON parsing bugs at once, audit-friendly
2.4	NEON Argon2 G-function compression	argon2.cpp:66-108	-20% cache init time (most observable on cold start + new key)
2.5	TLS default: switch to SSL_VERIFY_PEER + min TLS 1.2; add --no-verify-tls opt-out	tls_client.cpp:32	Closes S6 MITM risk
2.6	JIT optimizations A1 + A3 + A4 from jit_plan.md (ubfx scratchpad addr, ld1+sxtl FP loads, next-iter prefetch)	jit_compiler_a64.cpp:591-624, static.S:213-217, 225-227, 341	+3–7% h/s combined
2.7	Property tests for VM determinism + bijection; libFuzzer on randomx_calculate_hash	new tests/test_vm_property.cpp	Catches opcodes that violate spec invariant
2.8	Cross-compile pipeline in CI (x86_64 host → AArch64 binary, run under qemu-user with KAT suite)	.github/workflows/ci.yml	AArch64 regression detection without dedicated runner
Phase 3 — Long-term (3+ months, polish & future-proofing)
#	Task	Rationale
3.1	Status HTTP endpoint (Prometheus scrape format)	planned_improvements.md #4 — needed for production deployment; current TUI is local-only
3.2	Accepted/Rejected share counters surfaced via TUI + HTTP	planned_improvements.md #5; stratum_client.cpp:716-720 already differentiates — wire to atomic counters
3.3	Stratum V2 (Stratum-NextGen) support	planned_improvements.md #6 — required for next-gen Monero pools; non-trivial due to binary protocol + encryption
3.4	Verified RandomX v2 mode path — kRandOMXFlagV2=128 (jit_compiler_a64.cpp:38-58) already plumbed but never validated against upstream v2 KATs	Forward compatibility when Monero upgrades
3.5	Peephole pass for cross-instruction register coalescing in JIT	jit_plan.md "Tier D"; would close most of the remaining 15% gap vs XMRig; major effort
3.6	hwloc topology-aware pinning	Marked "DEFERRED" in OPTIMIZATION_REFERENCE.md; +2–5%; pulls in a dependency
3.7	AddressSanitizer-clean CI baseline	Once Phase 0.1 and Phase 1.4 land, run ASan-enabled CI as a hard gate; any new OOB is auto-detected
3.8	RandomX compliance test suite — automate running upstream reference KATs as part of CI	Currently the KATs are hardcoded in test_blake2b.cpp; a continuous run against the upstream randomx-tests repo would catch spec drift
Verification Plan
After each phase lands:
Phase	Verification
0	ctest --output-on-failure passes; ASan build on x86_64 passes (no vm.cpp:793 UBSan/ASan complaint with deliberately undersized dataset)
1	bench_armrx parity with baseline (≥22 H/s on Cortex-A53 reference); no new KAT failures; CI green on x86_64 + cross-compiled AArch64
2	All KATs still pass; perf ≥ 23 H/s (3-5% improvement); ASan-clean CI is permanently green
3	HTTP endpoint returns non-empty JSON; Stratum V2 KATs pass; perf ≥ 24 H/s (~10% improvement vs baseline)