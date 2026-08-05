cmake_minimum_required(VERSION 3.20)

project(armrx VERSION 0.2.0 LANGUAGES C CXX ASM)

# Embed git SHA and build date for --version flag
# .git_sha is written by devbox_sync or the build host
file(READ .git_sha ARMRX_GIT_SHA)
string(STRIP "${ARMRX_GIT_SHA}" ARMRX_GIT_SHA)
if(ARMRX_GIT_SHA STREQUAL "")
    set(ARMRX_GIT_SHA "unknown")
endif()
string(TIMESTAMP ARMRX_BUILD_DATE "%Y-%m-%d" UTC)
add_compile_definitions(ARMRX_VERSION="${PROJECT_VERSION}"
    ARMRX_GIT_SHA="${ARMRX_GIT_SHA}"
    ARMRX_BUILD_DATE="${ARMRX_BUILD_DATE}")

option(ARMRX_ENABLE_NATIVE "Tune for the compiler host CPU" OFF)
option(ARMRX_BUILD_TESTS "Build unit tests" ON)
option(ARMRX_BUILD_FUZZERS "Build libFuzzer harnesses (requires Clang; PLAN.md §3.1)" OFF)
option(ARMRX_JIT_PROFILE "Enable per-hash JIT profiling (adds 3 clock_gettime syscalls per hash)" OFF)
option(ARMRX_ENABLE_JIT_FAST_DIV_SQRT "Enable fast Newton-Raphson JIT FDIV and FSQRT" OFF)
option(ARMRX_ENABLE_NEON_AES "Use NEON vector-permute AES instead of scalar T-table lookups (experimental, needs on-device hashrate verification -- see docs/experiments/neon-vector-permute-aes.md)" OFF)
option(ARMRX_ENABLE_NEON_TTABLE_AES "Use NEON-vectorized T-table AES round functions for x4 hash/fill paths" ON)
option(ARMRX_FAST_MATH "Enable -Ofast / -ffast-math for floating-point speed" OFF)
option(ARMRX_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ARMRX_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ARMRX_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
set(ARMRX_PGO "OFF" CACHE STRING "Profile-Guided Optimization: OFF, GENERATE, or USE")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

add_library(armrx_core
    src/aes_generator.cpp
    src/argon2.cpp
    src/blake2_generator.cpp
    src/blake2b.cpp
    src/cpu_features.cpp
    src/cpu_thermal.cpp
    src/dataset.cpp
    src/memory.cpp
    src/superscalar.cpp
    src/vm.cpp
    src/aes_hash.cpp
    src/mining_engine.cpp
    src/soft_aes.cpp
    src/stratum_client.cpp
    src/config.cpp
    src/json.cpp
    src/pool_manager.cpp
    src/tui.cpp
    src/partial_dataset.cpp
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64|aarch64_be")
    target_sources(armrx_core PRIVATE
        src/jit_compiler_a64.cpp
        src/jit_compiler_a64_static.S
        src/jit_dataset_2way.cpp
        src/virtual_memory.c
    )
    target_compile_definitions(armrx_core PUBLIC ARMRX_HAVE_JIT=1)
    set(ARMRX_HAVE_JIT TRUE)
    # Enable hardware AES and NEON crypto extensions on AArch64
    target_compile_options(armrx_core PRIVATE -march=armv8-a+crypto)
else()
    target_sources(armrx_core PRIVATE
        src/virtual_memory.c
    )
endif()

find_package(Threads REQUIRED)

# Optional OpenSSL for TLS pool connections
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_sources(armrx_core PRIVATE src/tls_client.cpp)
    target_link_libraries(armrx_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(armrx_core PUBLIC ARMRX_HAVE_TLS=1)
    message(STATUS "OpenSSL found — TLS pool connections enabled")
else()
    message(STATUS "OpenSSL not found — TLS pool connections disabled")
endif()

# Optional hwloc for topology-aware CPU pinning
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(HWLOC hwloc QUIET)
endif()
if(HWLOC_FOUND OR NOT PkgConfig_FOUND)
    if(NOT HWLOC_FOUND)
        find_path(HWLOC_INCLUDE_DIRS hwloc.h)
        find_library(HWLOC_LIBRARIES hwloc)
    endif()
    if(HWLOC_INCLUDE_DIRS AND HWLOC_LIBRARIES)
        target_include_directories(armrx_core PRIVATE ${HWLOC_INCLUDE_DIRS})
        target_link_libraries(armrx_core PRIVATE ${HWLOC_LIBRARIES})
        target_compile_definitions(armrx_core PUBLIC ARMRX_HAVE_HWLOC=1)
        message(STATUS "hwloc found — topology-aware CPU pinning enabled")
    else()
        message(STATUS "hwloc not found — using sysfs-based CPU pinning")
    endif()
else()
    message(STATUS "hwloc not found — using sysfs-based CPU pinning")
endif()

target_include_directories(armrx_core PUBLIC include)
target_link_libraries(armrx_core PUBLIC Threads::Threads)
target_compile_features(armrx_core PUBLIC cxx_std_20)
target_compile_options(armrx_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=fast -funroll-loops -frounding-math)

# -fvisibility=hidden + -fno-semantic-interposition: on PIE builds (default on
# many Linux distros), these let the compiler inline across translation units
# more aggressively and drop PLT indirection for intra-library calls.
# Confirmed absent from this build (docs/plans/experimental-performance-ideas-20260725.md #6).
target_compile_options(armrx_core PRIVATE -fvisibility=hidden -fno-semantic-interposition)

if(NOT ARMRX_ENABLE_ASAN)
    # ASAN needs frame pointers for its stack traces (forced back on via
    # -fno-omit-frame-pointer below in that block); everywhere else, skip
    # the x29/x30 save/restore pair per call. Confirmed genuinely untried:
    # 219/523 functions in a release build carry a frame-pointer prologue
    # (docs/plans/experimental-performance-ideas-20260725.md #12).
    target_compile_options(armrx_core PRIVATE -fomit-frame-pointer)
endif()

if(ARMRX_ENABLE_NATIVE)
    target_compile_options(armrx_core PRIVATE -mcpu=native)
endif()

if(ARMRX_FAST_MATH)
    message(STATUS "Fast math enabled (-Ofast)")
    target_compile_options(armrx_core PRIVATE -Ofast)
endif()

# Profile-Guided Optimization (PGO) — GCC only
# Note: LTO must be disabled when using PGO (LTO+gcov conflict on GCC 15)
if(ARMRX_PGO STREQUAL "GENERATE")
    message(STATUS "PGO: generating profile data (LTO disabled)")
    target_compile_options(armrx_core PUBLIC -fprofile-generate -fno-lto)
    target_link_options(armrx_core PUBLIC -fprofile-generate -fno-lto)
elseif(ARMRX_PGO STREQUAL "USE")
    message(STATUS "PGO: using profile data from build directory (LTO disabled)")
    target_compile_options(armrx_core PUBLIC -fprofile-use -fprofile-correction -fno-lto)
    target_link_options(armrx_core PUBLIC -fprofile-use -fno-lto)
endif()

if(ARMRX_JIT_PROFILE)
    target_compile_definitions(armrx_core PUBLIC ARMRX_JIT_PROFILE)
    message(STATUS "JIT profiling enabled — adds 3 clock_gettime syscalls per hash")
endif()

if(ARMRX_ENABLE_JIT_FAST_DIV_SQRT)
    target_compile_definitions(armrx_core PRIVATE ARMRX_JIT_FAST_DIV_SQRT)
    message(STATUS "Fast JIT FDIV and FSQRT (Newton-Raphson) enabled")
endif()

if(ARMRX_ENABLE_NEON_AES)
    target_compile_definitions(armrx_core PRIVATE ARMRX_ENABLE_NEON_AES)
    message(STATUS "NEON vector-permute AES enabled (experimental)")
endif()

if(ARMRX_ENABLE_NEON_TTABLE_AES)
    target_compile_definitions(armrx_core PRIVATE ARMRX_ENABLE_NEON_TTABLE_AES)
    message(STATUS "NEON T-table AES enabled (experimental)")
endif()

if(ARMRX_ENABLE_ASAN)
    # PUBLIC, not PRIVATE: armrx_core is a static library, and executables
    # linking against it (armrx, all tests/benchmarks) need the sanitizer
    # runtime pulled into *their* link step too, or the link fails with
    # undefined references to __asan_* symbols coming from armrx_core's
    # instrumented object files. PRIVATE here silently only worked for
    # nothing, since armrx_core itself isn't a final linked binary.
    target_compile_options(armrx_core PUBLIC -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(armrx_core PUBLIC -fsanitize=address)
    message(STATUS "AddressSanitizer enabled")
endif()

if(ARMRX_ENABLE_UBSAN)
    target_compile_options(armrx_core PUBLIC -fsanitize=undefined)
    target_link_options(armrx_core PUBLIC -fsanitize=undefined)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
endif()

if(ARMRX_ENABLE_TSAN)
    target_compile_options(armrx_core PUBLIC -fsanitize=thread)
    target_link_options(armrx_core PUBLIC -fsanitize=thread)
    message(STATUS "ThreadSanitizer enabled")
endif()

add_executable(armrx src/main.cpp src/cli_parser.cpp src/miner_app.cpp)
target_link_libraries(armrx PRIVATE armrx_core)

if(ARMRX_BUILD_TESTS)
    enable_testing()
    # -UNDEBUG: these two test executables verify correctness with plain
    # assert() (including the RandomX KATs in test_blake2b.cpp). The default
    # build type is Release, which defines NDEBUG and silently compiles every
    # assert() out — the test would report success without checking anything.
    # Force assert() to stay live here regardless of overall build type; this
    # only affects these two translation units, not armrx_core/armrx itself.
    add_executable(armrx_tests tests/test_blake2b.cpp)
    target_link_libraries(armrx_tests PRIVATE armrx_core)
    target_compile_options(armrx_tests PRIVATE -UNDEBUG)
    add_test(NAME armrx_tests COMMAND armrx_tests)

    add_executable(test_mining tests/test_mining.cpp)
    target_link_libraries(test_mining PRIVATE armrx_core)
    target_compile_options(test_mining PRIVATE -UNDEBUG)
    add_test(NAME test_mining COMMAND test_mining)
    # Builds several full-size (~2080 MiB) fast-mode datasets to cover the
    # live worker-reuse dataset-rebuild path (PLAN.md §2.1) — generous ceiling
    # so a genuine hang fails loudly instead of blocking CI/ctest forever.
    set_tests_properties(test_mining PROPERTIES TIMEOUT 900)

    add_executable(bench_armrx tests/bench_armrx.cpp)
    target_link_libraries(bench_armrx PRIVATE armrx_core)
    add_test(NAME bench_armrx COMMAND bench_armrx)

    add_executable(test_config tests/test_config.cpp)
    target_link_libraries(test_config PRIVATE armrx_core)
    target_compile_options(test_config PRIVATE -UNDEBUG)
    add_test(NAME test_config COMMAND test_config)

    add_executable(test_partial_dataset tests/test_partial_dataset.cpp)
    target_link_libraries(test_partial_dataset PRIVATE armrx_core)
    target_compile_options(test_partial_dataset PRIVATE -UNDEBUG)

    # cli_parser.cpp lives outside armrx_core (it's compiled directly into the
    # armrx executable, see the add_executable(armrx ...) line below), so this
    # test binary compiles it directly rather than linking a library that
    # doesn't contain it.
    add_executable(test_cli_parser tests/test_cli_parser.cpp src/cli_parser.cpp)
    target_link_libraries(test_cli_parser PRIVATE armrx_core)
    target_compile_options(test_cli_parser PRIVATE -UNDEBUG)
    add_test(NAME test_cli_parser COMMAND test_cli_parser)

    add_executable(test_aes_hash tests/test_aes_hash.cpp)
    target_link_libraries(test_aes_hash PRIVATE armrx_core)
    target_compile_options(test_aes_hash PRIVATE -UNDEBUG)
    add_test(NAME test_aes_hash COMMAND test_aes_hash)
    add_test(NAME test_partial_dataset COMMAND test_partial_dataset)

    # Mock Stratum protocol tests (PLAN.md §3.2) — loopback TCP, no hardware
    # dependency. test_pool_failover() dominates: ~31s real production
    # backoff timing plus up to ~30s more from a stale reconnect thread's
    # blocking join in PoolManager::connect_to_current() (see the comment on
    # that scenario in the test file) — generous ceiling for slower/loaded
    # on-device hardware.
    add_executable(test_pool_protocol tests/test_pool_protocol.cpp)
    target_link_libraries(test_pool_protocol PRIVATE armrx_core)
    target_compile_options(test_pool_protocol PRIVATE -UNDEBUG)
    add_test(NAME test_pool_protocol COMMAND test_pool_protocol)
    set_tests_properties(test_pool_protocol PROPERTIES TIMEOUT 300)

    # Opcode frequency benchmark (not part of regular CTest; invoke manually)
    # JIT-only — uses JitDumpEntry/setJitDumpEnabled/getJitDump (ARMRX_HAVE_JIT)
    if(ARMRX_HAVE_JIT)
    add_executable(bench_opcodes tests/bench_opcodes.cpp)
    target_link_libraries(bench_opcodes PRIVATE armrx_core)
    add_test(NAME bench_opcodes COMMAND bench_opcodes)

    # JIT encoding and determinism tests
    add_executable(test_jit_encodings tests/test_jit_encodings.cpp)
    target_link_libraries(test_jit_encodings PRIVATE armrx_core)
    add_test(NAME test_jit_encodings COMMAND test_jit_encodings)

    add_executable(test_jit_determinism tests/test_jit_determinism.cpp)
    target_link_libraries(test_jit_determinism PRIVATE armrx_core)
    add_test(NAME test_jit_determinism COMMAND test_jit_determinism)

    add_executable(test_jit_equivalence tests/test_jit_equivalence.cpp)
    target_link_libraries(test_jit_equivalence PRIVATE armrx_core)
    add_test(NAME test_jit_equivalence COMMAND test_jit_equivalence)
    # A prior CBRANCH encoding bug caused an infinite loop that only showed up
    # as a 120s hang (docs/branchless-cbranch.md) — bound this test explicitly
    # so a future CBRANCH regression fails fast instead of hanging forever.
    set_tests_properties(test_jit_equivalence PROPERTIES TIMEOUT 600)

    # W3-2 bisection harness: regression guard for the ARMRX_MAX_SWAPS swap-budget
    # hook. Verifies the emitted order stays a valid permutation (JIT == interpreter)
    # across multiple swap budgets. AArch64-only (depends on the JIT scheduler).
    add_executable(test_scheduler_bisect tests/test_scheduler_bisect.cpp)
    target_link_libraries(test_scheduler_bisect PRIVATE armrx_core)
    add_test(NAME test_scheduler_bisect COMMAND test_scheduler_bisect)
    set_tests_properties(test_scheduler_bisect PROPERTIES TIMEOUT 600)

    # Large-N differential stress test for the emitter lookahead scheduler
    # (PLAN.md Phase 6 item 12/L1) — see the test file's own header comment
    # for why 16 fixed seeds aren't enough coverage for this specific change.
    add_executable(test_jit_scheduler_stress tests/test_jit_scheduler_stress.cpp)
    target_link_libraries(test_jit_scheduler_stress PRIVATE armrx_core)
    add_test(NAME test_jit_scheduler_stress COMMAND test_jit_scheduler_stress)
    # Measured on-device (2026-07-25): ~7.2s/cache-init x3 + ~2.6s/hash-pair
    # (interpreter-dominated) x450 pairs =~ 1290s -- the original 1200s budget
    # was too tight and caused a spurious ctest TIMEOUT with zero hash
    # mismatches reported, not a real hang. 2400s gives headroom for the
    # device's documented thermal variance between runs.
    set_tests_properties(test_jit_scheduler_stress PROPERTIES TIMEOUT 2400)

    # Seed-diversity differential stress test for the superscalar/dataset-
    # derivation scheduler extension (PLAN.md Phase 6 item 12, 2026-07-25) —
    # generateSuperscalarHash() compiles once per seed rotation, so unlike
    # the main-program scheduler above, coverage here needs many distinct
    # seeds, not many inputs against few seeds. See the test file's own
    # header comment.
    add_executable(test_jit_superscalar_scheduler_stress tests/test_jit_superscalar_scheduler_stress.cpp)
    target_link_libraries(test_jit_superscalar_scheduler_stress PRIVATE armrx_core)
    add_test(NAME test_jit_superscalar_scheduler_stress COMMAND test_jit_superscalar_scheduler_stress)
    # Same per-unit costs as test_jit_scheduler_stress above (~7.2s cache
    # init, ~2.6s/hash-pair interpreter-dominated): 100 seeds x (7.2s +
    # 2x2.6s) =~ 1240s. 2400s budget matches the sibling test for the same
    # thermal-variance headroom reasoning.
    set_tests_properties(test_jit_superscalar_scheduler_stress PROPERTIES TIMEOUT 2400)

    # NEON vector-permute AES parity tests (PLAN.md Phase 5 / NEXT_STEPS.md §5a) — AArch64/NEON-
    # only, always exercises both encrypt_transform/decrypt_transform (scalar) and
    # encrypt_transform_neon/decrypt_transform_neon directly by name, independent of whether
    # ARMRX_ENABLE_NEON_AES is set (that flag only controls production dispatch).
    add_executable(test_aes_neon tests/test_aes_neon.cpp)
    target_link_libraries(test_aes_neon PRIVATE armrx_core)
    target_compile_options(test_aes_neon PRIVATE -UNDEBUG)
    add_test(NAME test_aes_neon COMMAND test_aes_neon)

    # Track D1 (docs/plans/track-d1-superscalar-interleave-plan-20260728.md):
    # exhaustive differential test for the new, standalone 2-way interleaved
    # dataset-item derivation entry point. See the test file's own header
    # comment for the oracle and seed-diversity rationale.
    add_executable(test_jit_dataset_2way tests/test_jit_dataset_2way.cpp)
    target_link_libraries(test_jit_dataset_2way PRIVATE armrx_core)
    target_compile_options(test_jit_dataset_2way PRIVATE -UNDEBUG)
    add_test(NAME test_jit_dataset_2way COMMAND test_jit_dataset_2way)

    # Track D1's actual decision gate (L1 I-cache refill rate, 1-way vs
    # 2-way) -- perf-stat-wrapped microbenchmark invoked manually on device,
    # not part of the default ctest run (requires mode+iteration args; see
    # the file's own header comment).
    add_executable(bench_dataset_2way tests/bench_dataset_2way.cpp)
    target_link_libraries(bench_dataset_2way PRIVATE armrx_core)

    # Diagnostic (not a shipped test): times the PartialDataset fill for a given
    # --dataset-mb, isolating the "20-minute warmup" hypothesis from steady-state
    # hashing. Run on-device: ./time_partial_fill 512
    add_executable(time_partial_fill tools/time_partial_fill.cpp)
    target_link_libraries(time_partial_fill PRIVATE armrx_core)

    # Diagnostic (not a shipped test): verifies the light-mode seed-rotation
    # partial-dataset rebuild end-to-end, in SEPARATE PROCESSES for the engine
    # vs reference VM so no JIT program-cache is shared. Run on-device:
    #   ./verify_seed_rotation engine <nonce> <seedB_hex> [partial_items]
    #   ./verify_seed_rotation ref    <nonce> <seedB_hex>
    # Compare the HASH= lines; they must match (proving the engine rotated).
    add_executable(verify_seed_rotation tools/verify_seed_rotation.cpp)
    target_link_libraries(verify_seed_rotation PRIVATE armrx_core)
    target_compile_options(verify_seed_rotation PRIVATE -UNDEBUG)
    endif()

    # IMUL_R operand-magnitude sampler (T3-3 gate check) — interpreter-only, host-runnable
    add_executable(bench_imul_magnitudes tests/bench_imul_magnitudes.cpp)
    target_link_libraries(bench_imul_magnitudes PRIVATE armrx_core)
    add_test(NAME bench_imul_magnitudes COMMAND bench_imul_magnitudes 8 20)
endif()

# LibFuzzer harness for armrx::json (PLAN.md §3.1). Opt-in and Clang-only —
# -fsanitize=fuzzer isn't supported by GCC, which is what the rest of this
# project builds with. Compiles src/json.cpp directly (that module has no
# dependency on the rest of armrx_core) rather than linking against the
# GCC-built armrx_core static library, avoiding any question of GCC/Clang
# object compatibility entirely. Not part of the default ctest run — fuzzing
# is exploratory/unbounded, invoke the binary directly with -max_total_time=N.
if(ARMRX_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR
            "ARMRX_BUILD_FUZZERS requires Clang (-fsanitize=fuzzer is not supported by GCC). "
            "Reconfigure with -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang.")
    endif()
    add_executable(fuzz_json tests/fuzz_json.cpp src/json.cpp)
    target_include_directories(fuzz_json PRIVATE include)
    target_compile_features(fuzz_json PRIVATE cxx_std_20)
    target_compile_options(fuzz_json PRIVATE -fsanitize=fuzzer,address -g -O1)
    target_link_options(fuzz_json PRIVATE -fsanitize=fuzzer,address)
endif()

# Enable Interprocedural Optimization (LTO) if supported
option(ARMRX_DISABLE_LTO "Disable LTO/IPO even if supported (workaround for GCC 15 + musl crashes)" OFF)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported AND NOT ARMRX_PGO STREQUAL "GENERATE" AND NOT ARMRX_PGO STREQUAL "USE" AND NOT ARMRX_DISABLE_LTO)
    message(STATUS "LTO/IPO is supported and enabled")
    set_property(TARGET armrx_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET armrx PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    # Alpine/musl's fortify-headers package wraps libc calls (e.g. vsnprintf, reached via
    # std::to_string(double) -> libstdc++'s __to_xstring) in `extern` + `always_inline`
    # functions. Under LTO, GCC can't always prove it holds the sole authoritative
    # definition of an extern always_inline function, so it hard-errors instead of
    # emitting an out-of-line call ("function body can be overwritten at link time").
    # Confirmed via bisection this is unrelated to any armrx code path or the
    # cli_parser.cpp/miner_app.cpp split (reproduces identically on the pre-split
    # tree); it is purely this toolchain/libc combination. -flto-partition=one does
    # NOT fix it either (verified), so this isn't a WHOPR partitioning artifact.
    # Disabling fortify only on the one target that hits this, rather than dropping
    # LTO project-wide, keeps the rest of the build's hardening intact.
    target_compile_options(armrx PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
    if(ARMRX_BUILD_TESTS)
        set_property(TARGET armrx_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET test_mining PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET bench_armrx PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET test_config PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET test_cli_parser PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET test_aes_hash PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        if(ARMRX_HAVE_JIT)
            set_property(TARGET bench_opcodes PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET test_jit_encodings PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET test_jit_determinism PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET test_jit_equivalence PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET test_jit_scheduler_stress PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET test_jit_superscalar_scheduler_stress PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET test_aes_neon PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    endif()
elseif(NOT ipo_supported)
    message(STATUS "LTO/IPO is not supported: ${ipo_error}")
endif()
