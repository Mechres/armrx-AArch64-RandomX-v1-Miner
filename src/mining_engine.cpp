#include "armrx/mining_engine.hpp"
#include "armrx/cpu_features.hpp"
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
#include "armrx/log.hpp"

#ifdef ARMRX_HAVE_HWLOC
#include <hwloc.h>
#endif

namespace armrx {

namespace {

#ifdef ARMRX_HAVE_HWLOC

// hwloc-based core ordering: discovers topology and returns core IDs
// ordered by physical package (big.LITTLE clusters first by frequency).
std::vector<unsigned int> detect_core_order() {
    hwloc_topology_t topology;
    hwloc_topology_init(&topology);
    hwloc_topology_load(topology);

    int depth = hwloc_get_type_depth(topology, HWLOC_OBJ_PU);
    if (depth < 0) {
        hwloc_topology_destroy(topology);
        // Fallback
        unsigned int n = online_cpu_count();
        std::vector<unsigned int> fallback(n);
        for (unsigned int i = 0; i < n; ++i) fallback[i] = i;
        return fallback;
    }

    int num_pus = hwloc_get_nbobjs_by_depth(topology, depth);
    if (num_pus <= 0) {
        hwloc_topology_destroy(topology);
        return {0};
    }

    // Collect (freq, os_index) pairs via cpufreq (same as before)
    std::vector<std::pair<unsigned long, unsigned int>> freq_cores;
    for (int i = 0; i < num_pus; ++i) {
        hwloc_obj_t pu = hwloc_get_obj_by_depth(topology, depth, i);
        unsigned int os_idx = static_cast<unsigned int>(pu->os_index);
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(os_idx) + "/cpufreq/cpuinfo_max_freq";
        std::ifstream file(path);
        unsigned long freq = 0;
        if (file >> freq) {
            freq_cores.emplace_back(freq, os_idx);
        }
    }

    hwloc_topology_destroy(topology);

    if (freq_cores.empty()) {
        // Fallback: sequential order
        std::vector<unsigned int> fallback(static_cast<std::size_t>(num_pus));
        for (int i = 0; i < num_pus; ++i) fallback[static_cast<std::size_t>(i)] = static_cast<unsigned int>(i);
        return fallback;
    }

    std::sort(freq_cores.begin(), freq_cores.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<unsigned int> order;
    order.reserve(freq_cores.size());
    for (const auto& fc : freq_cores) order.push_back(fc.second);
    return order;
}

#else // !ARMRX_HAVE_HWLOC

// sysfs-based core ordering (fallback when hwloc is not available).
// Reads cpuinfo_max_freq from sysfs and sorts by frequency descending.
std::vector<unsigned int> detect_core_order() {
    unsigned int num_cpus = online_cpu_count();
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
        std::vector<unsigned int> fallback(num_cpus);
        for (unsigned int i = 0; i < num_cpus; ++i) fallback[i] = i;
        return fallback;
    }

    std::sort(freq_cores.begin(), freq_cores.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<unsigned int> order;
    order.reserve(freq_cores.size());
    for (const auto& fc : freq_cores) order.push_back(fc.second);
    return order;
}

#endif // ARMRX_HAVE_HWLOC

// Counts how many cores at the front of `order` (already sorted by
// cpuinfo_max_freq descending) share the maximum frequency -- the size of
// the "big" cluster on a big.LITTLE-shaped topology. AffinityMode::BigOnly
// previously hardcoded "cores 0-3 are big" instead of using this already-
// detected ordering (audit finding, 2026-07-25) -- wrong on any topology
// where the big cluster isn't cores 0-3.
unsigned int count_top_frequency_cores(const std::vector<unsigned int>& order) {
    if (order.empty()) return 1;

    std::vector<unsigned long> freqs;
    freqs.reserve(order.size());
    unsigned long max_freq = 0;
    for (unsigned int cpu : order) {
        std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/cpuinfo_max_freq";
        std::ifstream file(path);
        unsigned long freq = 0;
        file >> freq;
        freqs.push_back(freq);
        max_freq = std::max(max_freq, freq);
    }

    if (max_freq == 0) {
        // No frequency info available -- can't distinguish clusters, treat
        // everything as one cluster rather than guessing a fixed count.
        return static_cast<unsigned int>(order.size());
    }

    unsigned int count = 0;
    for (unsigned long freq : freqs) {
        if (freq == max_freq) ++count;
    }
    return count > 0 ? count : static_cast<unsigned int>(order.size());
}

} // namespace

MiningEngine::MiningEngine(RandomXMode mode, unsigned int num_threads)
    : mode_(mode), num_threads_(num_threads) {
    core_order_ = detect_core_order();
    big_core_count_ = count_top_frequency_cores(core_order_);
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
    worker_hashes_ = std::make_unique<PaddedCounter[]>(num_threads_);
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
    // Wake any set_job() call blocked waiting on workers to finish a live
    // dataset rebuild — its wait predicate also checks !running_, but a
    // condition_variable only re-checks the predicate when notified.
    dataset_init_cv_.notify_all();
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
        ARMRX_LOG_INFO << "New seed key detected. Initializing cache...";
        auto new_cache = std::make_shared<Argon2dCache>();
        new_cache->initialize(job.seed_key);
        shared_cache_ = new_cache;

        // Start background fill of the partial dataset if configured.
        // Only starts once (checked by partial_dataset_fill_started_ flag).
        if (partial_dataset_ && !partial_dataset_fill_started_.test_and_set(std::memory_order_relaxed)) {
            // Collect mining worker cores to exclude fill threads from them
            std::vector<unsigned> mining_cores;
            for (unsigned i = 0; i < num_threads_; ++i) {
                mining_cores.push_back(core_order_[i % core_order_.size()]);
            }
            // Deduplicate
            std::sort(mining_cores.begin(), mining_cores.end());
            mining_cores.erase(std::unique(mining_cores.begin(), mining_cores.end()), mining_cores.end());
            partial_dataset_->start_fill(*shared_cache_, core_order_, shared_cache_, mining_cores);
        }

        if (mode_ == RandomXMode::fast) {
            auto dataset_bytes = randomx_dataset_item_count() * 64;
            ARMRX_LOG_INFO << "Initializing " << dataset_bytes / (1024U * 1024U) << " MiB dataset...";
            auto new_dataset = std::make_shared<MappedMemory>(dataset_bytes);

            if (running_.load(std::memory_order_acquire) && num_workers_ > 0) {
                // Live seed rotation: reuse the persistent, already
                // affinity-pinned mining workers instead of spawning
                // temporary unpinned threads that would compete with them
                // for the same cores. See worker_loop() for the other half
                // of this handshake.
                {
                    std::lock_guard<std::mutex> init_lock(dataset_init_mutex_);
                    pending_dataset_ = new_dataset;
                    pending_cache_ = shared_cache_;
                    pending_items_per_thread_ = randomx_dataset_item_count() / num_workers_;
                    dataset_init_remaining_ = num_workers_;
                }
                // Bump the generation counter to signal all workers exactly
                // once — no boolean flag to reset, so no race window.
                dataset_init_generation_.fetch_add(1, std::memory_order_release);
                {
                    std::unique_lock<std::mutex> wait_lock(dataset_init_mutex_);
                    dataset_init_cv_.wait(wait_lock, [this] {
                        return dataset_init_remaining_ == 0 || !running_.load(std::memory_order_acquire);
                    });
                }
            } else {
                // No persistent workers exist yet (e.g. the very first job,
                // set before start()) — fall back to temporary threads across
                // all hardware cores, same as before. This is a one-time
                // startup cost with no live workers to reuse anyway.
                unsigned int init_threads_count = online_cpu_count();
                std::vector<std::thread> init_threads;
                std::uint64_t items_per_thread = randomx_dataset_item_count() / init_threads_count;
                for (unsigned int i = 0; i < init_threads_count; ++i) {
                    std::uint64_t start_item = i * items_per_thread;
                    std::uint64_t count = (i == init_threads_count - 1)
                        ? (randomx_dataset_item_count() - start_item)
                        : items_per_thread;
                    init_threads.emplace_back([this, new_dataset, start_item, count]() {
                        // initialize_dataset() writes starting at output[0], not at
                        // output[start_item] — it expects a buffer sized exactly for
                        // `count` items. Passing the full buffer here (pre-existing
                        // bug, found while adding §2.1's dataset-reinit test) made
                        // every thread but the first overwrite the same starting
                        // bytes instead of writing its own region, leaving most of
                        // the dataset uninitialized. Must pass the correct sub-span.
                        initialize_dataset(
                            std::span<std::byte>(new_dataset->data() + start_item * kRandomXDatasetItemBytes,
                                                  count * kRandomXDatasetItemBytes),
                            *shared_cache_, start_item, count);
                    });
                }
                for (auto& t : init_threads) {
                    t.join();
                }
            }
            shared_dataset_ = new_dataset;
            ARMRX_LOG_INFO << "Dataset initialization complete.";
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
    return static_cast<double>(worker_hashes_[thread_id].value.load(std::memory_order_relaxed)) / elapsed;
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

MiningEngine::HashSnapshot MiningEngine::snapshot() const {
    HashSnapshot s;
    s.ts = std::chrono::steady_clock::now();
    if (!worker_hashes_ || num_workers_ == 0) {
        s.total = 0;
        return s;
    }
    s.per_worker.resize(num_workers_);
    std::uint64_t sum = 0;
    for (unsigned i = 0; i < num_workers_; ++i) {
        s.per_worker[i] = worker_hashes_[i].value.load(std::memory_order_relaxed);
        sum += s.per_worker[i];
    }
    s.total = sum;
    return s;
}


void MiningEngine::worker_loop(unsigned int thread_id) {
    if (affinity_mode_ == AffinityMode::BigOnly && !core_order_.empty()) {
        // Pin strictly to the detected big cluster (the highest
        // cpuinfo_max_freq cores in core_order_), not a hardcoded "cores
        // 0-3" assumption -- wrong on any topology where the big cluster
        // isn't cores 0-3 (audit finding, 2026-07-25).
        cpu_set_t cpus{};
        CPU_ZERO(&cpus);
        unsigned int cpu_id = core_order_[thread_id % big_core_count_];
        CPU_SET(static_cast<int>(cpu_id), &cpus);
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    } else if (affinity_mode_ == AffinityMode::All) {
        // Pin to all cores sequentially
        cpu_set_t cpus{};
        CPU_ZERO(&cpus);
        unsigned int cpu_id = core_order_[thread_id % core_order_.size()];
        CPU_SET(static_cast<int>(cpu_id), &cpus);
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    }
    // If AffinityMode::Unpinned, skip pinning and let the OS handle scheduling

    if (rt_priority_) {
        struct sched_param param{};
        param.sched_priority = 1;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
            static std::once_flag warn_flag;
            std::call_once(warn_flag, []{
                ARMRX_LOG_WARN << "--rt-priority requires CAP_SYS_NICE; falling back to default scheduler.";
            });
        }
    }

    // Optional startup stagger: delay each worker thread by thread_id * stagger_ms
    // to desynchronize memory-intensive phases across cores.
    if (stagger_ms_ > 0 && thread_id > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<uint64_t>(thread_id) * stagger_ms_));
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
    std::uint64_t local_dataset_init_gen = 0;
    // Per-worker nonce: each worker gets thread_id + k * num_threads_
    std::uint64_t local_nonce = static_cast<std::uint64_t>(thread_id);
    // Per-worker buffer for block template — resized only on job changes
    std::vector<std::byte> block_input;

    while (running_.load(std::memory_order_relaxed)) {
        // Live dataset (re)initialization: participate directly instead of
        // letting set_job() spawn temporary threads. Checked first, ahead of
        // the active/idle branch below, so idle workers pick this up within
        // their existing sleep granularity and busy workers pick it up on
        // their next loop iteration (once per hash). Generation-counter
        // compare, not a boolean flag: guarantees this worker participates
        // exactly once per set_job() dataset rebuild.
        std::uint64_t current_dataset_init_gen = dataset_init_generation_.load(std::memory_order_acquire);
        if (current_dataset_init_gen != local_dataset_init_gen) {
            local_dataset_init_gen = current_dataset_init_gen;

            std::shared_ptr<MappedMemory> target_dataset;
            std::shared_ptr<Argon2dCache> target_cache;
            std::uint64_t start_item = 0;
            std::uint64_t count = 0;
            {
                std::lock_guard<std::mutex> lock(dataset_init_mutex_);
                target_dataset = pending_dataset_;
                target_cache = pending_cache_;
                start_item = static_cast<std::uint64_t>(thread_id) * pending_items_per_thread_;
                count = (thread_id == num_threads_ - 1)
                    ? (randomx_dataset_item_count() - start_item)
                    : pending_items_per_thread_;
            }
            // See the matching comment in set_job()'s temp-thread fallback:
            // initialize_dataset() writes relative to output[0], so this must
            // be the sub-span at this worker's own item range, not the full
            // dataset buffer.
            initialize_dataset(
                std::span<std::byte>(target_dataset->data() + start_item * kRandomXDatasetItemBytes,
                                      count * kRandomXDatasetItemBytes),
                *target_cache, start_item, count);
            {
                std::lock_guard<std::mutex> lock(dataset_init_mutex_);
                if (--dataset_init_remaining_ == 0) {
                    dataset_init_cv_.notify_all();
                }
            }
            continue;
        }

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

                // Copy block template to per-worker buffer (only on job change)
                block_input = local_job.block_template;

                vm.set_cache(active_cache.get());
                // Hybrid partial dataset: pass to VM if configured.
                // Always pass even if current item_count_ is 0 — the JIT
                // hybrid entry handles 0 items via cbz (always takes miss
                // path), and we want the pointer to be non-null so run_jit()
                // enables the hybrid code emission. item_count_ rises
                // atomically as fill progresses.
                if (partial_dataset_) {
                    vm.set_partial_dataset(partial_dataset_->data(),
                                           partial_dataset_->item_count_atomic());
                }
                if (mode_ == RandomXMode::fast && active_dataset) {
                    if (!vm.set_dataset(std::span<const std::byte>(active_dataset->data(), active_dataset->size()))) {
                        std::fprintf(stderr, "[worker %u] dataset size mismatch, skipping job\n", thread_id);
                        active = false;
                    }
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
        if (!update_nonce_in_template(block_input, nonce, local_job.nonce_offset, local_job.nonce_size)) {
            ARMRX_LOG_ERROR << "Worker " << thread_id << ": bad nonce offset=" << local_job.nonce_offset
                      << " size=" << local_job.nonce_size << " in job, deactivating";
            // Deactivate and go back to the top of the loop (matching every
            // other bad-state path above, e.g. the dataset-size-mismatch
            // case) rather than returning — a `return` here would exit
            // worker_loop() entirely and permanently kill this thread for
            // the rest of the process's life over a single bad job, instead
            // of just idling until job_generation_ advances to a new
            // (hopefully valid) job.
            active = false;
            continue;
        }

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
            worker_hashes_[thread_id].value.fetch_add(local_hashes, std::memory_order_relaxed);
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
        worker_hashes_[thread_id].value.fetch_add(local_hashes, std::memory_order_relaxed);
    }
#ifdef ARMRX_JIT_PROFILE
    total_jit_compile_time_ns_.fetch_add(vm.get_jit_compile_time_ns(), std::memory_order_relaxed);
    total_jit_execute_time_ns_.fetch_add(vm.get_jit_execute_time_ns(), std::memory_order_relaxed);
    total_jit_runs_.fetch_add(vm.get_jit_total_runs(), std::memory_order_relaxed);
#endif
}

bool MiningEngine::update_nonce_in_template(std::vector<std::byte>& block, std::uint64_t nonce, std::size_t offset, std::size_t size) {
    if (offset + size > block.size()) {
        return false;
    }
    for (std::size_t i = 0; i < size; ++i) {
        block[offset + i] = static_cast<std::byte>((nonce >> (8 * i)) & 0xff);
    }
    return true;
}

} // namespace armrx
