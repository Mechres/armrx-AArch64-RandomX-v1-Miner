#pragma once

#include "armrx/argon2.hpp"
#include "armrx/dataset.hpp"
#include "armrx/randomx_config.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <thread>

namespace armrx {

/**
 * PartialDataset — a memory-mapped prefix of the full RandomX dataset.
 *
 * In hybrid mode (light mode with a cached prefix), the JIT checks
 * item_number < partial_dataset_items_ before choosing the fast path
 * (direct load from this buffer) vs. the derivation path (existing
 * light-mode on-the-fly derivation).
 *
 * The buffer is mmap'd with MADV_HUGEPAGE and filled incrementally in
 * background threads with explicit CPU affinity (mirroring worker_loop()'s
 * AffinityMode::All pattern) — critical under isolcpus deployments where
 * un-pinned fill threads silently serialize onto one core (measured ~4-5×
 * slower during Gate A validation).
 */
class PartialDataset {
public:
    /// Construct with N items. N=0 means disabled (no allocation).
    explicit PartialDataset(std::size_t item_count);

    ~PartialDataset();

    PartialDataset(const PartialDataset&) = delete;
    PartialDataset& operator=(const PartialDataset&) = delete;
    PartialDataset(PartialDataset&&) = delete;
    PartialDataset& operator=(PartialDataset&&) = delete;

    /// Returns the number of cached items (0 = disabled).
    [[nodiscard]] std::size_t item_count() const { return item_count_.load(std::memory_order_acquire); }

    /// Returns a pointer to the atomic item count for live JIT bound checking.
    /// The JIT reads this pointer every hash via memory_order_acquire, paired
    /// with the fill worker's memory_order_release CAS — guaranteeing written
    /// item bytes are visible when item_count increases.
    [[nodiscard]] const std::atomic<std::size_t>* item_count_atomic() const { return &item_count_; }

    /// Returns a pointer to the start of the cached data (or nullptr if disabled).
    [[nodiscard]] const std::byte* data() const { return data_; }

    /// Returns a read-only span over the currently-filled portion.
    [[nodiscard]] std::span<const std::byte> span() const {
        const auto count = item_count_.load(std::memory_order_acquire);
        return {data_, count * kRandomXDatasetItemBytes};
    }

    /// Start background fill of the buffer using initialize_dataset.
    /// Fills from start_item to item_count, incrementally raising item_count_ as each
    /// chunk completes. Threads are explicitly pinned mirroring worker_loop()'s
    /// AffinityMode::All pattern, skipping any CPU IDs in `exclude_cores`
    /// (e.g. cores already occupied by mining workers).
    void start_fill(const Argon2dCache& cache,
                    const std::vector<unsigned>& core_order,
                    std::shared_ptr<void> cache_lifetime_holder = {},
                    const std::vector<unsigned>& exclude_cores = {});

    /// Returns true if the fill has completed (all items fully computed).
    [[nodiscard]] bool fill_complete() const {
        return fill_complete_.load(std::memory_order_acquire);
    }

    /// Wait for fill to complete (blocking, used in tests).
    void wait_for_fill();

private:
    std::byte* data_ = nullptr;
    std::size_t allocated_items_ = 0;
    std::atomic<std::size_t> item_count_{0};
    std::atomic<bool> fill_complete_{false};
    mutable std::vector<std::thread> fill_threads_;
    // Keeps the Argon2dCache alive while fill threads are running
    std::shared_ptr<void> cache_lifetime_holder_;

    void fill_worker(const Argon2dCache& cache,
                     std::uint64_t start_item,
                     std::uint64_t end_item,
                     unsigned cpu_id);
};

} // namespace armrx
