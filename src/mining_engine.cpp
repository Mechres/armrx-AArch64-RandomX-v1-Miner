#include "armrx/mining_engine.hpp"
#include "armrx/dataset.hpp"
#include "armrx/randomx_config.hpp"
#include <iostream>
#include <cstring>
#include <fstream>
#include <string>
#include <algorithm>
#include <mutex>
#include <sched.h>
#include <pthread.h>

namespace armrx {

namespace {

// Detect CPU core ordering by max frequency. Fastest cores first.
// For big.LITTLE systems this pins workers to big cores first.
std::vector<unsigned int> detect_core_order() {
    unsigned int num_cpus = std::thread::hardware_concurrency();
    if (num_cpus == 0) return {0};

    std::vector<std::pair<unsigned long, unsigned int>> freq_cores;
    for (unsigned int i = 0; i < num_cpus; ++i) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/cpuinfo_max_freq";
        std::ifstream file(path);
        unsigned long freq = 0;
        if (file >> freq) {
            freq_cores.emplace_back(freq, i);
        }
    }

    if (freq_cores.empty()) {
        // Fallback: no cpufreq info — use sequential order
        std::vector<unsigned int> fallback(num_cpus);
        for (unsigned int i = 0; i < num_cpus; ++i) fallback[i] = i;
        return fallback;
    }

    // Sort by frequency descending (big cores first)
    std::sort(freq_cores.begin(), freq_cores.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<unsigned int> order;
    order.reserve(freq_cores.size());
    for (const auto& fc : freq_cores) order.push_back(fc.second);
    return order;
}

} // namespace

MiningEngine::MiningEngine(RandomXMode mode, unsigned int num_threads)
    : mode_(mode), num_threads_(num_threads) {
    core_order_ = detect_core_order();
}

MiningEngine::~MiningEngine() {
    stop();
}

void MiningEngine::start(ShareCallback callback) {
    if (running_.load()) {
        return;
    }
    share_callback_ = std::move(callback);
    running_.store(true);
    total_hashes_.store(0);
    worker_hashes_ = std::make_unique<std::atomic<std::uint64_t>[]>(num_threads_);
    num_workers_ = num_threads_;
    start_time_ = std::chrono::steady_clock::now();

    workers_.clear();
    workers_.reserve(num_threads_);
    for (unsigned int i = 0; i < num_threads_; ++i) {
        workers_.emplace_back(&MiningEngine::worker_loop, this, i);
    }
}

void MiningEngine::stop() {
    if (!running_.load()) {
        return;
    }
    running_.store(false);
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }
    workers_.clear();
}

void MiningEngine::set_job(const Job& job) {
    std::lock_guard<std::mutex> lock(job_mutex_);

    bool key_changed = current_seed_key_ != job.seed_key;
    if (key_changed || !shared_cache_) {
        std::cout << "[MiningEngine] New seed key detected. Initializing cache...\n";
        auto new_cache = std::make_shared<Argon2dCache>();
        new_cache->initialize(job.seed_key);
        shared_cache_ = new_cache;

        if (mode_ == RandomXMode::fast) {
            auto dataset_bytes = randomx_dataset_item_count() * 64;
            std::cout << "[MiningEngine] Initializing " << dataset_bytes / (1024U * 1024U) << " MiB dataset...\n";
            auto new_dataset = std::make_shared<MappedMemory>(dataset_bytes);

            // Parallelize dataset initialization
            unsigned int init_threads_count = std::max(1U, std::thread::hardware_concurrency());
            std::vector<std::thread> init_threads;
            std::uint64_t items_per_thread = randomx_dataset_item_count() / init_threads_count;
            for (unsigned int i = 0; i < init_threads_count; ++i) {
                std::uint64_t start_item = i * items_per_thread;
                std::uint64_t count = (i == init_threads_count - 1)
                    ? (randomx_dataset_item_count() - start_item)
                    : items_per_thread;
                init_threads.emplace_back([this, new_dataset, start_item, count]() {
                    initialize_dataset(std::span<std::byte>(new_dataset->data(), new_dataset->size()), *shared_cache_, start_item, count);
                });
            }
            for (auto& t : init_threads) {
                t.join();
            }
            shared_dataset_ = new_dataset;
            std::cout << "[MiningEngine] Dataset initialization complete.\n";
        }
        current_seed_key_ = job.seed_key;
    }

    current_job_ = job;
    job_generation_.fetch_add(1, std::memory_order_release);
    has_job_ = true;
}

std::uint64_t MiningEngine::total_hashes() const {
    return total_hashes_.load(std::memory_order_relaxed);
}

double MiningEngine::worker_hash_rate(unsigned int thread_id) const {
    if (!running_.load() || !worker_hashes_ || thread_id >= num_workers_) {
        return 0.0;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - start_time_).count();
    if (elapsed <= 0.001) return 0.0;
    return static_cast<double>(worker_hashes_[thread_id].load(std::memory_order_relaxed)) / elapsed;
}

double MiningEngine::hash_rate() const {
    if (!running_.load()) {
        return 0.0;
    }
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - start_time_).count();
    if (elapsed <= 0.001) {
        return 0.0;
    }
    return static_cast<double>(total_hashes_.load()) / elapsed;
}

void MiningEngine::worker_loop(unsigned int thread_id) {
    // Pin this worker to a specific CPU core (big cores first on big.LITTLE)
    cpu_set_t cpus{};
    CPU_ZERO(&cpus);
    unsigned int cpu_id = core_order_[thread_id % core_order_.size()];
    CPU_SET(static_cast<int>(cpu_id), &cpus);
    pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);

    if (rt_priority_) {
        struct sched_param param{};
        param.sched_priority = 1;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
            static std::once_flag warn_flag;
            std::call_once(warn_flag, []{
                std::cerr << "[Warning] --rt-priority requires CAP_SYS_NICE; falling back to default scheduler.\n";
            });
        }
    }

    // On AArch64 with crypto extensions, hardware AES is always available
    std::uint32_t flags = kRandOMXFlagHardAes;
    if (mode_ == RandomXMode::fast) flags |= kRandOMXFlagFullMem;
#ifdef ARMRX_HAVE_JIT
    flags |= kRandOMXFlagJit;
#endif
    VirtualMachine vm(flags);

    std::shared_ptr<Argon2dCache> active_cache;
    std::shared_ptr<MappedMemory> active_dataset;
    Job local_job;
    bool active = false;

    std::uint64_t local_hashes = 0;
    std::uint64_t local_gen = 0;
    // Per-worker nonce: each worker gets thread_id + k * num_threads_
    std::uint64_t local_nonce = static_cast<std::uint64_t>(thread_id);
    // Per-worker buffer for block template — resized only on job changes
    std::vector<std::byte> block_input;

    while (running_.load(std::memory_order_relaxed)) {
        // Lock-free job check: only acquire mutex when generation counter changes
        std::uint64_t current_gen = job_generation_.load(std::memory_order_acquire);
        if (current_gen != local_gen) {
            std::lock_guard<std::mutex> lock(job_mutex_);
            local_gen = current_gen;
            if (!has_job_) {
                active = false;
            } else {
                local_job = current_job_;
                active_cache = shared_cache_;
                active_dataset = shared_dataset_;
                active = true;

                vm.set_cache(active_cache.get());
                if (mode_ == RandomXMode::fast && active_dataset) {
                    vm.set_dataset(std::span<const std::byte>(active_dataset->data(), active_dataset->size()));
                }
            }
        }

        if (!active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Partitioned nonce: each worker uses its own counter with stride = num_threads_
        // No shared atomic needed — workers never overlap
        std::uint64_t nonce = local_nonce;
        local_nonce += num_threads_;

        // Copy template to per-worker buffer (only actually reallocates on job change)
        block_input = local_job.block_template;
        update_nonce_in_template(block_input, nonce, local_job.nonce_offset, local_job.nonce_size);

        alignas(16) std::array<std::byte, 32> hash{};
        randomx_calculate_hash(&vm, block_input.data(), block_input.size(), hash.data());
        ++local_hashes;

        if (meets_target(hash, local_job.target)) {
            if (share_callback_) {
                share_callback_(local_job, nonce, hash);
            }
        }

        // Flush local counter to shared atomic periodically
        constexpr std::uint64_t flush_interval = 64U;
        if (local_hashes >= flush_interval) {
            total_hashes_.fetch_add(local_hashes, std::memory_order_relaxed);
            worker_hashes_[thread_id].fetch_add(local_hashes, std::memory_order_relaxed);
            local_hashes = 0;

#ifdef ARMRX_JIT_PROFILE
            total_jit_compile_time_ns_.fetch_add(vm.get_jit_compile_time_ns(), std::memory_order_relaxed);
            total_jit_execute_time_ns_.fetch_add(vm.get_jit_execute_time_ns(), std::memory_order_relaxed);
            total_jit_runs_.fetch_add(vm.get_jit_total_runs(), std::memory_order_relaxed);
            vm.reset_jit_timers();
#endif
        }
    }

    // Flush remaining
    if (local_hashes > 0) {
        total_hashes_.fetch_add(local_hashes, std::memory_order_relaxed);
        worker_hashes_[thread_id].fetch_add(local_hashes, std::memory_order_relaxed);
    }
#ifdef ARMRX_JIT_PROFILE
    total_jit_compile_time_ns_.fetch_add(vm.get_jit_compile_time_ns(), std::memory_order_relaxed);
    total_jit_execute_time_ns_.fetch_add(vm.get_jit_execute_time_ns(), std::memory_order_relaxed);
    total_jit_runs_.fetch_add(vm.get_jit_total_runs(), std::memory_order_relaxed);
#endif
}

void MiningEngine::update_nonce_in_template(std::vector<std::byte>& block, std::uint64_t nonce, std::size_t offset, std::size_t size) {
    if (offset + size > block.size()) {
        return;
    }
    for (std::size_t i = 0; i < size; ++i) {
        block[offset + i] = static_cast<std::byte>((nonce >> (8 * i)) & 0xff);
    }
}

} // namespace armrx
