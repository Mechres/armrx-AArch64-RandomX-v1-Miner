#pragma once

#include "armrx/mining_common.hpp"
#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <functional>
#include <memory>
#include <chrono>

namespace armrx {

class MiningEngine {
public:
    using ShareCallback = std::function<void(const Job& job, std::uint64_t nonce, std::array<std::byte, 32> hash)>;

    MiningEngine(RandomXMode mode, unsigned int num_threads);
    ~MiningEngine();

    void start(ShareCallback callback);
    void stop();

    void set_job(const Job& job);

    [[nodiscard]] std::uint64_t total_hashes() const;
    [[nodiscard]] double hash_rate() const;
    [[nodiscard]] double worker_hash_rate(unsigned int thread_id) const;

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
    void update_nonce_in_template(std::vector<std::byte>& block, std::uint64_t nonce, std::size_t offset, std::size_t size);

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
    std::atomic<std::uint64_t> nonce_counter_{0};
    bool has_job_{false};

    // Shared Cache and Dataset
    std::shared_ptr<Argon2dCache> shared_cache_;
    std::shared_ptr<std::vector<std::byte>> shared_dataset_; // Shared pointer to dataset for safe concurrent access during transition
    std::vector<std::byte> current_seed_key_;

    std::vector<std::thread> workers_;
};

} // namespace armrx
