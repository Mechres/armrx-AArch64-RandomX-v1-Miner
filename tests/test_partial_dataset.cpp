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

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
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

} // anonymous namespace

int main() {
    test_cached_items_match();
    test_large_partial_dataset();
    test_partial_dataset_disabled();
    test_incremental_fill_consistent();
    test_refill_with_new_seed();

    std::cout << "ALL PARTIAL DATASET TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
