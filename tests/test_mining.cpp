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
#include <future>
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

// Covers the 2026-08-07 light-mode seed-rotation bug: the hybrid partial
// dataset (`--dataset-mb=N`) was filled exactly once (gated by a one-shot
// atomic_flag) and never rebuilt on a seed-key rotation, so after a pool
// "New job" the workers kept reading the OLD seed's cached prefix while the
// cache held the NEW seed -> silent wrong hashes / invalid shares. The fix
// re-runs start_fill() with the new cache and makes each worker re-wait for
// the fresh fill before hashing again.
//
// Verification strategy (mirrors test_fast_mode_dataset_reinit_via_workers):
// RandomX light mode computes each dataset item on the fly via
// generate_dataset_item(), mathematically equal to the fast-mode value for
// the same seed. So after rotating to job2's seed, cross-check the engine's
// job2 (nonce, hash) samples against an independently built light-mode VM
// seeded with job2's cache. If the engine were still reading a stale seed-A
// partial prefix for job2, those hashes would diverge from the seed-B
// reference and the assertion would fail -- exactly the bug. A modest partial
// prefix (kPartialItems) is used so hashing actually hits the cached region
// and the divergence is observable, while keeping the re-fill fast on-device.
void test_light_mode_seed_rotation_rebuilds_partial_dataset() {
    constexpr unsigned kThreads = 2;
    // 65536 items = 4 MiB prefix. Large enough that random dataset-item
    // accesses hit it every hash (so a stale-prefix bug is caught), small
    // enough that the NEON re-fill takes well under a second on-device.
    constexpr std::size_t kPartialItems = 65536;

    armrx::MiningEngine engine(armrx::RandomXMode::light, kThreads);

    auto pd = std::make_shared<armrx::PartialDataset>(kPartialItems);
    engine.set_partial_dataset(pd);

    auto make_job = [](const std::string& id, std::vector<std::byte> seed) {
        armrx::Job job;
        job.job_id = id;
        job.block_template.assign(76, std::byte{0});
        job.nonce_offset = 39;
        job.nonce_size = 4;
        job.seed_key = std::move(seed);
        std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xff}); // accept every hash
        return job;
    };

    std::vector<std::byte> seedA{std::byte{'s'}, std::byte{'e'}, std::byte{'e'}, std::byte{'d'},
                                  std::byte{'A'}, std::byte{'A'}, std::byte{'A'}, std::byte{'A'}};
    std::vector<std::byte> seedB{std::byte{'s'}, std::byte{'e'}, std::byte{'e'}, std::byte{'d'},
                                  std::byte{'B'}, std::byte{'B'}, std::byte{'B'}, std::byte{'B'}};

    auto job1 = make_job("job1", seedA);
    auto job2 = make_job("job2", seedB);

    std::mutex found_mutex;
    std::vector<std::pair<std::string, std::pair<std::uint64_t, std::array<std::byte, 32>>>> found;

    // First job (seed A). The partial dataset fills once for this seed.
    engine.set_job(job1);
    engine.start([&](const armrx::Job& j, std::uint64_t nonce, std::array<std::byte, 32> hash) {
        std::lock_guard<std::mutex> lock(found_mutex);
        found.emplace_back(j.job_id, std::make_pair(nonce, hash));
    });

    // Wait for the first (seed-A) fill to finish before rotating.
    const auto fill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!pd->fill_complete() && std::chrono::steady_clock::now() < fill_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(pd->fill_complete());
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let job1 hash briefly

    // Rotate to job2 (different seed) WHILE workers are running. This is the
    // path that must re-fill the partial dataset with seed B and make workers
    // re-wait before hashing job2.
    engine.set_job(job2);

    // Poll for job2 samples (bounded so a real regression fails instead of hangs).
    std::vector<std::pair<std::uint64_t, std::array<std::byte, 32>>> job2_samples;
    const auto poll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (job2_samples.empty() && std::chrono::steady_clock::now() < poll_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lock(found_mutex);
        for (auto& [jid, nonce_hash] : found) {
            if (jid == "job2") job2_samples.push_back(nonce_hash);
        }
    }
    engine.stop();
    assert(!job2_samples.empty());

    // Build independent reference caches for seed-A and seed-B, ONE AT A TIME
    // (each needs ~256 MiB; the engine has already been stopped so we don't
    // hold its allocation concurrently). generate_dataset_item() is the same
    // reference test_partial_dataset already byte-validates.
    armrx::Argon2dCache seedA_cache_for_verify;
    seedA_cache_for_verify.initialize(seedA);
    armrx::Argon2dCache seedB_cache_for_verify;
    seedB_cache_for_verify.initialize(seedB);

    // The actual bug under test: before the fix, the partial dataset was
    // filled exactly once (gated by a one-shot flag) and NEVER rebuilt on a
    // seed rotation, so its buffer kept seed-A's data while the cache held
    // seed-B -> stale-seed wrong shares. After the fix, set_job() re-runs
    // start_fill() with the new cache on rotation, so the buffer must now
    // contain seed-B's dataset items.
    //
    // We verify the buffer DIRECTLY (byte-compare against
    // generate_dataset_item(seedB, i)) rather than via the engine's
    // end-to-end hash. Rationale: this isolates exactly what the rotation
    // fix changed (the buffer rebuild on rotation) from the separate
    // end-to-end hybrid-consistency check. NOTE (2026-08-10): an earlier
    // version of this comment claimed the hybrid consumption path "is
    // currently NOT producing correct end-to-end hashes on-device" — that
    // was stale. The sibling test
    // test_light_mode_partial_dataset_matches_reference() asserts the engine
    // hash against a clean light-mode reference VM for the same nonce and
    // PASSES on-device (JIT) — see README's "Hybrid partial dataset" note
    // (✅ correct and recommended). The hybrid --dataset-mb>0 path is
    // verified correct end-to-end; this buffer-byte check is a belt-and-
    // suspenders isolation of the rotation rebuild specifically.
    // generate_dataset_item() is the same reference test_partial_dataset
    // already byte-validates, so this check is platform-independent.
    assert(pd->fill_complete());
    assert(pd->item_count() == kPartialItems);
    constexpr std::size_t kVerifyItems = 256; // spot-check first 256 items
    std::size_t wrong = 0;
    for (std::size_t i = 0; i < kVerifyItems; ++i) {
        const auto expected = armrx::generate_dataset_item(seedB_cache_for_verify, static_cast<std::uint64_t>(i));
        const auto* actual = pd->data() + i * armrx::kRandomXDatasetItemBytes;
        if (std::memcmp(expected.data(), actual, armrx::kRandomXDatasetItemBytes) != 0) {
            ++wrong;
        }
        // Also confirm it is NOT the stale seed-A value.
        const auto stale = armrx::generate_dataset_item(seedA_cache_for_verify, static_cast<std::uint64_t>(i));
        if (std::memcmp(stale.data(), actual, armrx::kRandomXDatasetItemBytes) == 0) {
            std::fprintf(stderr, "[DIAG] item %zu is STALE seed-A data after rotation\n", i);
            ++wrong;
        }
    }
    assert(wrong == 0);

    std::cout << "[test_mining] test_light_mode_seed_rotation_rebuilds_partial_dataset passed ("
              << kVerifyItems << " cached items re-filled with seed-B after rotation; no stale seed-A data)\n";
}

void test_light_mode_partial_dataset_matches_reference() {
    constexpr std::size_t kPartialItems = 65536;

    armrx::MiningEngine engine(armrx::RandomXMode::light, 1);
    auto pd = std::make_shared<armrx::PartialDataset>(kPartialItems);
    engine.set_partial_dataset(pd);

    const std::vector<std::byte> seed{std::byte{'s'}, std::byte{'e'}, std::byte{'e'}, std::byte{'d'},
                                      std::byte{'B'}, std::byte{'B'}, std::byte{'B'}, std::byte{'B'}};
    armrx::Job job;
    job.job_id = "partial";
    job.block_template.assign(76, std::byte{0});
    job.nonce_offset = 39;
    job.nonce_size = 4;
    job.seed_key = seed;
    std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xff});

    std::mutex found_mutex;
    std::vector<std::pair<std::uint64_t, std::array<std::byte, 32>>> samples;
    engine.set_job(job);
    engine.start([&](const armrx::Job&, std::uint64_t nonce, std::array<std::byte, 32> hash) {
        std::lock_guard<std::mutex> lock(found_mutex);
        if (samples.empty()) samples.emplace_back(nonce, hash);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool have_sample = false;
    while (!have_sample && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lock(found_mutex);
        have_sample = !samples.empty();
    }
    engine.stop();
    assert(pd->fill_complete());
    assert(!samples.empty());

    armrx::Argon2dCache cache;
    cache.initialize(seed);
    armrx::VirtualMachine reference_vm(armrx::kRandOMXFlagHardAes
#ifdef ARMRX_HAVE_JIT
                                       | armrx::kRandOMXFlagJit
#endif
    );
    reference_vm.set_cache(&cache);
    std::vector<std::byte> input = job.block_template;
    const auto nonce = samples.front().first;
    for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i) {
        input[job.nonce_offset + i] = static_cast<std::byte>((nonce >> (8 * i)) & 0xff);
    }
    std::array<std::byte, 32> expected{};
    armrx::randomx_calculate_hash(&reference_vm, input.data(), input.size(), expected.data());
    assert(samples.front().second == expected);

    std::cout << "[test_mining] test_light_mode_partial_dataset_matches_reference passed\n";
}

// Audit follow-up (P1, seed-rotation race, round 2 -- reviewer-identified
// gap): the original ReadGuard fix stops start_fill() from tearing down the
// buffer WHILE a hash holds the guard, but a worker's generation checks (top
// of worker_loop) happen BEFORE the guard is acquired. A rotation can
// complete entirely in that window, leaving a worker to acquire the guard
// holding a STALE active_cache_/VM state while the buffer it's about to read
// already reflects the NEW seed -- an internally inconsistent hash (VM setup
// from one seed, some dataset bytes from another), never matching either
// seed's correct reference. Fixed by re-validating the fill generation
// immediately after acquiring the guard (see local_partial_fill_gen_for_cache
// in mining_engine.cpp) and skipping the hash entirely on a mismatch.
//
// This is inherently a narrow timing race between a worker's own checks and
// its later guard acquisition, so unlike the deterministic barrier tests in
// test_partial_dataset.cpp, it cannot be forced into one exact interleaving
// without adding test-only instrumentation to production code (out of scope
// here). Instead this hammers repeated back-to-back rotations (no settling
// wait between them, unlike the sibling test above) across multiple workers
// while continuously hashing, and cross-validates EVERY observed hash
// against an independently-built reference for its own claimed seed. A
// hash computed from a stale cache but a rotated prefix would, in general,
// match NEITHER seed's reference (VM execution depends on the cache far
// beyond just the dataset bytes a small prefix can shortcut) -- exactly what
// this checks for.
void test_rapid_seed_rotation_hashes_never_mixed() {
    constexpr unsigned kThreads = 2;
    // Tiny prefix: fills fast enough that many rotations complete within a
    // reasonable test budget even on slow hardware (a 256 MiB Argon2 cache
    // init dominates rotation cost regardless of prefix size), while still
    // large enough that a hash's many dataset accesses are very likely to
    // touch it at least once (making a mixed-generation hash observable).
    constexpr std::size_t kPartialItems = 4096;
    constexpr int kRotations = 6; // 3 full A<->B cycles

    armrx::MiningEngine engine(armrx::RandomXMode::light, kThreads);
    auto pd = std::make_shared<armrx::PartialDataset>(kPartialItems);
    engine.set_partial_dataset(pd);

    auto make_job = [](const std::string& id, std::vector<std::byte> seed) {
        armrx::Job job;
        job.job_id = id;
        job.block_template.assign(76, std::byte{0});
        job.nonce_offset = 39;
        job.nonce_size = 4;
        job.seed_key = std::move(seed);
        std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xff});
        return job;
    };
    std::vector<std::byte> seedA{std::byte{'r'}, std::byte{'a'}, std::byte{'p'}, std::byte{'i'},
                                  std::byte{'d'}, std::byte{'-'}, std::byte{'A'}};
    std::vector<std::byte> seedB{std::byte{'r'}, std::byte{'a'}, std::byte{'p'}, std::byte{'i'},
                                  std::byte{'d'}, std::byte{'-'}, std::byte{'B'}};
    auto jobA = make_job("rapid-A", seedA);
    auto jobB = make_job("rapid-B", seedB);

    std::mutex found_mutex;
    std::vector<std::pair<std::string, std::pair<std::uint64_t, std::array<std::byte, 32>>>> found;
    engine.set_job(jobA);
    engine.start([&](const armrx::Job& j, std::uint64_t nonce, std::array<std::byte, 32> hash) {
        std::lock_guard<std::mutex> lock(found_mutex);
        found.emplace_back(j.job_id, std::make_pair(nonce, hash));
    });

    // Rotate back and forth with NO settling wait between rotations -- the
    // point is to maximize the chance a worker is mid-checks or about to
    // acquire the guard exactly when a rotation completes, not to let things
    // settle between them (the sibling test above already covers the
    // settled case).
    for (int i = 0; i < kRotations; ++i) {
        engine.set_job((i % 2 == 0) ? jobB : jobA);
    }

    // The rapid-fire loop above is opportunistic: with no settling wait, it
    // is not guaranteed (nor was ever intended to guarantee) that BOTH seeds
    // actually get hashed before rotating away again -- a reviewer correctly
    // flagged that the original `!samples_a.empty() || !samples_b.empty()`
    // assertion let the whole test pass even if every single rotation landed
    // on the same seed, exercising no real A<->B transition at all. To make
    // "both seeds get hashed" deterministic rather than lucky, explicitly
    // settle on each seed in turn afterward and poll (bounded, not a fixed
    // guess) until at least one sample lands for it, so this test cannot
    // pass without genuinely observing both sides of a rotation.
    std::vector<std::pair<std::uint64_t, std::array<std::byte, 32>>> samples_a, samples_b;
    auto collect = [&](const char* job_id, std::vector<std::pair<std::uint64_t, std::array<std::byte, 32>>>& out) {
        const auto poll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (out.empty() && std::chrono::steady_clock::now() < poll_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::lock_guard<std::mutex> lock(found_mutex);
            for (auto& [jid, nonce_hash] : found) {
                if (jid == job_id) out.push_back(nonce_hash);
            }
        }
    };
    engine.set_job(jobA);
    collect("rapid-A", samples_a);
    engine.set_job(jobB);
    collect("rapid-B", samples_b);
    engine.stop();

    {
        std::lock_guard<std::mutex> lock(found_mutex);
        samples_a.clear();
        samples_b.clear();
        for (auto& [jid, nonce_hash] : found) {
            if (jid == "rapid-A") samples_a.push_back(nonce_hash);
            else if (jid == "rapid-B") samples_b.push_back(nonce_hash);
        }
    }
    // Both sides of the rotation must be genuinely represented -- this is
    // the exact condition the reviewer's weaker `||` version let slip.
    assert(!samples_a.empty());
    assert(!samples_b.empty());

    armrx::Argon2dCache cache_a, cache_b;
    cache_a.initialize(seedA);
    cache_b.initialize(seedB);

    auto verify = [](armrx::Argon2dCache& cache, const std::vector<std::byte>& seed_unused,
                      const std::vector<std::pair<std::uint64_t, std::array<std::byte, 32>>>& samples) {
        (void)seed_unused;
        std::size_t checked = 0;
        for (auto& [nonce, hash] : samples) {
            armrx::VirtualMachine ref_vm(armrx::kRandOMXFlagHardAes
#ifdef ARMRX_HAVE_JIT
                                         | armrx::kRandOMXFlagJit
#endif
            );
            ref_vm.set_cache(&cache);
            std::vector<std::byte> block(76, std::byte{0});
            for (std::size_t k = 0; k < 4; ++k) {
                block[39 + k] = static_cast<std::byte>((nonce >> (8 * k)) & 0xff);
            }
            std::array<std::byte, 32> expected{};
            armrx::randomx_calculate_hash(&ref_vm, block.data(), block.size(), expected.data());
            if (hash != expected) {
                std::fprintf(stderr, "[DIAG] mismatched hash for nonce %llu -- possible "
                                      "mixed-generation (stale-cache/fresh-prefix) hash\n",
                             static_cast<unsigned long long>(nonce));
            }
            assert(hash == expected);
            ++checked;
        }
        return checked;
    };

    const auto checked_a = verify(cache_a, seedA, samples_a);
    const auto checked_b = verify(cache_b, seedB, samples_b);

    std::cout << "[test_mining] test_rapid_seed_rotation_hashes_never_mixed passed ("
              << checked_a << " seed-A + " << checked_b << " seed-B hashes across "
              << kRotations << " back-to-back rotations, none mixed-generation)\n";
}

// Audit P1 (shutdown hangs before the first partial-dataset fill): a mining
// engine configured with a PartialDataset but never given a job (the real
// scenario is the pool never delivering a first job -- run_pool_mining()
// starts workers before pool_mgr->connect() completes) must not hang on
// stop(). Direct reproducer for the audit's own harness
// (/tmp/armrx-audit-stop.cpp): engine.start() with a PartialDataset
// configured, no set_job() call ever made, then engine.stop(). Before the
// fix, worker_loop()'s unconditional initial wait_for_fill() call never
// returns (no fill was ever started, so fill_complete_/stop_ never change)
// and stop()'s t.join() hangs forever.
//
// Bounded via std::async + wait_for rather than a plain call, so a
// regression fails this test with a clear assertion instead of hanging the
// whole suite (ctest would eventually kill it via the file-level TIMEOUT,
// but that reports a timeout, not a specific, attributable failure).
void test_stop_before_first_job_returns_promptly() {
    auto run = std::async(std::launch::async, [] {
        constexpr std::size_t kPartialItems = 1; // matches the audit harness
        armrx::MiningEngine engine(armrx::RandomXMode::light, 1);
        engine.set_partial_dataset(std::make_shared<armrx::PartialDataset>(kPartialItems));
        engine.start({});
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        engine.stop(); // must return without ever receiving a job
    });

    const auto status = run.wait_for(std::chrono::seconds(10));
    assert(status == std::future_status::ready);

    std::cout << "[test_mining] test_stop_before_first_job_returns_promptly passed "
                 "(engine.stop() returned with no job ever set)\n";
}

// Audit P1, active-fill variant: stop() called WHILE a fill is actively in
// progress must not wait for the whole fill -- cancellation must be observed
// promptly. set_job() always gives the fill every detected core (see its own
// comment on why), so each worker's assigned range here is exactly one
// sub-chunk; this test is therefore an engine-level integration check that
// MiningEngine::stop() -> PartialDataset::cancel() plumbing works end-to-end,
// complementing (not duplicating) the precise multi-sub-chunk, single-core,
// self-calibrated timing proof in
// test_partial_dataset.cpp::test_cancel_bounds_active_fill_to_one_subchunk.
void test_stop_during_active_fill_returns_promptly() {
    // A few sub-chunks' worth (kFillChunkItems = 64 MiB / 64 B = 1,048,576
    // items) so the fill cannot finish in a single sub-chunk on any
    // reasonable host -- if cancellation were not checked between
    // sub-chunks, this would take multiple sub-chunks' worth of wall time.
    const std::size_t kItems = 3 * 1048576; // ~192 MiB, single core

    auto run = std::async(std::launch::async, [kItems] {
        armrx::MiningEngine engine(armrx::RandomXMode::light, 1);
        auto pd = std::make_shared<armrx::PartialDataset>(kItems);
        engine.set_partial_dataset(pd);

        armrx::Job job;
        job.job_id = "job";
        job.block_template.assign(76, std::byte{0});
        job.nonce_offset = 39;
        job.nonce_size = 4;
        job.seed_key = {std::byte{'x'}};
        std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xff});

        engine.set_job(job); // starts the fill (all cores, per set_job()'s design)
        engine.start({});
        // Stop almost immediately -- the fill has barely started.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        engine.stop();
    });

    // Generous, hardware-portable bound: with 8 detected cores (the common
    // case) each worker's assigned range is exactly one 1,048,576-item
    // sub-chunk, so cancellation checked at the loop's first iteration
    // (before that sub-chunk starts) should return almost instantly on any
    // host -- but if the 20 ms head start lets a sub-chunk actually begin
    // before cancel() lands, worst case is waiting for ONE sub-chunk to
    // finish, which this test measured at ~7s on an x86_64 dev workstation
    // but well over a minute on a Cortex-A53 target device (an order of
    // magnitude slower for this compute-bound Argon2d-derived work). 180s
    // comfortably covers that worst case on any host this project targets
    // while still being a real, finite bound (not "eventually", per the
    // precise self-calibrated proof in
    // test_partial_dataset.cpp::test_cancel_bounds_active_fill_to_one_subchunk,
    // which this test complements at the MiningEngine integration level).
    const auto status = run.wait_for(std::chrono::seconds(180));
    assert(status == std::future_status::ready);

    std::cout << "[test_mining] test_stop_during_active_fill_returns_promptly passed "
                 "(engine.stop() returned during an active fill, well within the bound)\n";
}

int main() {
    test_target_comparison();
    test_mining_engine_lifecycle();
    test_worker_survives_bad_nonce_job();
    test_fast_mode_dataset_reinit_via_workers();
    test_stop_races_dataset_reinit();
    test_light_mode_seed_rotation_rebuilds_partial_dataset();
    test_rapid_seed_rotation_hashes_never_mixed();
    test_light_mode_partial_dataset_matches_reference();
    test_stop_before_first_job_returns_promptly();
    test_stop_during_active_fill_returns_promptly();
    std::cout << "ALL MINING TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
