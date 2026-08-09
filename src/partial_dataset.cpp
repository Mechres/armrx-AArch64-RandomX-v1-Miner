#include "armrx/partial_dataset.hpp"
#include "armrx/dataset.hpp"
#include "armrx/log.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace armrx {

namespace {

/// Size of each chunk per thread when filling. 64 MiB chunks give good
/// parallelism without excessive thread creation overhead.
constexpr std::uint64_t kFillChunkBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kFillChunkItems = kFillChunkBytes / kRandomXDatasetItemBytes;

} // namespace

PartialDataset::PartialDataset(std::size_t item_count)
    : allocated_items_(item_count)
{
    if (item_count == 0) {
        fill_complete_.store(true, std::memory_order_release);
        return;
    }

    const std::size_t total_bytes = item_count * kRandomXDatasetItemBytes;

    // Simple mmap with transparent hugepage hint
    data_ = static_cast<std::byte*>(
        ::mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (data_ == MAP_FAILED) {
        throw std::bad_alloc();
    }

    // Hugepage hint (best-effort; succeeds silently if THP is available)
    ::madvise(data_, total_bytes, MADV_HUGEPAGE);

    // Pre-fault the buffer so THP promotion happens NOW, at init, instead of
    // lazily on first random write-touch during hashing. Without this, a 256 MiB
    // (light) / 512 MiB buffer is promoted to 2 MB huge pages page-by-page under
    // mining load -- a soft-fault storm that drags the hashrate up gradually over
    // ~20 minutes (observed) before reaching steady state. XMRig pre-faults its
    // dataset and is at full speed immediately. Mirrors the pre-fault already done
    // for the scratchpad (vm.cpp) and Argon2d cache (argon2.cpp).
    // MADV_POPULATE_WRITE (Linux 5.14+) prefaults writable pages without memset.
#if defined(MADV_POPULATE_WRITE)
    ::madvise(data_, total_bytes, MADV_POPULATE_WRITE);
#else
    // Fallback: touch every page to fault it in (mmap/MADV_HUGEPAGE anon is
    // already zero, so writing zeros only forces the fault + THP promotion).
    constexpr std::size_t kPage = 4096;
    volatile std::byte* p = data_;
    for (std::size_t off = 0; off < total_bytes; off += kPage)
        p[off];  // read-touch faults the page; combined with MADV_HUGEPAGE the
                  // kernel promotes to a huge page on the write that follows.
    // Force write fault (promotes THP): write a zero to each page.
    for (std::size_t off = 0; off < total_bytes; off += kPage)
        p[off] = std::byte{0};
#endif

    ARMRX_LOG_INFO << "PartialDataset: allocated " << item_count
                   << " items (" << (total_bytes / (1024ULL * 1024ULL))
                   << " MiB)";
}

PartialDataset::~PartialDataset() {
    const bool fill_finished =
        item_count_.load(std::memory_order_acquire) >= allocated_items_;
    if (!fill_finished && stop_) {
        stop_->store(true, std::memory_order_release);
        // Wake any wait_for_fill()/wait_until_published() waiter so a cancelled
        // fill does not block forever (predicate also checks stop_).
        fill_cv_.notify_all();
    }

    for (auto& t : fill_threads_) {
        if (t.joinable()) t.join();
    }

    if (data_) {
        ::munmap(data_, allocated_items_ * kRandomXDatasetItemBytes);
        data_ = nullptr;
    }
}

void PartialDataset::start_fill(std::shared_ptr<const Argon2dCache> cache_holder,
                                 const std::vector<unsigned>& core_order,
                                 const std::vector<unsigned>& exclude_cores,
                                 const std::vector<unsigned>& chunk_delays_ms)
{
    if (allocated_items_ == 0) return;

    // Serialize with any still-running prior fill: join and drop its threads
    // (they would otherwise keep writing this same buffer concurrently with
    // the new fill, corrupting it). This is what makes start_fill() safe to
    // call repeatedly across seed rotations. The mutex guards against a
    // concurrent wait_for_fill() also joining these threads.
    {
        std::lock_guard<std::mutex> lock(fill_join_mutex_);
        for (auto& t : fill_threads_) {
            if (t.joinable()) t.join();
        }
        fill_threads_.clear();
    }

    // Reset only after all workers from the previous generation have stopped.
    item_count_.store(0, std::memory_order_relaxed);
    contiguous_done_.store(0, std::memory_order_relaxed);
    fill_complete_.store(false, std::memory_order_relaxed);
    stop_ = std::make_shared<std::atomic<bool>>(false);

    // Keep the cache alive while fill threads are running
    cache_holder_ = cache_holder;

    const auto total_items = static_cast<std::uint64_t>(allocated_items_);

    // Build the list of available cores excluding mining cores
    std::vector<unsigned> avail_cores;
    for (auto c : core_order) {
        if (std::find(exclude_cores.begin(), exclude_cores.end(), c) == exclude_cores.end()) {
            avail_cores.push_back(c);
        }
    }
    if (avail_cores.empty()) {
        // Fallback: allow sharing if all cores excluded
        avail_cores = core_order;
    }

    const unsigned num_workers = static_cast<unsigned>(std::min<std::size_t>(
        (total_items + kFillChunkItems - 1) / kFillChunkItems,
        std::max<std::size_t>(1, avail_cores.size())));

    const auto items_per_worker = (total_items + num_workers - 1) / num_workers;

    // Contiguous-publish bookkeeping: one done-flag per fill chunk (chunk_id
    // indexes this vector). items_per_chunk_ maps a chunk index to its item span.
    chunk_done_ = std::make_unique<std::vector<std::atomic<bool>>>(num_workers);
    for (auto& f : *chunk_done_) f.store(false, std::memory_order_relaxed);
    items_per_chunk_ = items_per_worker;

    fill_threads_.reserve(num_workers);
    for (unsigned i = 0; i < num_workers; ++i) {
        const auto start = static_cast<std::uint64_t>(i) * items_per_worker;
        const auto end = std::min(start + items_per_worker, total_items);
        if (start >= end) break;

        const unsigned cpu_id = avail_cores[i % avail_cores.size()];
        const unsigned delay_ms = (i < chunk_delays_ms.size()) ? chunk_delays_ms[i] : 0;
        fill_threads_.emplace_back(&PartialDataset::fill_worker, this,
                                   cache_holder_, stop_, start, end, cpu_id, i,
                                   delay_ms);
    }

    ARMRX_LOG_INFO << "PartialDataset: started fill with " << num_workers
                   << " workers (" << items_per_worker << " items each)";
}

void PartialDataset::fill_worker(std::shared_ptr<const Argon2dCache> cache_holder,
                                 std::shared_ptr<std::atomic<bool>> stop,
                                 std::uint64_t start_item,
                                  std::uint64_t end_item,
                                  unsigned cpu_id,
                                  std::size_t chunk_id,
                                  unsigned delay_ms)
{
    // Keep the cache alive for the duration of this fill worker.
    // The shared_ptr is passed by value into the thread, so each worker
    // holds its own reference independently.

    // Test-only artificial lag so a test can force a lagging chunk (verifies
    // contiguous-publish safety). Production passes delay_ms == 0.
    if (stop->load(std::memory_order_acquire)) return;
    if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

    // Explicit CPU pinning (mirrors worker_loop()'s AffinityMode::All pattern)
    cpu_set_t cpus{};
    CPU_ZERO(&cpus);
    CPU_SET(static_cast<int>(cpu_id), &cpus);
    pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);

    const auto total_items = end_item - start_item;
    const auto byte_offset = start_item * kRandomXDatasetItemBytes;

    auto span = std::span<std::byte>(
        data_ + byte_offset,
        static_cast<std::size_t>(total_items * kRandomXDatasetItemBytes));

    // Fill the chunk using the existing vectorized initialize_dataset.
    // The cache is valid because cache_holder keeps it alive.
    initialize_dataset(span, *cache_holder, start_item, total_items);

    // Mark THIS chunk done (release: its bytes are now fully initialized and
    // visible to any reader that observes item_count_ >= end_item via acquire).
    (*chunk_done_)[chunk_id].store(true, std::memory_order_release);

    // Contiguous-publish: advance the published bound only over the prefix that
    // is NOW fully initialized. While the chunk immediately after the current
    // contiguous cursor is done, swallow it and keep going. This guarantees
    // item_count_ always names a fully-filled [0, item_count_) prefix, so a
    // hashing worker can safely read any item < item_count_ (no out-of-order
    // publish of a lagging chunk's uninitialized bytes -- closes audit C1).
    std::uint64_t cursor = contiguous_done_.load(std::memory_order_acquire);
    for (;;) {
        // Advance cursor over consecutive finished chunks. The chunk that
        // owns item `cursor` is chunk (cursor / items_per_chunk_); it is done
        // when its flag is set, and then the whole chunk's item span is safe.
        while (cursor < allocated_items_ && chunk_done_) {
            const std::size_t owning = static_cast<std::size_t>(cursor / items_per_chunk_);
            if (owning >= chunk_done_->size()) break;
            if (!(*chunk_done_)[owning].load(std::memory_order_acquire)) break;
            const std::uint64_t chunk_span = (owning + 1 < chunk_done_->size())
                ? items_per_chunk_
                : (allocated_items_ - owning * items_per_chunk_);
            cursor += chunk_span;
        }
        // Publish the new contiguous bound (release pairs with the JIT's acquire).
        const std::uint64_t prev = contiguous_done_.exchange(cursor, std::memory_order_release);
        // Also raise the legacy item_count_ to the contiguous bound so existing
        // readers (span(), wait_for_fill polling) see the safe prefix.
        std::uint64_t item_prev = item_count_.load(std::memory_order_relaxed);
        while (item_prev < cursor &&
               !item_count_.compare_exchange_weak(item_prev, cursor,
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) {
            // another worker published further; loop
        }
        // Wake any wait_for_fill()/wait_until_published() waiter now that the
        // contiguous bound advanced (deterministic mid-fill observation).
        fill_cv_.notify_all();
        // If we made no progress, another worker will handle further advances
        // when its chunk completes. Terminate.
        if (cursor == prev) break;
        // Re-load and try again (a chunk after `prev` may now be done).
        cursor = contiguous_done_.load(std::memory_order_acquire);
    }

    // Mark the whole fill complete once the contiguous prefix reaches the end.
    if (!stop->load(std::memory_order_acquire) &&
        contiguous_done_.load(std::memory_order_acquire) >= allocated_items_) {
        fill_complete_.store(true, std::memory_order_release);
        // Wake wait_for_fill() (and any wait_until_published) — full completion.
        fill_cv_.notify_all();
    }

    ARMRX_LOG_DEBUG << "PartialDataset worker on cpu " << cpu_id
                    << ": filled items [" << start_item << ", " << end_item << ")";
}

void PartialDataset::wait_for_fill() {
    // Wait for fill completion via condition variable (replaces the old 100 ms
    // poll loop, which added wakeup latency and test flakiness under TSAN/ASan
    // scheduling skew). Wakes on fill_complete_ OR cancellation (stop_) so a
    // cancelled fill never blocks forever.
    //
    // The fill workers raise the atomic item_count_/fill_complete_ and call
    // fill_cv_.notify_all() WITHOUT holding fill_cv_mutex_ (the hot publish
    // path is intentionally lock-free). A notify_all() that lands in the
    // window between the predicate check and the futex wait is therefore lost,
    // which can block this waiter forever (no subsequent notify, no guaranteed
    // spurious wakeup) -- the observed test_partial_dataset deadlock. Re-check
    // the predicate on a short timeout backstop so liveness never depends on a
    // single notify. Correctness is unchanged: the published counters are
    // atomics and the CV is only a wakeup hint.
    {
        std::unique_lock<std::mutex> lk(fill_cv_mutex_);
        static constexpr auto kPoll = std::chrono::milliseconds(20);
        while (!fill_complete_.load(std::memory_order_acquire) &&
               !stop_->load(std::memory_order_acquire)) {
            fill_cv_.wait_for(lk, kPoll);
        }
    }

    // Log fill completion once per process (multiple workers may observe it).
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        ARMRX_LOG_INFO << "PartialDataset: fill complete — workers resuming";
    }

    // Join all threads. Guarded so concurrent callers (one per mining worker)
    // don't race on join() of the same fill threads (undefined behavior).
    {
        std::lock_guard<std::mutex> lock(fill_join_mutex_);
        for (auto& t : fill_threads_) {
            if (t.joinable()) t.join();
        }
        // Drop the (now-joined) handles so a subsequent start_fill() doesn't
        // re-emplace onto a vector of stale, unjoinable threads and grow it
        // unboundedly across many seed rotations. The next start_fill() also
        // clears this under the same mutex; this clears the trailing ones.
        fill_threads_.clear();
    }
}

void PartialDataset::wait_until_published(std::size_t count) {
    std::unique_lock<std::mutex> lk(fill_cv_mutex_);
    // See wait_for_fill(): a lock-free notify_all() from a fill worker can be
    // lost in the predicate-check/futex-wait window, so re-check the published
    // count on a short timeout backstop instead of relying on a single wakeup.
    static constexpr auto kPoll = std::chrono::milliseconds(20);
    while (item_count_.load(std::memory_order_acquire) < count &&
           !stop_->load(std::memory_order_acquire)) {
        fill_cv_.wait_for(lk, kPoll);
    }
}

} // namespace armrx
