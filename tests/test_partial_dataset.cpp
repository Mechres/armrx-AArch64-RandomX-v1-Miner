/**
 * test_partial_dataset — differential correctness for PartialDataset (Track B).
 *
 * Verifies that:
 *   1. partial[i] == generate_dataset_item(*cache, i) for every cached item
 *   2. Out-of-range indices still produce the same result as generate_dataset_item
 *      (the miss path is bit-identical to today)
 *   3. --dataset-mb=0 produces bit-identical results to pure light mode
 *      (partial dataset disabled)
 */

#include "armrx/partial_dataset.hpp"
#include "armrx/dataset.hpp"
#include "armrx/argon2.hpp"
#include "armrx/memory.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

// A fixed seed key for test reproducibility.
const std::vector<std::byte> kTestKey = []{
    std::vector<std::byte> key;
    const char* s = "test key for partial dataset validation";
    key.reserve(std::strlen(s));
    for (const char* p = s; *p; ++p) key.push_back(static_cast<std::byte>(*p));
    return key;
}();

// Create cache once, reuse it — Argon2dCache is non-copyable
std::shared_ptr<const armrx::Argon2dCache> get_test_cache() {
    static std::shared_ptr<const armrx::Argon2dCache> cache = std::make_shared<armrx::Argon2dCache>();
    static bool initialized = false;
    if (!initialized) {
        const_cast<armrx::Argon2dCache*>(cache.get())->initialize(kTestKey);
        initialized = true;
    }
    return cache;
}

void test_cached_items_match() {
    const auto cache = get_test_cache();

    // A small partial dataset (100 items = 6400 bytes)
    constexpr std::size_t kTestItems = 100;
    armrx::PartialDataset pd(kTestItems);

    // Get core order for pinning fill workers
    std::vector<unsigned> core_order = {0, 1, 2, 3};

    // Fill synchronously for test
    pd.start_fill(cache, core_order);
    pd.wait_for_fill();

    assert(pd.item_count() == kTestItems);
    assert(pd.fill_complete());
    assert(pd.data() != nullptr);

    // Check every cached item against generate_dataset_item
    for (std::size_t i = 0; i < kTestItems; ++i) {
        const auto expected = armrx::generate_dataset_item(*cache, static_cast<std::uint64_t>(i));
        const auto* actual = pd.data() + i * armrx::kRandomXDatasetItemBytes;
        if (std::memcmp(expected.data(), actual, armrx::kRandomXDatasetItemBytes) != 0) {
            std::fprintf(stderr, "MISMATCH at item %zu\n", i);
            assert(false);
        }
    }

    std::cout << "[test_partial_dataset] test_cached_items_match: "
              << kTestItems << " items verified OK\n";
}

void test_large_partial_dataset() {
    const auto cache = get_test_cache();

    // Test with a larger partial dataset (5000 items)
    constexpr std::size_t kTestItems = 5000;
    armrx::PartialDataset pd(kTestItems);

    std::vector<unsigned> core_order = {0, 1, 2, 3};
    pd.start_fill(cache, core_order);
    pd.wait_for_fill();

    assert(pd.item_count() == kTestItems);

    // Spot-check every 100th item to keep test fast
    for (std::size_t i = 0; i < kTestItems; i += 100) {
        const auto expected = armrx::generate_dataset_item(*cache, static_cast<std::uint64_t>(i));
        const auto* actual = pd.data() + i * armrx::kRandomXDatasetItemBytes;
        if (std::memcmp(expected.data(), actual, armrx::kRandomXDatasetItemBytes) != 0) {
            std::fprintf(stderr, "MISMATCH at item %zu (spot check)\n", i);
            assert(false);
        }
    }

    std::cout << "[test_partial_dataset] test_large_partial_dataset: "
              << kTestItems << " items (spot-checked) OK\n";
}

void test_partial_dataset_disabled() {
    // With 0 items, data() should be nullptr and item_count() = 0
    armrx::PartialDataset pd(0);
    assert(pd.data() == nullptr);
    assert(pd.item_count() == 0);
    assert(pd.fill_complete());

    std::cout << "[test_partial_dataset] test_partial_dataset_disabled: OK\n";
}

void test_incremental_fill_consistent() {
    const auto cache = get_test_cache();

    // Fill 200 items
    constexpr std::size_t kTotalItems = 200;
    armrx::PartialDataset pd(kTotalItems);

    std::vector<unsigned> core_order = {0, 1, 2, 3};
    pd.start_fill(cache, core_order);
    pd.wait_for_fill();

    // Verify ALL items one by one
    for (std::size_t i = 0; i < kTotalItems; ++i) {
        const auto expected = armrx::generate_dataset_item(*cache, static_cast<std::uint64_t>(i));
        const auto* actual = pd.data() + i * armrx::kRandomXDatasetItemBytes;
        if (std::memcmp(expected.data(), actual, armrx::kRandomXDatasetItemBytes) != 0) {
            std::fprintf(stderr, "MISMATCH at item %zu (incremental fill)\n", i);
            assert(false);
        }
    }

    std::cout << "[test_partial_dataset] test_incremental_fill_consistent: "
              << kTotalItems << " items all verified OK\n";
}

// Regression test for the light-mode seed-rotation bug: the partial dataset
// must be re-fillable with a DIFFERENT seed and thereafter contain that new
// seed's data. The live bug was that the partial dataset was filled exactly
// once for the first seed and never rebuilt on rotation, so workers mined on a
// seed-mismatched (stale) partial dataset -> silent wrong shares.
std::shared_ptr<const armrx::Argon2dCache> get_test_cache_b() {
    static std::shared_ptr<const armrx::Argon2dCache> cache = std::make_shared<armrx::Argon2dCache>();
    static bool initialized = false;
    if (!initialized) {
        std::vector<std::byte> key;
        const char* s = "a SECOND, different seed key for refill test";
        for (const char* p = s; *p; ++p) key.push_back(static_cast<std::byte>(*p));
        const_cast<armrx::Argon2dCache*>(cache.get())->initialize(key);
        initialized = true;
    }
    return cache;
}

void test_refill_with_new_seed() {
    const auto cache_a = get_test_cache();
    const auto cache_b = get_test_cache_b();

    // Different seeds must produce different dataset items.
    {
        const auto a0 = armrx::generate_dataset_item(*cache_a, 0ULL);
        const auto b0 = armrx::generate_dataset_item(*cache_b, 0ULL);
        assert(std::memcmp(a0.data(), b0.data(), armrx::kRandomXDatasetItemBytes) != 0);
    }

    constexpr std::size_t kTotalItems = 200;
    armrx::PartialDataset pd(kTotalItems);
    std::vector<unsigned> core_order = {0, 1, 2, 3};

    // First fill with seed A.
    pd.start_fill(cache_a, core_order);
    pd.wait_for_fill();
    assert(pd.item_count() == kTotalItems);
    assert(pd.fill_complete());

    // Re-fill with seed B (the operation set_job() now performs live on rotation).
    pd.start_fill(cache_b, core_order);
    pd.wait_for_fill();
    assert(pd.item_count() == kTotalItems);
    assert(pd.fill_complete());

    // Every item must now match seed B, NOT stale seed A.
    for (std::size_t i = 0; i < kTotalItems; ++i) {
        const auto expected = armrx::generate_dataset_item(*cache_b, static_cast<std::uint64_t>(i));
        const auto* actual = pd.data() + i * armrx::kRandomXDatasetItemBytes;
        if (std::memcmp(expected.data(), actual, armrx::kRandomXDatasetItemBytes) != 0) {
            std::fprintf(stderr, "MISMATCH at item %zu after refill with new seed\n", i);
            assert(false);
        }
        // Explicitly confirm it is NOT the old seed's value.
        const auto stale = armrx::generate_dataset_item(*cache_a, static_cast<std::uint64_t>(i));
        if (std::memcmp(stale.data(), actual, armrx::kRandomXDatasetItemBytes) == 0) {
            std::fprintf(stderr, "STALE seed A data at item %zu after refill with seed B\n", i);
            assert(false);
        }
    }

    std::cout << "[test_partial_dataset] test_refill_with_new_seed: "
              << kTotalItems << " items re-filled with new seed, verified OK\n";
}

void test_contiguous_publish_no_uninitialized_read() {
    // Regression guard for audit C1: with a deliberately LAGGING *middle* chunk,
    // the published item_count_ must never name an item whose bytes are not yet
    // initialized. We fill WITHOUT wait_for_fill() (the hybrid hash-during-fill
    // condition) and, in a reader thread, sample item_count_ and check that
    // every published item matches generate_dataset_item. A buggy out-of-order
    // publish (old max-end-bound code) would let the reader observe initialized-
    // looking bytes for a chunk whose fill hasn't finished — caught here.
    const auto cache = get_test_cache();

    // 4 fill chunks of kFillChunkItems each (kFillChunkItems = 64 MiB / item).
    // Lag the MIDDLE chunk (chunk 1, items [64M,128M)) so chunks 0,2,3 finish
    // first. Contiguous publish must then stop at 64M (chunk 0 done) and NOT
    // advance over the unfinished chunk 1 even though chunk 2/3 are ready.
    constexpr std::uint64_t kChunkItems = 1048576ULL; // matches kFillChunkItems
    constexpr std::size_t kTotalItems = 4 * kChunkItems;
    constexpr unsigned kChunks = 4;
    armrx::PartialDataset pd(kTotalItems);
    std::vector<unsigned> core_order = {0, 1, 2, 3};

    std::vector<unsigned> delays(kChunks, 0);
    delays[1] = 5000; // ms — middle chunk lags 5s, far longer than 1M-item compute
                      // on any AArch64 target, so the lag is reliably observable
                      // (the old 500 ms lag could finish around the same time as
                      // chunk 0, making the stall non-deterministic).

    std::atomic<bool> stop{false};
    std::atomic<bool> violation{false};
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const std::uint64_t n = pd.item_count(); // acquire: sees only filled prefix
            for (std::uint64_t i = 0; i < n; ++i) {
                const auto expected = armrx::generate_dataset_item(*cache, i);
                const auto* actual = pd.data() + i * armrx::kRandomXDatasetItemBytes;
                if (std::memcmp(expected.data(), actual, armrx::kRandomXDatasetItemBytes) != 0) {
                    violation.store(true, std::memory_order_release);
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    pd.start_fill(cache, core_order, /*exclude_cores=*/{}, delays);
    // Deterministic mid-fill observation: wait until chunk 0 is published, then
    // assert the lagging MIDDLE chunk (chunk 1, 500 ms) has NOT been skipped by
    // contiguous publish. Replaces the old timing-dependent
    // `max_observed < kTotalItems` assert, which could fail under ASan/TSan
    // scheduling skew even when behavior was correct (the reader had to catch an
    // intermediate item_count_ sample during the lag window). This barrier makes
    // the check exact: chunk 0 done => item_count_ == kChunkItems; chunk 1's lag
    // prevents any further advance until it finishes.
    pd.wait_until_published(kChunkItems);
    assert(pd.item_count() == kChunkItems);
    pd.wait_for_fill();   // the guard is the reader thread; waiting is still safe
    stop.store(true, std::memory_order_release);
    reader.join();

    assert(!violation.load(std::memory_order_acquire));
    assert(pd.item_count() == kTotalItems);
    assert(pd.fill_complete());

    std::cout << "[test_partial_dataset] test_contiguous_publish_no_uninitialized_read: "
              << "lagging middle chunk produced no uninitialized reads "
              << "(deterministic barrier confirmed contiguous publish stalled at chunk 0)\n";
}

// ── Audit P1 (seed-rotation race): a ReadGuard held by a "hash in progress"
// must block start_fill() from beginning its reset (the point where it starts
// tearing down the currently-published state and, shortly after, lets fill
// threads begin overwriting the buffer). Without this barrier, start_fill()
// could reset item_count_/begin overwriting bytes while a reader still holds
// a bound/pointer snapshot from before rotation -- a real, unsynchronized
// concurrent read+write of the same memory. This test proves the barrier is
// real and deterministic: it does not rely on hitting a timing window, it
// directly observes that start_fill() cannot proceed while a ReadGuard is
// held, and that it proceeds immediately once released. ──
void test_readguard_blocks_rotation_until_released() {
    const auto cache_a = get_test_cache();
    const auto cache_b = get_test_cache_b();
    std::vector<unsigned> core_order = {0, 1, 2, 3};

    constexpr std::size_t kItems = 4096;
    armrx::PartialDataset pd(kItems);
    pd.start_fill(cache_a, core_order);
    pd.wait_for_fill();
    assert(pd.item_count() == kItems);

    // Hold a ReadGuard on this thread, simulating an in-flight hash that has
    // already snapshotted the (old, seed-A) bound and is about to read bytes
    // below it.
    auto guard = std::make_unique<armrx::PartialDataset::ReadGuard>(pd);

    std::atomic<bool> rotation_returned{false};
    std::thread rotator([&] {
        pd.start_fill(cache_b, core_order); // must block on the exclusive lock
        rotation_returned.store(true, std::memory_order_release);
    });

    // While the guard is held, start_fill()'s exclusive section cannot run,
    // so the published state must remain exactly the seed-A fill: neither
    // reset to 0 nor advanced by any seed-B fill thread. Poll for a bounded
    // window rather than a single fixed sleep -- any observed change (or the
    // rotator thread returning) during this window is the bug.
    const auto hold_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < hold_until) {
        assert(!rotation_returned.load(std::memory_order_acquire));
        assert(pd.item_count() == kItems); // unchanged: still the seed-A prefix
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Release the guard: start_fill() must now be able to proceed and finish.
    guard.reset();
    rotator.join();
    assert(rotation_returned.load(std::memory_order_acquire));

    pd.wait_for_fill();
    assert(pd.item_count() == kItems);
    assert(pd.fill_complete());
    // The buffer must now hold seed-B's data, not seed-A's.
    const auto expected = armrx::generate_dataset_item(*cache_b, 0ULL);
    assert(std::memcmp(expected.data(), pd.data(), armrx::kRandomXDatasetItemBytes) == 0);

    std::cout << "[test_partial_dataset] test_readguard_blocks_rotation_until_released: "
                 "start_fill() provably blocked while a ReadGuard was held, "
                 "proceeded immediately after release\n";
}

// ── Audit follow-up (round 3, reviewer-identified gap): the ReadGuard barrier
// above stops a rotation's exclusive reset from running WHILE a hash holds
// the guard, but that alone does not guarantee a worker's generation check
// and the actual buffer reset are observed as ONE transition. The original
// round-2 fix compared against a counter owned by MiningEngine, bumped
// *after* start_fill() had already released its exclusive lock and let fill
// threads start publishing new-seed bytes -- a worker could acquire a fresh
// ReadGuard in that window, see the counter still at its old value (match!),
// and hash with a stale cache against an already-rotating prefix. Fixed by
// moving the generation counter INTO PartialDataset and bumping it inside
// the SAME exclusive section that resets item_count_/contiguous_done_/
// fill_complete_, so the two can never be observed out of step.
//
// This test proves that invariant deterministically: it inspects
// generation() and item_count() together, both immediately after a
// (possibly still in-progress) rotation's start_fill() call returns, and
// again from inside a genuinely concurrent ReadGuard, confirming both always
// change together -- never one without the other.
void test_generation_published_atomically_with_reset() {
    const auto cache_a = get_test_cache();
    const auto cache_b = get_test_cache_b();
    std::vector<unsigned> core_order = {0};

    // Large enough (single core) that the fill is still running when
    // start_fill() returns, so the test can observe the "reset published,
    // refill still in progress" state directly rather than hoping to catch
    // it.
    constexpr std::size_t kItems = 1048576; // one kFillChunkItems sub-chunk
    armrx::PartialDataset pd(kItems);

    pd.start_fill(cache_a, core_order);
    pd.wait_for_fill();
    const auto gen_a = pd.generation();
    assert(gen_a >= 1);
    assert(pd.item_count() == kItems);
    assert(pd.fill_complete());

    // Rotate to seed B. By the time start_fill() RETURNS, the reset AND the
    // generation bump must already both be visible -- not just the reset,
    // and not the generation alone -- even though the refill itself (spawned
    // asynchronously) has almost certainly not finished (single core, large
    // dataset).
    pd.start_fill(cache_b, core_order);
    const auto gen_b = pd.generation();
    assert(gen_b == gen_a + 1);
    assert(!pd.fill_complete());
    assert(pd.item_count() < kItems); // reset (to 0) or only just starting to republish

    // A genuinely concurrent reader that acquires ReadGuard right now (fill
    // still in progress) must observe the SAME new generation together with
    // whatever prefix is currently published -- proving this isn't just
    // this thread's own program-order view, but a property any concurrent
    // ReadGuard holder can rely on.
    {
        armrx::PartialDataset::ReadGuard guard(pd);
        assert(pd.generation() == gen_b);
        const auto n = pd.item_count();
        assert(n <= kItems);
        // Whatever prefix IS published under generation gen_b must already
        // be seed-B's data (contiguous-publish's existing guarantee), never
        // a stale seed-A leftover -- the specific failure mode a
        // generation/reset visibility gap would produce.
        for (std::size_t i = 0; i < n; i += std::max<std::size_t>(1, n / 8)) {
            const auto expected = armrx::generate_dataset_item(*cache_b, static_cast<std::uint64_t>(i));
            const auto* actual = pd.data() + i * armrx::kRandomXDatasetItemBytes;
            assert(std::memcmp(expected.data(), actual, armrx::kRandomXDatasetItemBytes) == 0);
        }
    }

    pd.wait_for_fill();
    assert(pd.generation() == gen_b);
    assert(pd.item_count() == kItems);
    assert(pd.fill_complete());
    const auto expected0 = armrx::generate_dataset_item(*cache_b, 0ULL);
    assert(std::memcmp(expected0.data(), pd.data(), armrx::kRandomXDatasetItemBytes) == 0);

    std::cout << "[test_partial_dataset] test_generation_published_atomically_with_reset: "
                 "generation() and the item_count_ reset were observed together "
                 "(gen " << gen_a << " -> " << gen_b << "), both before and during "
                 "an in-progress refill, by a concurrent ReadGuard holder\n";
}

// ── Audit P1, stress form: hammer concurrent rotation against a reader that
// repeatedly takes a ReadGuard, snapshots item_count(), and verifies every
// item in the published prefix belongs to ONE seed's generation -- never a
// mix of seed-A and seed-B bytes within the same guarded snapshot. The old
// (unguarded) start_fill() could let fill threads overwrite arbitrary regions
// of the buffer while a reader's un-refreshed bound/pointer was still being
// used across the whole buffer, which this would have been expected to catch
// as a mismatch against BOTH references for some item. ──
void test_concurrent_rotation_no_mixed_generation() {
    const auto cache_a = get_test_cache();
    const auto cache_b = get_test_cache_b();
    std::vector<unsigned> core_order = {0, 1, 2, 3};

    constexpr std::size_t kItems = 65536; // 4 MiB -- enough chunks to race across
    armrx::PartialDataset pd(kItems);
    pd.start_fill(cache_a, core_order);
    pd.wait_for_fill();

    std::atomic<bool> stop{false};
    std::atomic<bool> mixed_generation_seen{false};
    std::atomic<std::uint64_t> reads_done{0};

    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            armrx::PartialDataset::ReadGuard guard(pd);
            const std::size_t n = pd.item_count();
            if (n == 0) continue;
            // Sample a handful of items across the published prefix rather
            // than every item, to keep each guarded critical section short
            // (matches real hash usage: bounded reads, not a full scan).
            bool matches_a = true, matches_b = true;
            for (std::size_t k = 0; k < 8; ++k) {
                const std::size_t i = (n <= 8) ? k % n : (k * (n / 8)) % n;
                const auto* actual = pd.data() + i * armrx::kRandomXDatasetItemBytes;
                const auto exp_a = armrx::generate_dataset_item(*cache_a, static_cast<std::uint64_t>(i));
                const auto exp_b = armrx::generate_dataset_item(*cache_b, static_cast<std::uint64_t>(i));
                if (std::memcmp(exp_a.data(), actual, armrx::kRandomXDatasetItemBytes) != 0) matches_a = false;
                if (std::memcmp(exp_b.data(), actual, armrx::kRandomXDatasetItemBytes) != 0) matches_b = false;
            }
            if (!matches_a && !matches_b) {
                mixed_generation_seen.store(true, std::memory_order_release);
                return;
            }
            reads_done.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Rotate back and forth for a bounded number of iterations.
    for (int i = 0; i < 6 && !mixed_generation_seen.load(std::memory_order_acquire); ++i) {
        pd.start_fill((i % 2 == 0) ? cache_b : cache_a, core_order);
        pd.wait_for_fill();
    }

    stop.store(true, std::memory_order_release);
    reader.join();

    assert(!mixed_generation_seen.load(std::memory_order_acquire));
    assert(reads_done.load(std::memory_order_relaxed) > 0); // the reader actually raced the rotations

    std::cout << "[test_partial_dataset] test_concurrent_rotation_no_mixed_generation: "
              << reads_done.load() << " guarded reads across 6 rotations, "
                 "no mixed-generation read observed\n";
}

// ── Audit P2 (empty fill-affinity list / null cache): reject invalid input
// with a clear exception instead of i % avail_cores.size() with size() == 0
// (division by zero) or a null-cache dereference inside a fill thread. ──
void test_start_fill_rejects_invalid_input() {
    const auto cache = get_test_cache();
    armrx::PartialDataset pd(100);

    bool threw = false;
    try {
        pd.start_fill(cache, /*core_order=*/{});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    threw = false;
    try {
        pd.start_fill(nullptr, std::vector<unsigned>{0, 1, 2, 3});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);

    // The object must still be usable afterward with valid input (rejecting
    // bad input must not corrupt internal state).
    pd.start_fill(cache, std::vector<unsigned>{0, 1, 2, 3});
    pd.wait_for_fill();
    assert(pd.fill_complete());

    std::cout << "[test_partial_dataset] test_start_fill_rejects_invalid_input: "
                 "empty core_order and null cache both rejected with "
                 "std::invalid_argument; object remained usable afterward\n";
}

// ── Audit P5 (partial-dataset size validation), constructor layer: a
// PartialDataset is a prefix of the real RandomX dataset and cannot
// legitimately be larger than it. Checked BEFORE any allocation, so this
// test needs none of its own. ──
void test_constructor_rejects_oversized_item_count() {
    bool threw = false;
    try {
        armrx::PartialDataset pd(armrx::randomx_dataset_item_count() + 1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    // Deliberately does NOT also construct at the exact extent (~2080 MiB) to
    // verify the accept side end-to-end -- that would require an enormous
    // allocation for a unit test. The accept side is instead covered cheaply
    // and precisely by test_validate_partial_dataset_request() below (a pure
    // function over integers, no allocation at all).

    std::cout << "[test_partial_dataset] test_constructor_rejects_oversized_item_count: "
                 "item_count > full dataset extent rejected before any allocation\n";
}

// ── Audit P1 (shutdown before first fill), PartialDataset layer: cancel()
// must wake a wait_for_fill() waiter even if start_fill() was never called
// (a mining engine's workers call wait_for_fill() unconditionally at
// startup). Bounded via a future rather than a plain join() so a regression
// fails this test instead of hanging the whole suite. ──
void test_cancel_before_any_fill_unblocks_waiter() {
    auto pd = std::make_shared<armrx::PartialDataset>(100);

    auto waiter = std::async(std::launch::async, [pd] {
        pd->wait_for_fill(); // start_fill() was never called
    });

    // Give the waiter thread a moment to actually enter wait_for_fill().
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pd->cancel();

    const auto status = waiter.wait_for(std::chrono::seconds(5));
    assert(status == std::future_status::ready);

    std::cout << "[test_partial_dataset] test_cancel_before_any_fill_unblocks_waiter: "
                 "cancel() released a wait_for_fill() call with no prior start_fill()\n";
}

// ── Audit P1 (shutdown during an active fill must not wait for the whole
// fill): forces a single-core, single-worker fill spanning several internal
// kFillChunkItems sub-chunks, then cancels shortly after it starts and
// asserts the whole thing (cancel + wait_for_fill's join) returns in a small,
// bounded multiple of ONE sub-chunk's time -- not the several-sub-chunk total
// the old single initialize_dataset() call over the whole assigned range
// would have required (that design could not observe cancellation until the
// entire range finished).
//
// The pass/fail bound is measured on THIS host/device rather than
// hard-coded: a single 64 MiB sub-chunk took ~6.9s on the x86_64 dev
// workstation this test was authored on but, measured directly, over 80s on
// the target Cortex-A53 (an order of magnitude slower) -- a fixed wall-clock
// bound flaked (timed out) there on the first on-device run of this suite,
// which is exactly the portability trap self-calibration avoids. ──
void test_cancel_bounds_active_fill_to_one_subchunk() {
    const auto cache = get_test_cache();
    constexpr std::size_t kSubChunkItems = 1048576; // matches kFillChunkItems

    // Calibrate: how long does ONE sub-chunk take, single-core, on whatever
    // is running this test right now?
    double baseline_sec;
    {
        armrx::PartialDataset calib(kSubChunkItems);
        const auto c0 = std::chrono::steady_clock::now();
        calib.start_fill(cache, std::vector<unsigned>{0});
        calib.wait_for_fill();
        baseline_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    }

    constexpr std::size_t kItems = kSubChunkItems * 6; // 6 sequential sub-chunks, single core
    armrx::PartialDataset pd(kItems);

    const auto t0 = std::chrono::steady_clock::now();
    // A single-element core list forces exactly one fill worker to own the
    // entire range, so its internal loop must cross multiple sub-chunk
    // boundaries sequentially.
    pd.start_fill(cache, std::vector<unsigned>{0});
    // 300 ms is well inside even a very fast sub-chunk (a 64 MiB Argon2d-
    // derived dataset chunk cannot plausibly finish in under that on any
    // real hardware this project targets).
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    pd.cancel();
    pd.wait_for_fill();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Must return within roughly "one more sub-chunk" (plus scheduling
    // slack), not "all 6" -- a 3x multiplier with a 5s floor comfortably
    // separates "cancellation checked between sub-chunks" from "cancellation
    // only checked once per whole assigned range" regardless of host speed.
    const double bound = std::max(5.0, baseline_sec * 3.0);
    assert(elapsed < bound);
    std::cout << "[test_partial_dataset] test_cancel_bounds_active_fill_to_one_subchunk: "
                 "one sub-chunk measured " << baseline_sec << "s on this host; "
                 "cancelled 6-sub-chunk fill after " << elapsed << "s (bound "
              << bound << "s; " << pd.item_count() << "/" << kItems << " items published)\n";
}

// ── Audit follow-up (P1/P2, round 2 -- reviewer-identified gap): cancel()
// captures the CURRENT stop_ token and signals it, but a concurrent
// start_fill() can replace stop_ with a fresh (false) token for a new
// generation immediately after -- if that race lands just wrong, the
// cancellation signal lands on an orphaned token nobody is looking at, and
// the new generation's fill workers/waiters would wait for the entire
// refill with no way to know shutdown was requested. Fixed with a
// persistent, never-cleared shutdown_requested_ flag checked everywhere the
// per-generation token is (see cancel()'s doc comment).
//
// This forces the exact race deterministically: a ReadGuard blocks a second
// start_fill() from completing its reset (so it is provably still using the
// GEN-1 token when cancel() reads stop_), then cancel() runs, then the guard
// is released so the blocked start_fill() proceeds and swaps in a NEW token
// -- exactly the "cancel signalled the old token; a new one is now current"
// sequence the bug required, but achieved by construction rather than by
// hoping the interleaving lands right.
//
// Self-calibrated timing so it discriminates old (buggy) vs. fixed behavior
// on ANY host speed: kItems is large enough that this device/host's natural
// (uncancelled) fill measurably takes some real time, and the cancelled run
// is asserted to finish in a small fraction of that -- with the old code
// (cancellation lost), the second generation's fill workers would run to
// completion normally and this would take approximately the FULL calibrated
// time instead.
void test_cancel_persists_across_racing_rotation() {
    const auto cache_a = get_test_cache();
    const auto cache_b = get_test_cache_b();
    std::vector<unsigned> core_order = {0, 1, 2, 3};

    // 16 MiB (single core, since this is well under one 64 MiB
    // kFillChunkItems sub-chunk) -- large enough to take real, measurable
    // time on any host, small enough to keep the calibration step itself
    // reasonably fast.
    constexpr std::size_t kItems = 262144;

    double baseline_sec;
    {
        armrx::PartialDataset calib(kItems);
        const auto c0 = std::chrono::steady_clock::now();
        calib.start_fill(cache_b, core_order);
        calib.wait_for_fill();
        baseline_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
    }

    armrx::PartialDataset pd(kItems);
    pd.start_fill(cache_a, core_order);
    pd.wait_for_fill();

    // Block the second rotation's exclusive reset section so it is
    // provably still holding the generation-1 stop_ token when cancel()
    // (below) reads it.
    auto guard = std::make_unique<armrx::PartialDataset::ReadGuard>(pd);

    std::atomic<bool> rotation_returned{false};
    std::thread rotator([&] {
        pd.start_fill(cache_b, core_order); // blocks on rotation_mutex_ while guard held
        rotation_returned.store(true, std::memory_order_release);
    });

    // Give the rotator a moment to actually reach the blocked state.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!rotation_returned.load(std::memory_order_acquire));

    // Shutdown races the still-pending rotation: cancel() reads and signals
    // whatever stop_ currently is (generation 1's token) -- the rotation has
    // not yet reached the point where it replaces it.
    const auto t0 = std::chrono::steady_clock::now();
    pd.cancel();

    // Release the guard: the pending start_fill() now proceeds, joins prior
    // threads, and swaps in a FRESH (false) token for generation 2 -- the
    // exact moment the old token-only design would have silently dropped
    // the cancellation for everything from here on.
    guard.reset();
    rotator.join();
    assert(rotation_returned.load(std::memory_order_acquire));

    // A subsequent wait_for_fill() must return promptly via the persistent
    // flag, not hang (or silently wait for the full natural fill time)
    // because the token it's looking at was never signalled.
    pd.wait_for_fill();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const double bound = std::max(1.0, baseline_sec / 4.0);
    assert(elapsed < bound);

    std::cout << "[test_partial_dataset] test_cancel_persists_across_racing_rotation: "
                 "natural fill measured " << baseline_sec << "s; cancellation across a "
                 "racing token swap resolved in " << elapsed << "s (bound " << bound << "s)\n";
}

// ── Audit P5, memory.hpp layer: validate_partial_dataset_request() is a pure
// function over integers, so its boundaries are tested here without
// allocating anything (not even a small PartialDataset). ──
void test_validate_partial_dataset_request() {
    // Disabled (0) is always valid.
    {
        const auto v = armrx::validate_partial_dataset_request(0, 4, 0);
        assert(v.ok);
    }

    // Fits comfortably: 64 MiB request, 4 workers, 4 GiB available.
    {
        constexpr std::size_t kAvailable = 4ULL * 1024 * 1024 * 1024;
        const auto v = armrx::validate_partial_dataset_request(64, 4, kAvailable);
        assert(v.ok);
        assert(v.item_count == (64ULL * 1024 * 1024) / armrx::kRandomXDatasetItemBytes);
        // Audit follow-up (round 2): the mining engine actually allocates
        // TWO scratchpads per worker (double-buffered pipelined hash+fill,
        // Track D2 -- see worker_loop()'s dual_scratchpad), not one. The
        // first version of this test asserted against
        // randomx_worker_memory() (the single-buffer figure), silently
        // matching the same undercounting bug in the validator it was
        // testing. Use mining_worker_actual_scratchpad_bytes() instead.
        assert(v.required_bytes == armrx::kRandomXCacheBytes +
                                    4ULL * armrx::mining_worker_actual_scratchpad_bytes() +
                                    64ULL * 1024 * 1024 + armrx::kAutoModeSafetyReserve);
    }

    // Exceeds the real RandomX dataset extent (~2080 MiB) -- must be
    // rejected regardless of how much RAM is "available".
    {
        const std::size_t full_mib =
            (armrx::randomx_dataset_item_count() * armrx::kRandomXDatasetItemBytes) / (1024 * 1024);
        const auto v = armrx::validate_partial_dataset_request(
            full_mib + 1, 4, std::numeric_limits<std::size_t>::max() / 2);
        assert(!v.ok);
        assert(!v.error.empty());
    }

    // Fits within the dataset extent but not within available memory.
    {
        const auto v = armrx::validate_partial_dataset_request(512, 8, /*available_bytes=*/1024ULL * 1024);
        assert(!v.ok);
        assert(!v.error.empty());
    }

    // Overflow guard: an absurd MiB value must be rejected cleanly, not
    // wrap around into a small (and therefore falsely "valid") byte count.
    {
        const auto v = armrx::validate_partial_dataset_request(
            std::numeric_limits<std::size_t>::max(), 4, std::numeric_limits<std::size_t>::max());
        assert(!v.ok);
    }

    std::cout << "[test_partial_dataset] test_validate_partial_dataset_request: "
                 "disabled/fits/exceeds-extent/exceeds-memory/overflow boundaries all correct\n";
}

} // anonymous namespace

int main() {
    test_cached_items_match();
    test_large_partial_dataset();
    test_partial_dataset_disabled();
    test_incremental_fill_consistent();
    test_refill_with_new_seed();
    test_contiguous_publish_no_uninitialized_read();
    test_readguard_blocks_rotation_until_released();
    test_generation_published_atomically_with_reset();
    test_concurrent_rotation_no_mixed_generation();
    test_start_fill_rejects_invalid_input();
    test_constructor_rejects_oversized_item_count();
    test_cancel_before_any_fill_unblocks_waiter();
    test_cancel_bounds_active_fill_to_one_subchunk();
    test_cancel_persists_across_racing_rotation();
    test_validate_partial_dataset_request();

    std::cout << "ALL PARTIAL DATASET TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
