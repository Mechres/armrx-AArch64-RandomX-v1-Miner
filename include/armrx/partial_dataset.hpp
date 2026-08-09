#pragma once

#include "armrx/argon2.hpp"
#include "armrx/dataset.hpp"
#include "armrx/randomx_config.hpp"

#include <memory>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <condition_variable>
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

    /// Number of fill chunks from the most recent start_fill (for the contiguous
    /// publish cursor). 0 if no fill has started.
    [[nodiscard]] std::size_t chunk_count() const {
        return chunk_done_ ? chunk_done_->size() : 0;
    }

    /// Start background fill of the buffer using initialize_dataset.
    /// Fills from start_item to item_count, incrementally raising item_count_ as each
    /// chunk completes. Threads are explicitly pinned mirroring worker_loop()'s
    /// AffinityMode::All pattern, skipping any CPU IDs in `exclude_cores`
    /// (e.g. cores already occupied by mining workers).
    /// `chunk_delays_ms` (optional, test-only) injects a per-chunk startup delay so a
    /// test can force a lagging chunk and verify contiguous-publish safety.
    void start_fill(std::shared_ptr<const Argon2dCache> cache_holder,
                    const std::vector<unsigned>& core_order,
                    const std::vector<unsigned>& exclude_cores = {},
                    const std::vector<unsigned>& chunk_delays_ms = {});

    /// Returns true if the fill has completed (all items fully computed).
    [[nodiscard]] bool fill_complete() const {
        return fill_complete_.load(std::memory_order_acquire);
    }

    /// Wait for fill to complete (blocking, used in tests and by the mining
    /// workers at startup). The fill-thread join is guarded by fill_join_mutex_
    /// so multiple concurrent callers (one per mining worker) cannot race on
    /// join() of the same threads.
    void wait_for_fill();

    /// Block until at least `count` items are published (the contiguous prefix
    /// [0, count) is fully initialized). Replaces timing-dependent test polling:
    /// a test can observe a deterministic mid-fill state (e.g. a lagging chunk
    /// has NOT been skipped by contiguous publish) without relying on the
    /// scheduler catching an intermediate item_count_ sample. Returns once
    /// item_count_ >= count or the fill is cancelled (stop_ set).
    void wait_until_published(std::size_t count);

private:
    std::byte* data_ = nullptr;
    std::size_t allocated_items_ = 0;
    std::atomic<std::size_t> item_count_{0};
    std::atomic<bool> fill_complete_{false};
    // Contiguous-publish cursor: the highest item such that EVERY item in
    // [0, contiguous_done_) is fully initialized. item_count_ is advanced only
    // up to this bound, so any item < item_count_ is safe to read (no
    // out-of-order publish of a lagging chunk's bytes). Set by whichever fill
    // worker closes the contiguous gap after its chunk finishes.
    std::atomic<std::uint64_t> contiguous_done_{0};
    // Items per fill chunk (set in start_fill; last chunk may be smaller). Used
    // by the contiguous-publish cursor to map a chunk index to its item span.
    std::uint64_t items_per_chunk_{0};
    // One done-flag per fill chunk (indexed by chunk id assigned in start_fill).
    // A chunk sets its flag (release) after initialize_dataset completes, then
    // attempts to advance contiguous_done_ over now-contiguous finished chunks.
    std::unique_ptr<std::vector<std::atomic<bool>>> chunk_done_;
    mutable std::vector<std::thread> fill_threads_;
    // Guards the fill-thread join in wait_for_fill() against concurrent callers.
    mutable std::mutex fill_join_mutex_;
    // Condition variable + mutex backing wait_for_fill() and wait_until_published():
    // replaces the old 100 ms poll loop (test flakiness / wakeup latency). Notified
    // by fill_worker when item_count_ advances or fill_complete_ is set, and by the
    // destructor on cancellation so a waiter never blocks forever.
    mutable std::mutex fill_cv_mutex_;
    std::condition_variable fill_cv_;
    // Keeps the Argon2dCache alive while fill threads are running
    std::shared_ptr<const Argon2dCache> cache_holder_;
    // Shared independently of PartialDataset so a worker never needs the object
    // to stay alive to observe cancellation.
    std::shared_ptr<std::atomic<bool>> stop_ =
        std::make_shared<std::atomic<bool>>(false);

    void fill_worker(std::shared_ptr<const Argon2dCache> cache_holder,
                     std::shared_ptr<std::atomic<bool>> stop,
                     std::uint64_t start_item,
                     std::uint64_t end_item,
                     unsigned cpu_id,
                     std::size_t chunk_id,
                     unsigned delay_ms);
};

} // namespace armrx
