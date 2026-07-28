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

    // Over-allocate by 2 MiB to ensure 2 MiB alignment for THP
    const std::size_t kHugePageSize = 2ULL * 1024ULL * 1024ULL;
    const std::size_t alloc_bytes = total_bytes + kHugePageSize;

    void* raw = ::mmap(nullptr, alloc_bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) {
        throw std::bad_alloc();
    }

    // Align to 2 MiB boundary for THP
    void* aligned = reinterpret_cast<void*>(
        (reinterpret_cast<std::uintptr_t>(raw) + kHugePageSize - 1) & ~(kHugePageSize - 1));
    data_ = static_cast<std::byte*>(aligned);

    // Unmap the unaligned prefix and suffix
    const auto prefix_bytes = static_cast<std::size_t>(
        static_cast<std::byte*>(aligned) - static_cast<std::byte*>(raw));
    if (prefix_bytes > 0) {
        ::munmap(raw, prefix_bytes);
    }
    const auto suffix_start = static_cast<std::byte*>(aligned) + total_bytes;
    const auto suffix_bytes = alloc_bytes - prefix_bytes - total_bytes;
    if (suffix_bytes > 0) {
        ::munmap(suffix_start, suffix_bytes);
    }

    // Hugepage hint
    ::madvise(data_, total_bytes, MADV_HUGEPAGE);

    // Do NOT prefault with MADV_POPULATE_WRITE — it faults in 4 KiB pages
    // and prevents THP coalescing. Let the fill workers' sequential writes
    // naturally allocate 2 MiB huge pages instead.

    ARMRX_LOG_INFO << "PartialDataset: allocated " << item_count
                   << " items (" << (total_bytes / (1024ULL * 1024ULL))
                   << " MiB)";
}

PartialDataset::~PartialDataset() {
    // Ensure fill threads are done before unmapping
    for (auto& t : fill_threads_) {
        if (t.joinable()) t.join();
    }
    if (data_) {
        ::munmap(data_, allocated_items_ * kRandomXDatasetItemBytes);
        data_ = nullptr;
    }
}

void PartialDataset::start_fill(const Argon2dCache& cache,
                                 const std::vector<unsigned>& core_order,
                                 std::shared_ptr<void> cache_lifetime_holder,
                                 const std::vector<unsigned>& exclude_cores)
{
    if (allocated_items_ == 0) return;

    // Keep the cache alive while fill threads are running
    cache_lifetime_holder_ = std::move(cache_lifetime_holder);

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
                                   std::ref(cache), start, end, cpu_id);
    }

    ARMRX_LOG_INFO << "PartialDataset: started fill with " << num_workers
                   << " workers (" << items_per_worker << " items each)";
}

void PartialDataset::fill_worker(const Argon2dCache& cache,
                                  std::uint64_t start_item,
                                  std::uint64_t end_item,
                                  unsigned cpu_id)
{
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

    // Fill the chunk using the existing vectorized initialize_dataset
    initialize_dataset(span, cache, start_item, total_items);

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

    // Join all threads
    for (auto& t : fill_threads_) {
        if (t.joinable()) t.join();
    }
}

} // namespace armrx
