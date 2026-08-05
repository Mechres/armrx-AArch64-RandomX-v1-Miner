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
    // Detach any still-running fill threads. We check if fill completed
    // synchronously: if so, all threads have exited and we can unmap safely.
    // If not, the threads are still writing to data_ — skip munmap and let
    // the OS reclaim the mapping on process exit (the process is shutting
    // down anyway in this case, e.g. SIGTERM during a benchmark).
    // This avoids a race between fill_worker's data access and munmap.
    // Check if all items are filled (fill_worker threads have completed).
    // If the fill finished before shutdown, join and unmap cleanly.
    // If still in progress, detach and skip munmap to avoid racing with
    // fill_worker's data_ access — the OS reclaims the mapping on exit.
    bool fill_finished = (item_count_.load(std::memory_order_acquire) >= allocated_items_);
    for (auto& t : fill_threads_) {
        if (t.joinable()) {
            if (fill_finished) {
                t.join();
            } else {
                t.detach();
            }
        }
    }
    if (data_ && fill_finished) {
        ::munmap(data_, allocated_items_ * kRandomXDatasetItemBytes);
        data_ = nullptr;
    }
}

void PartialDataset::start_fill(std::shared_ptr<const Argon2dCache> cache_holder,
                                 const std::vector<unsigned>& core_order,
                                 const std::vector<unsigned>& exclude_cores)
{
    if (allocated_items_ == 0) return;

    // Re-fillable: a prior fill (e.g. for an earlier seed) may have already
    // advanced item_count_ and set fill_complete_. Reset both so this new fill
    // (with a possibly different cache/seed) starts from zero and is observed
    // as incomplete until the last chunk publishes the final item. Without
    // this, a second start_fill() would publish nothing (item_count_ already
    // at allocated_items_) and fill_complete_ would stay true — letting a
    // wait_for_fill() caller skip the re-wait entirely.
    item_count_.store(0, std::memory_order_relaxed);
    fill_complete_.store(false, std::memory_order_relaxed);

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

    fill_threads_.reserve(num_workers);
    for (unsigned i = 0; i < num_workers; ++i) {
        const auto start = static_cast<std::uint64_t>(i) * items_per_worker;
        const auto end = std::min(start + items_per_worker, total_items);
        if (start >= end) break;

        const unsigned cpu_id = avail_cores[i % avail_cores.size()];
        fill_threads_.emplace_back(&PartialDataset::fill_worker, this,
                                   cache_holder_, start, end, cpu_id);
    }

    ARMRX_LOG_INFO << "PartialDataset: started fill with " << num_workers
                   << " workers (" << items_per_worker << " items each)";
}

void PartialDataset::fill_worker(std::shared_ptr<const Argon2dCache> cache_holder,
                                  std::uint64_t start_item,
                                  std::uint64_t end_item,
                                  unsigned cpu_id)
{
    // Keep the cache alive for the duration of this fill worker.
    // The shared_ptr is passed by value into the thread, so each worker
    // holds its own reference independently.

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

    // Atomically advance the published bound. Each worker reports its
    // progress as the max item it has completed. The item_count_ is
    // monotonic — once an item is published, it's fully initialized.
    const auto completed = start_item + total_items;
    auto prev = item_count_.load(std::memory_order_relaxed);
    while (prev < completed &&
           !item_count_.compare_exchange_weak(prev, completed,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
        // CAS failed because another thread published a higher value; that's fine
    }

    // Mark the whole fill complete once the last chunk publishes the final
    // item. This lets fill_complete()/wait_for_fill() observe completion
    // reliably even if no caller invokes wait_for_fill() (the mining workers
    // block on wait_for_fill() at startup, so it must be set by the fill
    // itself, not only by wait_for_fill() polling item_count_).
    if (completed >= allocated_items_) {
        fill_complete_.store(true, std::memory_order_release);
    }

    ARMRX_LOG_DEBUG << "PartialDataset worker on cpu " << cpu_id
                    << ": filled items [" << start_item << ", " << end_item << ")";
}

void PartialDataset::wait_for_fill() {
    while (!fill_complete_.load(std::memory_order_acquire)) {
        // Check if all items are done
        if (item_count_.load(std::memory_order_acquire) >= allocated_items_) {
            fill_complete_.store(true, std::memory_order_release);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

} // namespace armrx
