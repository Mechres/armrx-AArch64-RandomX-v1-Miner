/**
 * Armrx benchmark protocol v2 — measurement foundation.
 *
 * Addresses known issues from v1:
 *  - Proper statistical reporting (median, min, max, IQR) over multiple samples
 *  - Deterministic precomputed random index sequences (no fixed cache-line-42 trap)
 *  - Honest benchmark sizing (actual 2 MiB AES fill, not 64 bytes)
 *  - Region attribution: separate phases of the hash pipeline
 *  - JIT compile vs execute separation (when built with ARMRX_JIT_PROFILE)
 *  - Configurable sample count and output format
 *
 * Build: cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON
 *        cmake --build build -j
 *        ./build/bench_armrx
 *
 * For per-phase JIT timing: cmake -S . -B build -DARMRX_JIT_PROFILE=ON ...
 *
 * Run under perf stat for per-region PMU counters:
 *   perf stat ./build/bench_armrx --attribution-only
 *   perf stat ./build/bench_armrx --full-hash-only
 *   perf stat -p $(pgrep ...) via tools/perf_ready_bench.sh (clean steady-state capture)
 *   Run: ./build/bench_armrx --full-hash-only --perf-ready  (see tools/perf_ready_bench.sh)
 */

#include "armrx/argon2.hpp"
#include "armrx/superscalar.hpp"
#include "armrx/dataset.hpp"
#include "armrx/vm.hpp"
#include "armrx/aes_hash.hpp"
#include "armrx/blake2b.hpp"
#include "armrx/randomx_config.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <span>
#include <array>
#include <cmath>
#include <random>
#include <string>
#include <numeric>
#include <cstdint>
#include <cfenv>
#include <poll.h>
#include <unistd.h>

#ifdef ARMRX_HAVE_JIT
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#endif

namespace {

using bench_clock = std::chrono::steady_clock;

// ============================================================================
// Statistics helpers
// ============================================================================

struct BenchmarkResult {
    std::string name;
    std::string unit;
    double      median_us;      // median μs per operation
    double      min_us;         // fastest sample
    double      max_us;         // slowest sample
    double      mean_us;        // arithmetic mean
    double      stddev_pct;     // relative standard deviation (%)
    double      throughput;     // operations per second (scaled)
    unsigned    samples;        // number of samples collected
};

/** Collect N timing samples of `fn()`, return statistics. */
template<typename F>
BenchmarkResult sample_benchmark(const char* name, unsigned samples,
                                 unsigned warmup_samples, F&& fn,
                                 const char* unit = "ops", double scale = 1.0) {
    // Warmup
    for (unsigned i = 0; i < warmup_samples; ++i) fn();

    std::vector<double> ns_samples;
    ns_samples.reserve(samples);

    for (unsigned i = 0; i < samples; ++i) {
        auto t0 = bench_clock::now();
        fn();
        auto t1 = bench_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        ns_samples.push_back(static_cast<double>(ns));
    }

    std::sort(ns_samples.begin(), ns_samples.end());

    double median_ns = ns_samples[samples / 2];
    double min_ns    = ns_samples.front();
    double max_ns    = ns_samples.back();
    double sum_ns    = std::accumulate(ns_samples.begin(), ns_samples.end(), 0.0);
    double mean_ns   = sum_ns / samples;

    // Population stddev
    double sq_sum = 0.0;
    for (auto ns : ns_samples) {
        double d = ns - mean_ns;
        sq_sum += d * d;
    }
    double stddev_ns = std::sqrt(sq_sum / samples);
    double stddev_pct = (mean_ns > 0.0) ? (stddev_ns / mean_ns * 100.0) : 0.0;

    double median_us = median_ns / 1000.0;
    double min_us    = min_ns / 1000.0;
    double max_us    = max_ns / 1000.0;
    double mean_us   = mean_ns / 1000.0;
    double throughput = (median_ns > 0.0) ? (1e9 / median_ns * scale) : 0.0;

    return {name, unit, median_us, min_us, max_us, mean_us, stddev_pct, throughput, samples};
}

void print_result(const BenchmarkResult& r) {
    std::cout << std::left << std::setw(44) << r.name
              << std::right << std::fixed << std::setprecision(2)
              << std::setw(10) << r.median_us << " μs  "
              << std::setw(8) << r.throughput << " " << r.unit << "/s  "
              << "[min " << r.min_us << " / max " << r.max_us
              << " μs, σ " << std::setprecision(1) << r.stddev_pct
              << "%, n=" << r.samples << "]\n";
}

void print_header(const char* title) {
    std::cout << "\n─── " << title << " ─────────────────────────────────────────────\n\n";
}

// ============================================================================
// Deterministic random-index generator
// ============================================================================

/**
 * Precompute a deterministic sequence of pseudorandom indices.
 * Uses a fixed-seed std::mt19937 for reproducibility across runs and builds.
 */
std::vector<std::size_t> make_index_sequence(std::size_t count, std::size_t range, std::uint64_t seed = 0xDEADBEEF) {
    std::mt19937_64 rng(seed);
    std::vector<std::size_t> indices;
    indices.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        indices.push_back(static_cast<std::size_t>(rng() % range));
    }
    return indices;
}

// ============================================================================
// Benchmark suites
// ============================================================================

void bench_blake2b() {
    print_header("1. Blake2b");
    std::array<std::byte, 64> blake_input{};
    std::array<std::byte, 64> blake_output{};

    auto r = sample_benchmark("blake2b (64-in, 64-out)", 500, 10, [&] {
        auto res = armrx::blake2b(std::span<const std::byte>(blake_input), 64);
        std::memcpy(blake_output.data(), res.data(), 64);
    }, "hash");
    print_result(r);
}

void bench_aes_primitives() {
    print_header("2. AES scratchpad operations");
    armrx::AesState aes_state{};

    // Benchmark 2 MiB scratchpad fill (realistic size)
    std::vector<std::byte> aes_buf_2mib(2097152, std::byte{0});
    auto r_fill = sample_benchmark("fill_aes_1r_x4 (2 MiB)", 30, 3, [&] {
        armrx::fill_aes_1r_x4(aes_state, std::span<std::byte>(aes_buf_2mib));
    }, "fill", 1.0);
    print_result(r_fill);

    // Benchmark hash_aes_1r_x4 (finalization step)
    auto r_hash = sample_benchmark("hash_aes_1r_x4 (2 MiB)", 30, 3, [&] {
        armrx::hash_aes_1r_x4(std::span<const std::byte>(aes_buf_2mib), aes_state);
    }, "finalize", 1.0);
    print_result(r_hash);

    // hash_and_fill_aes_1r_x4: fuses the two traversals above into one pass
    // over the scratchpad (KAT-pinned equivalence in test_aes_hash.cpp), but
    // unused by production mining today. This benchmark measures the real
    // primitive-level saving before any mining-engine integration is
    // attempted (PLAN.md Phase 6 item 11's "bench first" prerequisite).
    armrx::AesState fused_hash_state{};
    armrx::AesState fused_fill_state{};
    auto r_fused = sample_benchmark("hash_and_fill_aes_1r_x4 (2 MiB, fused)", 30, 3, [&] {
        armrx::hash_and_fill_aes_1r_x4(std::span<std::byte>(aes_buf_2mib), fused_hash_state, fused_fill_state);
    }, "fused-op", 1.0);
    print_result(r_fused);

    // Direct comparison: two separate full 2 MiB traversals back to back,
    // mirroring exactly what the mining hot path does today at a nonce
    // boundary (hash nonce N's final scratchpad, then fill nonce N+1's
    // initial scratchpad) -- same total work as the fused call above, done
    // the unfused way, so the two lines are directly comparable.
    armrx::AesState separate_hash_state{};
    armrx::AesState separate_fill_state{};
    auto r_separate = sample_benchmark("hash_aes_1r_x4 + fill_aes_1r_x4 (2x 2 MiB, separate)", 30, 3, [&] {
        armrx::hash_aes_1r_x4(std::span<const std::byte>(aes_buf_2mib), separate_hash_state);
        armrx::fill_aes_1r_x4(separate_fill_state, std::span<std::byte>(aes_buf_2mib));
    }, "separate-op", 1.0);
    print_result(r_separate);
}

void bench_dataset_helpers() {
    print_header("3. Dataset helpers (light-mode primitives)");

    std::vector<std::byte> seed_key = {std::byte{0x00}, std::byte{0x11}};
    armrx::Argon2dCache cache;
    cache.initialize(seed_key);

    std::size_t n_lines = armrx::cache_line_count(cache);
    std::size_t n_items = armrx::randomx_dataset_item_count();

    constexpr std::size_t kNumAccesses = 5000;
    auto line_indices  = make_index_sequence(kNumAccesses, n_lines, 0xA11CE);
    auto item_indices  = make_index_sequence(kNumAccesses, n_items, 0x17E5E);

    // Cache line reads with random access pattern
    std::size_t li = 0;
    auto r_line = sample_benchmark("load_cache_line (random)", kNumAccesses, 50, [&] {
        (void)armrx::load_cache_line(cache, line_indices[li++ % kNumAccesses]);
    }, "line", 1.0);
    print_result(r_line);

    // Dataset item generation with random item numbers
    std::size_t ii = 0;
    auto r_item = sample_benchmark("generate_dataset_item (random)", kNumAccesses, 50, [&] {
        (void)armrx::generate_dataset_item(cache, item_indices[ii++ % kNumAccesses]);
    }, "item", 1.0);
    print_result(r_item);

    // Dataset initialization of 5000 items (light-mode startup cost)
    std::vector<std::byte> dataset_buf(5000 * armrx::kRandomXDatasetItemBytes);
    auto r_init = sample_benchmark("initialize_dataset (5000 items)", 5, 2, [&] {
        armrx::initialize_dataset(dataset_buf, cache, 0, 5000);
    }, "batch", 1.0);
    print_result(r_init);
}

void bench_argon2_compress() {
    print_header("3b. Argon2 compression (Argon2dCache::initialize hot path)");

    // Chain compress calls (each feeding the next) so the compiler can't
    // elide the work and so this mirrors the dependent-block chain that
    // Argon2dCache::initialize walks during real cache init.
    armrx::Argon2Block a{};
    armrx::Argon2Block b{};
    for (std::size_t i = 0; i < a.size(); ++i) { a[i] = i; b[i] = i * 3 + 1; }

    constexpr std::size_t kSamples = 5000;
    auto r_compress = sample_benchmark("argon2_compress (single block)", kSamples, 200, [&] {
        a = armrx::argon2_compress(a, b);
    }, "compress", 1.0);
    print_result(r_compress);
}

void bench_argon2_cache_init() {
    print_header("3c. Argon2dCache::initialize() — isolated full light-mode cache init");

    // Reuse one allocation across all calls (matches how the constructor's
    // mmap/munmap cost is separate from the per-seed-change compute cost
    // this benchmark exists to isolate) and vary the key per call so the
    // compiler/predictor doesn't see identical input on every iteration.
    armrx::Argon2dCache cache; // default: 262144 blocks (256 MiB), 3 passes — real light-mode size
    unsigned key_counter = 0;
    auto r = sample_benchmark("Argon2dCache::initialize (light, 256 MiB)", 3, 1, [&] {
        const std::array<std::byte, 4> key{
            std::byte{'k'}, std::byte{'e'}, std::byte{'y'}, static_cast<std::byte>(key_counter++)};
        cache.initialize(key);
    }, "init", 1.0);
    print_result(r);
}

#ifdef ARMRX_HAVE_JIT
// Maps a small physical backing (`alias_bytes`) repeatedly across a much
// larger virtual address range (`virtual_bytes`), so every offset the JIT
// program computes into the "scratchpad" lands on the same small physical
// footprint -- without touching any of the JIT's own address-masking logic.
// Used to bound how much of the main VM program's ~2.2x IPC penalty
// (docs/plans/performance-plan-20260725.md Step 1) is recoverable: if
// forcing the scratchpad to be effectively L1-resident closes most of the
// gap, the penalty is a real, fixable memory-latency stall; if IPC barely
// moves, the penalty is architectural (pipeline depth vs. any memory
// latency), not something a code change can chase further.
struct AliasedScratchpad {
    std::byte*  base          = nullptr;
    std::size_t virtual_bytes = 0;
    int         fd            = -1;

    static AliasedScratchpad create(std::size_t alias_bytes, std::size_t virtual_bytes) {
        AliasedScratchpad result;
        result.virtual_bytes = virtual_bytes;

        int fd = static_cast<int>(::memfd_create("armrx_l1_bench", 0));
        if (fd < 0) { std::perror("memfd_create"); std::exit(1); }
        if (::ftruncate(fd, static_cast<off_t>(alias_bytes)) != 0) { std::perror("ftruncate"); std::exit(1); }

        void* reservation = ::mmap(nullptr, virtual_bytes, PROT_NONE,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (reservation == MAP_FAILED) { std::perror("mmap (reserve)"); std::exit(1); }

        for (std::size_t off = 0; off < virtual_bytes; off += alias_bytes) {
            void* tile = ::mmap(static_cast<std::byte*>(reservation) + off, alias_bytes,
                                 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
            if (tile == MAP_FAILED) { std::perror("mmap (tile)"); std::exit(1); }
        }

        // Touch + warm the one physical alias so it's resident before timing starts.
        std::memset(reservation, 0, alias_bytes);

        result.base = static_cast<std::byte*>(reservation);
        result.fd = fd;
        return result;
    }
};

void bench_scratchpad_locality(bool use_l1_alias) {
    print_header(use_l1_alias
        ? "6. Scratchpad locality experiment -- L1-aliased (16 KiB) backing"
        : "6. Scratchpad locality experiment -- real 2 MiB scratchpad (baseline)");

    std::vector<std::byte> seed_key = {std::byte{0x00}, std::byte{0x11}};
    armrx::Argon2dCache cache;
    cache.initialize(seed_key);

    std::uint32_t jit_flags = armrx::kRandOMXFlagHardAes | armrx::kRandOMXFlagJit;
    armrx::VirtualMachine vm(jit_flags);
    vm.set_cache(&cache);

    alignas(16) std::array<std::byte, 32> hash_out{};
    std::array<std::byte, 76> block_template{};
    for (size_t i = 0; i < block_template.size(); ++i)
        block_template[i] = static_cast<std::byte>(i & 0xff);

    // Prime state with one real hash before any scratchpad swap.
    armrx::randomx_calculate_hash(&vm, block_template.data(), block_template.size(), hash_out.data());

    AliasedScratchpad aliased;
    if (use_l1_alias) {
        constexpr std::size_t kAliasBytes = 16384; // 16 KiB -- Cortex-A53 L1 D-cache size
        aliased = AliasedScratchpad::create(kAliasBytes, armrx::kRandomXScratchpadBytes);
        vm.override_scratchpad_for_bench(aliased.base, armrx::kRandomXScratchpadBytes);
    }

    // One more full run() so the compiled program reflects current state
    // immediately before the execute-only loop below takes over.
    vm.run(block_template.data());

    constexpr unsigned kWarmupIters = 100;
    for (unsigned i = 0; i < kWarmupIters; ++i) vm.run_execute_only();

    // Each iteration is a full 2048-instruction JIT program execution against
    // the scratchpad (~26 ms/iteration measured on-device) -- this is not a
    // cheap microbenchmark op. 2000 iterations is already a low-noise sample
    // for a perf-stat cycles/instructions *ratio* (a deterministic repeated
    // loop has very little run-to-run variance) while keeping each condition
    // under a minute; the first version of this experiment used 20000 and
    // took ~9 minutes per condition for no measurement benefit.
    constexpr unsigned kIterations = 2000;
    auto t0 = bench_clock::now();
    for (unsigned i = 0; i < kIterations; ++i) vm.run_execute_only();
    auto t1 = bench_clock::now();
    double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

    std::cout << std::left << std::setw(44)
              << (use_l1_alias ? "run_execute_only (L1-aliased)" : "run_execute_only (real scratchpad)")
              << std::right << std::fixed << std::setprecision(2)
              << std::setw(10) << (total_us / kIterations) << " μs/program  "
              << std::setw(10) << (kIterations / (total_us / 1e6)) << " programs/s\n";
    std::cout << "  (" << kIterations << " executions of the already-compiled program; "
              << "wrap this process in `perf stat -e cycles,instructions` for the real\n"
              << "  IPC comparison -- wall-clock alone is not sensitive enough, per this "
              << "project's own track record.)\n";
}
#endif // ARMRX_HAVE_JIT

void bench_region_attribution() {
    print_header("4. Region attribution — hash pipeline phases");

    // Build one cache + VM for all phases
    std::vector<std::byte> seed_key = {std::byte{0x00}, std::byte{0x11}};
    armrx::Argon2dCache cache;
    cache.initialize(seed_key);

    std::uint32_t jit_flags = armrx::kRandOMXFlagHardAes | armrx::kRandOMXFlagJit;
    armrx::VirtualMachine vm(jit_flags);
    vm.set_cache(&cache);

    // Deterministic input block (76 bytes, like a Monero block header)
    alignas(16) std::array<std::byte, 32> hash_out{};
    std::array<std::byte, 76> block_template{};
    for (size_t i = 0; i < block_template.size(); ++i)
        block_template[i] = static_cast<std::byte>(i & 0xff);

    // Pre-run one hash to warm caches and JIT
    armrx::randomx_calculate_hash(&vm, block_template.data(),
                                   block_template.size(), hash_out.data());

    // ── 4a. Full hash (light, JIT) — median of many samples ────────────
    constexpr unsigned kFullHashSamples = 200;
    std::vector<double> full_hash_ns;
    full_hash_ns.reserve(kFullHashSamples);

    // Warmup: 10 hashes
    for (unsigned w = 0; w < 10; ++w) {
        block_template[39] = static_cast<std::byte>(w);
        armrx::randomx_calculate_hash(&vm, block_template.data(),
                                       block_template.size(), hash_out.data());
    }

    // Collect samples with varying nonce
    for (unsigned i = 0; i < kFullHashSamples; ++i) {
        block_template[39] = static_cast<std::byte>(i + 100);
        auto t0 = bench_clock::now();
        armrx::randomx_calculate_hash(&vm, block_template.data(),
                                       block_template.size(), hash_out.data());
        auto t1 = bench_clock::now();
        full_hash_ns.push_back(
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }

    std::sort(full_hash_ns.begin(), full_hash_ns.end());
    double median_ns = full_hash_ns[kFullHashSamples / 2];
    double min_ns    = full_hash_ns.front();
    double max_ns    = full_hash_ns.back();
    double sum_ns    = std::accumulate(full_hash_ns.begin(), full_hash_ns.end(), 0.0);
    double mean_ns   = sum_ns / kFullHashSamples;

    double sq_sum = 0.0;
    for (auto ns : full_hash_ns) { double d = ns - mean_ns; sq_sum += d * d; }
    double stddev_pct = (mean_ns > 0.0) ? std::sqrt(sq_sum / kFullHashSamples) / mean_ns * 100.0 : 0.0;

    std::cout << std::left << std::setw(44) << "full hash (light, JIT)"
              << std::right << std::fixed << std::setprecision(2)
              << std::setw(10) << median_ns / 1000.0 << " μs  "
              << std::setw(8) << (median_ns > 0.0 ? 1e9 / median_ns : 0.0) << " hash/s  "
              << "[min " << min_ns / 1000.0 << " / max " << max_ns / 1000.0
              << " μs, σ " << std::setprecision(1) << stddev_pct
              << "%, n=" << kFullHashSamples << "]\n";

    // ── 4b. Phase breakdown via instrumented full pipeline ────────────
    //
    // Replicate randomx_calculate_hash() manually, inserting timing points
    // around each phase. This gives accurate per-phase timing because every
    // phase runs as part of an actual hash computation.
    {
        constexpr unsigned kPhaseSamples = 50;

        // Per-phase accumulators (nanoseconds)
        std::vector<double> b2b_input_ns(kPhaseSamples);
        std::vector<double> init_sp_ns(kPhaseSamples);
        std::vector<double> run_chain_ns(kPhaseSamples);   // 7 × run() + 7 × blake2b
        std::vector<double> run_final_ns(kPhaseSamples);   // 1 × run()
        std::vector<double> finalize_ns(kPhaseSamples);    // get_final_result

        for (unsigned s = 0; s < kPhaseSamples; ++s) {
            block_template[39] = static_cast<std::byte>(s + 500);

            fenv_t fpstate;
            std::fegetenv(&fpstate);

            alignas(16) std::array<std::byte, 64> tempHash{};
            std::span<const std::byte> input_span(
                reinterpret_cast<const std::byte*>(block_template.data()), block_template.size());

            // Phase: blake2b input → 64-byte seed
            auto t0 = bench_clock::now();
            armrx::blake2b(input_span, tempHash.data(), 64);
            auto t1 = bench_clock::now();
            b2b_input_ns[s] = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            // Phase: init_scratchpad (AES fill 2 MiB)
            t0 = bench_clock::now();
            vm.init_scratchpad(tempHash.data());
            vm.reset_rounding_mode();
            t1 = bench_clock::now();
            init_sp_ns[s] = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            // Phase: chain loop (7 × run + 7 × blake2b mix)
            alignas(16) std::array<std::byte, sizeof(armrx::RegisterFile)> reg_bytes{};
            t0 = bench_clock::now();
            for (int chain = 0; chain < 7; ++chain) {
                vm.run(tempHash.data());
                const auto& reg = vm.get_register_file();
                std::memcpy(reg_bytes.data(), &reg, sizeof(reg));
                armrx::blake2b(std::span<const std::byte>(reg_bytes), tempHash.data(), 64);
            }
            t1 = bench_clock::now();
            run_chain_ns[s] = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            // Phase: final run()
            t0 = bench_clock::now();
            vm.run(tempHash.data());
            t1 = bench_clock::now();
            run_final_ns[s] = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            // Phase: get_final_result
            t0 = bench_clock::now();
            vm.get_final_result(hash_out.data());
            t1 = bench_clock::now();
            finalize_ns[s] = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

            std::fesetenv(&fpstate);
        }

        // Helper to report a phase from collected ns samples
        auto report_phase = [&](const char* label, std::vector<double>& ns) {
            std::sort(ns.begin(), ns.end());
            unsigned n = static_cast<unsigned>(ns.size());
            double med = ns[n / 2];
            double mn  = ns.front();
            double mx  = ns.back();
            double sum = std::accumulate(ns.begin(), ns.end(), 0.0);
            double avg = sum / n;
            double sq = 0.0;
            for (auto v : ns) { double d = v - avg; sq += d * d; }
            double sp = (avg > 0.0) ? std::sqrt(sq / n) / avg * 100.0 : 0.0;
            std::cout << std::left << std::setw(44) << label
                      << std::right << std::fixed << std::setprecision(2)
                      << std::setw(10) << med / 1000.0 << " μs  "
                      << std::setw(8) << (med > 0.0 ? 1e9 / med : 0.0) << " /s  "
                      << "[min " << mn / 1000.0 << " / max " << mx / 1000.0
                      << " μs, σ " << std::setprecision(1) << sp
                      << "%, n=" << n << "]\n";
        };

        report_phase("  ├ blake2b (input→seed)",        b2b_input_ns);
        report_phase("  ├ init_scratchpad (AES 2MiB)",   init_sp_ns);
        report_phase("  ├ chain: 7×run() + 7×blake2b",   run_chain_ns);
        report_phase("  ├ final run()",                   run_final_ns);
        report_phase("  └ get_final_result (AES+blake2b)", finalize_ns);

        // Totals
        struct Agg {
            std::string label;
            std::vector<double> data;
        };
        std::vector<Agg> phases = {
            {"  blake2b (input→seed)",       b2b_input_ns},
            {"  init_scratchpad",             init_sp_ns},
            {"  chain: 7×run + 7×blake2b",   run_chain_ns},
            {"  final run()",                 run_final_ns},
            {"  get_final_result",            finalize_ns},
        };

        std::cout << "\n  Phase totals (% of full hash):\n";
        for (auto& p : phases) {
            std::sort(p.data.begin(), p.data.end());
            double p_med = p.data[p.data.size() / 2];
            double pct = (median_ns > 0.0) ? (p_med / median_ns * 100.0) : 0.0;
            std::cout << "    ├ " << std::left << std::setw(34) << p.label
                      << std::right << std::fixed << std::setprecision(2)
                      << std::setw(10) << p_med / 1000.0 << " μs  "
                      << std::setw(6) << pct << "%\n";
        }
    }

    // ── 4c. JIT compile vs execute (when ARMRX_JIT_PROFILE is enabled) ──
#ifdef ARMRX_JIT_PROFILE
    // Run enough hashes that the JIT timers accumulate measurable values
    vm.reset_jit_timers();
    {
        block_template[39] = static_cast<std::byte>(0);
        armrx::randomx_calculate_hash(&vm, block_template.data(),
                                       block_template.size(), hash_out.data());
        // Run 50 more hashes to accumulate profile data
        for (unsigned i = 1; i <= 50; ++i) {
            block_template[39] = static_cast<std::byte>(i + 200);
            armrx::randomx_calculate_hash(&vm, block_template.data(),
                                           block_template.size(), hash_out.data());
        }
    }
    std::uint64_t total_compile_ns = vm.get_jit_compile_time_ns();
    std::uint64_t total_execute_ns = vm.get_jit_execute_time_ns();
    std::uint64_t total_runs       = vm.get_jit_total_runs();

    if (total_runs > 0) {
        double compile_per_run_us = static_cast<double>(total_compile_ns) / total_runs / 1000.0;
        double execute_per_run_us = static_cast<double>(total_execute_ns) / total_runs / 1000.0;
        // Each hash runs 8 programs, so 8 runs per hash
        double per_program_compile = compile_per_run_us;
        double per_program_execute = execute_per_run_us;
        double per_hash_compile    = compile_per_run_us * 8.0;
        double per_hash_execute    = execute_per_run_us * 8.0;

        std::cout << "\n  JIT profile (" << total_runs << " program runs over 50 hashes):\n";
        std::cout << "    ├ JIT compile:    "
                  << std::fixed << std::setprecision(2)
                  << per_program_compile << " μs/program  ("
                  << per_hash_compile << " μs/hash)\n";
        std::cout << "    ├ JIT execute:    "
                  << per_program_execute << " μs/program  ("
                  << per_hash_execute << " μs/hash)\n";
        double total_ph = per_hash_compile + per_hash_execute;
        std::cout << "    └ JIT total:      "
                  << total_ph << " μs/hash  ("
                  << (per_hash_compile / total_ph * 100.0) << "% compile / "
                  << (per_hash_execute / total_ph * 100.0) << "% exec)\n";
    }
#else
    std::cout << "\n  JIT profile: not enabled (rebuild with -DARMRX_JIT_PROFILE=ON)\n";
#endif

    // ── 4d. Interpreted mode comparison ────────────────────────────────
    {
        std::uint32_t interp_flags = 0;
        armrx::VirtualMachine vm_interp(interp_flags);
        vm_interp.set_cache(&cache);

        // Warmup
        block_template[39] = static_cast<std::byte>(0);
        armrx::randomx_calculate_hash(&vm_interp, block_template.data(),
                                       block_template.size(), hash_out.data());

        // Collect samples
        constexpr unsigned kInterpSamples = 30;
        std::vector<double> interp_ns;
        interp_ns.reserve(kInterpSamples);
        for (unsigned i = 0; i < kInterpSamples; ++i) {
            block_template[39] = static_cast<std::byte>(i + 300);
            auto t0 = bench_clock::now();
            armrx::randomx_calculate_hash(&vm_interp, block_template.data(),
                                           block_template.size(), hash_out.data());
            auto t1 = bench_clock::now();
            interp_ns.push_back(
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }

        std::sort(interp_ns.begin(), interp_ns.end());
        double interp_median = interp_ns[kInterpSamples / 2];
        double interp_min    = interp_ns.front();
        double interp_max    = interp_ns.back();

        std::cout << "\n  Interpreted mode:\n";
        std::cout << "    ├ full hash (light): "
                  << std::fixed << std::setprecision(2)
                  << interp_median / 1000.0 << " μs  ["
                  << interp_min / 1000.0 << " – " << interp_max / 1000.0
                  << " μs, n=" << kInterpSamples << "]\n";
        std::cout << "    └ JIT speedup: "
                  << (interp_median / median_ns) << "×\n";
        std::cout << std::endl;
    }
}

} // namespace

int main(int argc, char** argv) {
    bool run_all         = true;
    bool attribution_only = false;
    bool full_hash_only   = false;
    bool micro_only       = false;
    bool argon2_only      = false;
    bool scratchpad_real  = false;
    bool scratchpad_l1    = false;
    bool perf_ready       = false;
    unsigned perf_ready_timeout = 300;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--attribution-only") { run_all = false; attribution_only = true; }
        else if (arg == "--full-hash-only") { run_all = false; full_hash_only = true; }
        else if (arg == "--perf-ready") { perf_ready = true; }
        else if (arg.rfind("--perf-ready-timeout=", 0) == 0) {
            perf_ready_timeout = static_cast<unsigned>(std::stoul(arg.substr(21)));
        }
        else if (arg == "--micro-only") { run_all = false; micro_only = true; }
        else if (arg == "--argon2-only") { run_all = false; argon2_only = true; }
        else if (arg == "--scratchpad-real") { run_all = false; scratchpad_real = true; }
        else if (arg == "--scratchpad-l1") { run_all = false; scratchpad_l1 = true; }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: bench_armrx [OPTIONS]\n"
                      << "Options:\n"
                      << "  --attribution-only   Only run region-attribution benchmarks\n"
                      << "  --full-hash-only     Only run full-hash throughput benchmark\n"
                      << "  --micro-only         Only run micro-benchmarks (blake2b, AES, dataset)\n"
                      << "  --argon2-only        Only run isolated Argon2dCache::initialize() benchmark\n"
                      << "  --scratchpad-real    Scratchpad locality experiment, real 2 MiB scratchpad\n"
                      << "                       (AArch64/JIT builds only; run each of --scratchpad-real\n"
                      << "                       and --scratchpad-l1 under `perf stat -e cycles,instructions`\n"
                      << "                       and compare IPC -- see performance-plan-20260725.md Step 1)\n"
                      << "  --scratchpad-l1      Same experiment, scratchpad aliased to 16 KiB (L1-resident)\n"
                      << "  --perf-ready         Signal PERF_READY on stdout and block on stdin before full hash loop\n"
                      << "  --perf-ready-timeout=N Timeout for --perf-ready in seconds (default 300)\n"
                      << "  --help               Show this message\n";
            return 0;
        }
    }

    std::cout << "\n╔══════════════════════════════════════════════════════════╗\n"
              << "║        armrx Benchmark Protocol v2                      ║\n"
              << "╚══════════════════════════════════════════════════════════╝\n";

    if (run_all || micro_only) {
        bench_blake2b();
        bench_aes_primitives();
        bench_dataset_helpers();
        bench_argon2_compress();
    }

    // Kept separate from --micro-only (not bundled with it) so it can be
    // profiled in isolation without blake2b/AES/dataset-helper/compress
    // samples diluting the picture — the whole point of this benchmark.
    if (run_all || argon2_only) {
        bench_argon2_cache_init();
    }

    if (run_all || attribution_only) {
        bench_region_attribution();
    }

    if (run_all || full_hash_only) {
        print_header("5. Full hash throughput (light, JIT) — extended measurement");

        std::vector<std::byte> seed_key = {std::byte{0x00}, std::byte{0x11}};
        armrx::Argon2dCache cache;
        cache.initialize(seed_key);

        std::uint32_t jit_flags = armrx::kRandOMXFlagHardAes | armrx::kRandOMXFlagJit;
        armrx::VirtualMachine vm(jit_flags);
        vm.set_cache(&cache);

        alignas(16) std::array<std::byte, 32> hash_out{};
        std::array<std::byte, 76> block_template{};
        for (size_t i = 0; i < block_template.size(); ++i)
            block_template[i] = static_cast<std::byte>(i & 0xff);

        // Warmup: 30 hashes
        for (unsigned w = 0; w < 30; ++w) {
            block_template[39] = static_cast<std::byte>(w);
            armrx::randomx_calculate_hash(&vm, block_template.data(),
                                           block_template.size(), hash_out.data());
        }

        // T1-2: clean perf-stat hook — signal readiness after all one-time setup
        // (cache init, JIT compile, warmup) and gate the measured region on a
        // wrapper-provided stdin line so `perf stat -p <pid>` can attach exactly
        // at steady state (master-plan 2.77x measurement-methodology pitfall).
        if (perf_ready) {
            if (!full_hash_only) {
                std::cerr << "error: --perf-ready requires --full-hash-only\n";
                return 1;
            }
            std::cout << "PERF_READY" << std::endl;   // endl flushes
            // Wait for wrapper go-ahead (or timeout / EOF).
            struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
            const int pr = ::poll(&pfd, 1, static_cast<int>(perf_ready_timeout) * 1000);
            if (pr > 0 && (pfd.revents & POLLIN)) {
                std::string line;
                std::getline(std::cin, line);   // consume the go line; EOF proceeds
            }
        }

        // Steady-state: 500 hashes collecting individual samples
        constexpr unsigned kSamples = 500;
        std::vector<double> ns_samples;
        ns_samples.reserve(kSamples);

        for (unsigned i = 0; i < kSamples; ++i) {
            block_template[39] = static_cast<std::byte>(i + 1000);
            auto t0 = bench_clock::now();
            armrx::randomx_calculate_hash(&vm, block_template.data(),
                                           block_template.size(), hash_out.data());
            auto t1 = bench_clock::now();
            ns_samples.push_back(
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
        }

        std::sort(ns_samples.begin(), ns_samples.end());

        auto report_percentile = [&](double p) -> double {
            size_t idx = static_cast<size_t>(p * (kSamples - 1) / 100.0);
            size_t idx_clamped = std::min(idx, static_cast<size_t>(kSamples - 1));
            return ns_samples[idx_clamped] / 1000.0;
        };

        double sum = std::accumulate(ns_samples.begin(), ns_samples.end(), 0.0);
        double mean = sum / kSamples;

        std::cout << std::left << std::setw(44) << "full hash (light, JIT)"
                  << std::right << std::fixed << std::setprecision(2)
                  << std::setw(10) << ns_samples[kSamples / 2] / 1000.0 << " μs median  "
                  << std::setw(8) << (ns_samples[kSamples / 2] > 0.0 ? 1e9 / ns_samples[kSamples / 2] : 0.0) << " hash/s\n";

        std::cout << "  min:       " << report_percentile(0)   << " μs\n"
                  << "  1st pctl:  " << report_percentile(1)   << " μs\n"
                  << "  5th pctl:  " << report_percentile(5)   << " μs\n"
                  << "  25th pctl: " << report_percentile(25)  << " μs\n"
                  << "  50th pctl: " << report_percentile(50)  << " μs (median)\n"
                  << "  75th pctl: " << report_percentile(75)  << " μs\n"
                  << "  95th pctl: " << report_percentile(95)  << " μs\n"
                  << "  99th pctl: " << report_percentile(99)  << " μs\n"
                  << "  max:       " << report_percentile(100) << " μs\n"
                  << "  mean:      " << mean / 1000.0          << " μs\n";
    }

#ifdef ARMRX_HAVE_JIT
    if (scratchpad_real) {
        bench_scratchpad_locality(/*use_l1_alias=*/false);
    }
    if (scratchpad_l1) {
        bench_scratchpad_locality(/*use_l1_alias=*/true);
    }
#else
    if (scratchpad_real || scratchpad_l1) {
        std::cout << "\n--scratchpad-real/--scratchpad-l1 require an ARMRX_HAVE_JIT build "
                     "(AArch64 target).\n";
    }
#endif

    if (run_all) {
        std::cout << "\n=== Done ===\n";
    }

    return 0;
}
