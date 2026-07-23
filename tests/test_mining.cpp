#include "armrx/mining_engine.hpp"
#include "armrx/mining_common.hpp"
#include "armrx/dataset.hpp"
#include "armrx/vm.hpp"
#include "armrx/memory.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>

void test_target_comparison() {
    armrx::Target target1;
    std::fill(target1.bytes.begin(), target1.bytes.end(), std::byte{0xff}); // Maximum value

    std::array<std::byte, 32> hash1{};
    std::fill(hash1.begin(), hash1.end(), std::byte{0x7f});

    assert(armrx::meets_target(hash1, target1));

    armrx::Target target2;
    std::fill(target2.bytes.begin(), target2.bytes.end(), std::byte{0x00});
    target2.bytes[0] = std::byte{0x01}; // target is 1

    std::array<std::byte, 32> hash2{};
    std::fill(hash2.begin(), hash2.end(), std::byte{0x00});
    assert(armrx::meets_target(hash2, target2)); // 0 <= 1

    hash2[0] = std::byte{0x02};
    assert(!armrx::meets_target(hash2, target2)); // 2 > 1
    
    // Test boundary: most significant byte
    armrx::Target target3;
    std::fill(target3.bytes.begin(), target3.bytes.end(), std::byte{0x00});
    target3.bytes[31] = std::byte{0x10};

    std::array<std::byte, 32> hash3{};
    std::fill(hash3.begin(), hash3.end(), std::byte{0x00});
    hash3[31] = std::byte{0x0f};
    assert(armrx::meets_target(hash3, target3)); // 0x0f < 0x10

    hash3[31] = std::byte{0x11};
    assert(!armrx::meets_target(hash3, target3)); // 0x11 > 0x10

    std::cout << "[test_mining] test_target_comparison passed!\n";
}

void test_mining_engine_lifecycle() {
    // Initialize light mode engine with 2 threads
    armrx::MiningEngine engine(armrx::RandomXMode::light, 2);

    armrx::Job job;
    job.job_id = "test_job_1";
    job.block_template = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    job.nonce_offset = 3;
    job.nonce_size = 4;
    job.seed_key = {std::byte{0x74}, std::byte{0x65}, std::byte{0x73}, std::byte{0x74}}; // "test"
    
    // Set target to max so that any calculated hash meets it
    std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xff});

    std::atomic<int> share_count{0};
    engine.set_job(job);

    engine.start([&share_count](const armrx::Job&, std::uint64_t, std::array<std::byte, 32>) {
        share_count.fetch_add(1, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    engine.stop();

    assert(engine.total_hashes() > 0);
    assert(share_count.load() > 0);
    std::cout << "[test_mining] test_mining_engine_lifecycle passed with " 
              << engine.total_hashes() << " hashes and " << share_count.load() << " shares!\n";
}

// Regression test for PLAN.md Phase 4 item A: a job whose nonce_offset/size
// doesn't fit the block_template used to permanently kill the worker thread
// that hit it (update_nonce_in_template() failing led to `return;` inside
// worker_loop(), exiting the thread for the rest of the process's life,
// instead of `continue;` like every neighboring bad-state path). Pool-
// triggerable (a malformed/truncated block template from the pool), and
// silent — no crash, just one fewer working thread forever. This test feeds
// exactly that bad job first, confirms no hashes come from it, then feeds a
// valid job afterward and confirms the *same* worker threads pick it up and
// produce hashes/shares — proving they're still alive, not just that the
// process didn't crash.
void test_worker_survives_bad_nonce_job() {
    armrx::MiningEngine engine(armrx::RandomXMode::light, 2);

    armrx::Job bad_job;
    bad_job.job_id = "bad_job";
    bad_job.block_template = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}; // only 3 bytes
    bad_job.nonce_offset = 3;
    bad_job.nonce_size = 4; // offset+size = 7 > block_template.size() = 3
    bad_job.seed_key = {std::byte{'b'}, std::byte{'a'}, std::byte{'d'}};
    std::fill(bad_job.target.bytes.begin(), bad_job.target.bytes.end(), std::byte{0xff});

    std::atomic<int> share_count{0};
    engine.set_job(bad_job);
    engine.start([&share_count](const armrx::Job&, std::uint64_t, std::array<std::byte, 32>) {
        share_count.fetch_add(1, std::memory_order_relaxed);
    });

    // Give workers a chance to hit the bad job and (before the fix) exit.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    assert(engine.total_hashes() == 0);

    armrx::Job good_job;
    good_job.job_id = "good_job";
    good_job.block_template = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    good_job.nonce_offset = 3;
    good_job.nonce_size = 4;
    good_job.seed_key = {std::byte{'g'}, std::byte{'o'}, std::byte{'o'}, std::byte{'d'}};
    std::fill(good_job.target.bytes.begin(), good_job.target.bytes.end(), std::byte{0xff});

    engine.set_job(good_job);

    // Poll rather than a fixed sleep: without JIT this can take a while.
    const auto poll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (engine.total_hashes() == 0 && std::chrono::steady_clock::now() < poll_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    engine.stop();

    // If the worker threads had exited on the bad job, total_hashes() would
    // still be 0 here and no shares would ever be found.
    assert(engine.total_hashes() > 0);
    assert(share_count.load() > 0);

    std::cout << "[test_mining] test_worker_survives_bad_nonce_job passed ("
              << engine.total_hashes() << " hashes, " << share_count.load()
              << " shares after recovering from a bad-nonce job)\n";
}

// Covers PLAN.md §2.1: fast-mode seed-key rotation while workers are already
// running now reuses the persistent, affinity-pinned mining threads to build
// the new dataset (instead of spawning temporary threads). This test exercises
// that live-reuse path (not the pre-start() fallback, which is unchanged) and
// verifies the resulting dataset is correct.
//
// Verification strategy: RandomX "light mode" computes each dataset item
// on the fly via generate_dataset_item() instead of reading a materialized
// buffer (see VirtualMachine::dataset_read(), src/vm.cpp) — mathematically
// guaranteed to equal the fast-mode value for the same seed. So instead of
// building a second independent ~2080 MiB reference dataset (which briefly
// needs 3x that size resident at once: job1's engine dataset, job2's engine
// dataset, and the reference — found to reliably OOM-kill the ~1.8 GiB
// on-device target hardware), cross-check the engine's fast-mode hashes
// against a light-mode VM sharing only the (256 MiB) cache. Also a strictly
// stronger check: it validates the actual fast/light equivalence invariant
// fast mode exists to preserve, not just "did initialize_dataset() get
// called with the right arguments".
//
// Skips (rather than fails) on hosts too memory-constrained to fit fast
// mode at all — mirrors the same availability check MinerApp::run() applies
// before ever attempting it (include/armrx/memory.hpp::choose_randomx_mode).
// Found via a real on-device failure: a ~1.8 GiB devbox reports fast mode
// needs 2338 MiB and refuses it through the normal CLI path, so forcing fast
// mode directly here (bypassing that check, as MiningEngine's constructor
// itself does not enforce it) reliably got the process OOM-killed while
// building the dataset. Not a regression — fast mode was never actually
// exercised at full scale on that device before this test existed.
constexpr unsigned kFastModeTestThreads = 4;

bool fast_mode_fits_on_this_host() {
    const auto mem = armrx::available_memory();
    const auto choice = armrx::choose_randomx_mode(mem.available_bytes, kFastModeTestThreads);
    return choice.mode == armrx::RandomXMode::fast;
}

void test_fast_mode_dataset_reinit_via_workers() {
    if (!fast_mode_fits_on_this_host()) {
        std::cout << "[test_mining] test_fast_mode_dataset_reinit_via_workers SKIPPED "
                     "(fast mode does not fit in available memory on this host)\n";
        return;
    }

    constexpr unsigned kThreads = kFastModeTestThreads;
    armrx::MiningEngine engine(armrx::RandomXMode::fast, kThreads);

    armrx::Job job1;
    job1.job_id = "job1";
    job1.block_template.assign(76, std::byte{0});
    job1.nonce_offset = 39;
    job1.nonce_size = 4;
    job1.seed_key = {std::byte{'s'}, std::byte{'e'}, std::byte{'e'}, std::byte{'d'}, std::byte{'1'}};
    std::fill(job1.target.bytes.begin(), job1.target.bytes.end(), std::byte{0xff}); // accept every hash

    std::mutex found_mutex;
    std::vector<std::pair<std::string, std::pair<std::uint64_t, std::array<std::byte, 32>>>> found;

    // First job, set before start(): no persistent workers exist yet, so this
    // takes the unchanged temporary-thread fallback path.
    engine.set_job(job1);
    engine.start([&](const armrx::Job& j, std::uint64_t nonce, std::array<std::byte, 32> hash) {
        std::lock_guard<std::mutex> lock(found_mutex);
        found.emplace_back(j.job_id, std::make_pair(nonce, hash));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    armrx::Job job2 = job1;
    job2.job_id = "job2";
    job2.seed_key = {std::byte{'s'}, std::byte{'e'}, std::byte{'e'}, std::byte{'d'}, std::byte{'2'}};

    // Second job, set while workers are already running: this is the path
    // that now reuses the persistent workers for the dataset rebuild.
    engine.set_job(job2);

    // Poll for at least one job2 share rather than guessing a fixed sleep:
    // without JIT (this dev sandbox is x86_64), fast-mode hashing is
    // interpreted and can take well over a second per hash, so a short fixed
    // sleep here undercounts real hardware variance. Bounded by a generous
    // timeout so a genuine regression still fails instead of hanging.
    std::vector<std::pair<std::uint64_t, std::array<std::byte, 32>>> job2_samples;
    const auto poll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (job2_samples.empty() && std::chrono::steady_clock::now() < poll_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::mutex> lock(found_mutex);
        for (auto& [jid, nonce_hash] : found) {
            if (jid == "job2") job2_samples.push_back(nonce_hash);
        }
    }
    // Done with the engine — release its dataset/workers before allocating
    // the (much smaller, but still non-trivial) light-mode reference cache,
    // rather than holding two large allocations concurrently.
    engine.stop();
    assert(!job2_samples.empty());

    // Independently build only the cache (256 MiB) for job2's seed — no
    // materialized dataset needed for a light-mode reference.
    armrx::Argon2dCache ref_cache;
    ref_cache.initialize(job2.seed_key);

    // Light mode: kRandOMXFlagFullMem intentionally omitted, so dataset_read()
    // takes the generate_dataset_item()-on-the-fly path instead of expecting
    // set_dataset() to have been called.
    std::uint32_t flags = armrx::kRandOMXFlagHardAes;
#ifdef ARMRX_HAVE_JIT
    flags |= armrx::kRandOMXFlagJit;
#endif
    armrx::VirtualMachine ref_vm(flags);
    ref_vm.set_cache(&ref_cache);

    // Cross-check a sample of the engine's job2 (nonce, hash) pairs against
    // the light-mode reference. A wrong per-thread range in the engine's fast-
    // mode dataset build (off-by-one, wrong thread_id, a race in the mutex/cv
    // handshake) would corrupt part of the materialized dataset, and the
    // resulting fast-mode hash would then diverge from the light-mode value
    // for the same seed/nonce — exactly what this catches.
    constexpr std::size_t kMaxChecked = 8;
    std::size_t checked = 0;
    for (auto& [nonce, hash] : job2_samples) {
        if (checked >= kMaxChecked) break;
        std::vector<std::byte> block = job2.block_template;
        for (std::size_t i = 0; i < job2.nonce_size; ++i) {
            block[job2.nonce_offset + i] = static_cast<std::byte>((nonce >> (8 * i)) & 0xff);
        }
        std::array<std::byte, 32> ref_hash{};
        armrx::randomx_calculate_hash(&ref_vm, block.data(), block.size(), ref_hash.data());
        assert(ref_hash == hash);
        ++checked;
    }
    assert(checked > 0);

    std::cout << "[test_mining] test_fast_mode_dataset_reinit_via_workers passed ("
              << checked << " nonces cross-checked against an independently built dataset)\n";
}

// Covers the stop()-vs-live-dataset-rebuild race identified while designing
// §2.1: if stop() lands while set_job() is waiting on workers to finish a
// fast-mode dataset rebuild, the wait must be released via the !running_
// escape hatch rather than hang forever.
//
// Note on what "promptly" means here: set_job()'s own wait returns quickly
// once notified (its predicate includes !running_), but stop()'s worker
// t.join() calls can still take a while — a worker already mid-chunk inside
// initialize_dataset() has no way to notice running_ went false until that
// call returns, so stop() can legitimately take up to roughly one worker's
// share of a full dataset rebuild in the worst case. That's an accepted,
// bounded latency tradeoff (see PLAN.md §2.1), not a bug — this test's
// assertion is "eventually returns", not "returns immediately". Kept to 2
// iterations given each one pays the cost of a real fast-mode dataset build.
//
// Skips on hosts too memory-constrained to fit fast mode — see the matching
// comment on fast_mode_fits_on_this_host() above.
void test_stop_races_dataset_reinit() {
    if (!fast_mode_fits_on_this_host()) {
        std::cout << "[test_mining] test_stop_races_dataset_reinit SKIPPED "
                     "(fast mode does not fit in available memory on this host)\n";
        return;
    }

    constexpr unsigned kThreads = kFastModeTestThreads;
    constexpr int kIterations = 2;

    for (int iter = 0; iter < kIterations; ++iter) {
        armrx::MiningEngine engine(armrx::RandomXMode::fast, kThreads);

        armrx::Job job;
        job.job_id = "job";
        job.block_template.assign(76, std::byte{0});
        job.nonce_offset = 39;
        job.nonce_size = 4;
        job.seed_key = {std::byte{'a'}};
        std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xff});

        engine.set_job(job);
        engine.start([](const armrx::Job&, std::uint64_t, std::array<std::byte, 32>) {});

        // Kick off a seed-changing set_job() on a background thread. Sleep
        // past cache init (~0.5-0.6s, measured) before calling stop() below,
        // so the race reliably lands while workers are mid dataset-rebuild
        // (reusing workers) rather than during the unrelated cache-init
        // phase that precedes it.
        armrx::Job job2 = job;
        job2.seed_key = {std::byte{'b'}, static_cast<std::byte>(iter)};
        std::thread setter([&engine, job2] { engine.set_job(job2); });

        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        engine.stop(); // must eventually return, not hang forever

        setter.join(); // must also eventually return once running_ is false
    }

    std::cout << "[test_mining] test_stop_races_dataset_reinit passed ("
              << kIterations << " iterations, no hang)\n";
}

int main() {
    test_target_comparison();
    test_mining_engine_lifecycle();
    test_worker_survives_bad_nonce_job();
    test_fast_mode_dataset_reinit_via_workers();
    test_stop_races_dataset_reinit();
    std::cout << "ALL MINING TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
