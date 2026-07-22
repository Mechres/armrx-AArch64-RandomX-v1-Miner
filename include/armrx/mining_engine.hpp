#pragma once

#include "armrx/mining_common.hpp"
#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include "armrx/virtual_memory.h"

namespace armrx {

// RAII wrapper for mmap'd memory with huge-page hint.
// If huge pages are unavailable, falls back to plain anonymous memory.
class MappedMemory {
public:
    MappedMemory() = default;

    explicit MappedMemory(std::size_t bytes) {
        if (bytes == 0) return;
        // Try MAP_HUGETLB first (true huge pages, no THP dependency)
        void* ptr = allocLargePagesMemory(bytes);
        if (ptr) {
            data_ = static_cast<std::byte*>(ptr);
            size_ = bytes;
            return;
        }
        // Fallback: plain anonymous + MADV_HUGEPAGE (relies on THP)
        ptr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) throw std::bad_alloc();
        ::madvise(ptr, bytes, MADV_HUGEPAGE);
        data_ = static_cast<std::byte*>(ptr);
        size_ = bytes;
    }

    ~MappedMemory() {
        if (data_) ::munmap(data_, size_);
    }

    MappedMemory(const MappedMemory&) = delete;
    MappedMemory& operator=(const MappedMemory&) = delete;

    MappedMemory(MappedMemory&& other) noexcept
        : data_(other.data_), size_(other.size_)
    {
        other.data_ = nullptr;
        other.size_ = 0;
    }

    MappedMemory& operator=(MappedMemory&& other) noexcept {
        if (this != &other) {
            if (data_) ::munmap(data_, size_);
            data_ = other.data_;
            size_ = other.size_;
            other.data_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    std::byte* data() noexcept { return data_; }
    const std::byte* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

enum class AffinityMode {
    All,
    Unpinned,
    BigOnly
};

class MiningEngine {
public:
    using ShareCallback = std::function<void(const Job& job, std::uint64_t nonce, std::array<std::byte, 32> hash)>;

    MiningEngine(RandomXMode mode, unsigned int num_threads);
    ~MiningEngine();

    void start(ShareCallback callback);
    void stop();

    void set_rt_priority(bool enable) { rt_priority_ = enable; }
    void set_stagger_ms(unsigned ms) { stagger_ms_ = ms; }
    void set_affinity_mode(AffinityMode mode) { affinity_mode_ = mode; }

    void set_job(const Job& job);

    [[nodiscard]] std::uint64_t total_hashes() const;
    [[nodiscard]] double hash_rate() const;
    [[nodiscard]] double worker_hash_rate(unsigned int thread_id) const;

    /// Atomic snapshot of per-worker hash counts + wall-clock time.
    /// Take two snapshots and diff them to get steady-state (post-warmup) rates.
    struct HashSnapshot {
        std::chrono::steady_clock::time_point ts;
        std::uint64_t total;                     ///< sum of all workers
        std::vector<std::uint64_t> per_worker;   ///< indexed by thread_id
    };
    [[nodiscard]] HashSnapshot snapshot() const;
    [[nodiscard]] unsigned int num_workers() const { return num_workers_; }

#ifdef ARMRX_JIT_PROFILE
    [[nodiscard]] std::uint64_t total_jit_compile_time_ns() const { return total_jit_compile_time_ns_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t total_jit_execute_time_ns() const { return total_jit_execute_time_ns_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t total_jit_runs() const { return total_jit_runs_.load(std::memory_order_relaxed); }
#else
    [[nodiscard]] std::uint64_t total_jit_compile_time_ns() const { return 0; }
    [[nodiscard]] std::uint64_t total_jit_execute_time_ns() const { return 0; }
    [[nodiscard]] std::uint64_t total_jit_runs() const { return 0; }
#endif

private:
    void worker_loop(unsigned int thread_id);
    bool update_nonce_in_template(std::vector<std::byte>& block, std::uint64_t nonce, std::size_t offset, std::size_t size);

    RandomXMode mode_;
    unsigned int num_threads_;
    ShareCallback share_callback_;

    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> total_hashes_{0};
    // Per-worker hash counters (C-style array to avoid std::atomic move issues)
    std::unique_ptr<std::atomic<std::uint64_t>[]> worker_hashes_;
    unsigned int num_workers_{0};
    std::chrono::steady_clock::time_point start_time_;

#ifdef ARMRX_JIT_PROFILE
    std::atomic<std::uint64_t> total_jit_compile_time_ns_{0};
    std::atomic<std::uint64_t> total_jit_execute_time_ns_{0};
    std::atomic<std::uint64_t> total_jit_runs_{0};
#endif

    // Lock-free job distribution: workers compare generation counter to avoid mutex
    std::mutex job_mutex_;
    Job current_job_;
    std::atomic<std::uint64_t> job_generation_{0};
    bool has_job_{false};
    bool rt_priority_ = false;
    unsigned stagger_ms_ = 0;
    AffinityMode affinity_mode_ = AffinityMode::All;

    // Shared Cache and Dataset
    std::shared_ptr<Argon2dCache> shared_cache_;
    std::shared_ptr<MappedMemory> shared_dataset_; // Huge-page backed dataset for safe concurrent access
    std::vector<std::byte> current_seed_key_;

    // Dataset (re)initialization coordination — while the engine is already
    // running, a fast-mode seed-key change lets the persistent, already
    // affinity-pinned mining workers build the new dataset directly instead
    // of spawning temporary unpinned threads (see PLAN.md §2.1). This is a
    // separate mutex from job_mutex_ deliberately: set_job() holds job_mutex_
    // for its entire duration (including the wait below), so workers must be
    // able to participate without ever needing to acquire job_mutex_.
    //
    // Uses the same monotonic-generation-counter idiom as job_generation_/
    // local_gen below rather than a boolean flag: each worker tracks its own
    // local_dataset_init_gen and only participates once per bump, so there is
    // no reset step and no race window where a worker could see a stale
    // "pending" flag and redo its chunk a second time.
    std::mutex dataset_init_mutex_;
    std::condition_variable dataset_init_cv_;
    std::atomic<std::uint64_t> dataset_init_generation_{0};
    unsigned dataset_init_remaining_{0};             // guarded by dataset_init_mutex_
    std::shared_ptr<MappedMemory> pending_dataset_;  // guarded by dataset_init_mutex_
    std::shared_ptr<Argon2dCache> pending_cache_;    // guarded by dataset_init_mutex_
    std::uint64_t pending_items_per_thread_{0};      // guarded by dataset_init_mutex_

    // CPU core ordering: fastest cores first (big.LITTLE-aware)
    std::vector<unsigned int> core_order_;

    std::vector<std::thread> workers_;
};

} // namespace armrx
