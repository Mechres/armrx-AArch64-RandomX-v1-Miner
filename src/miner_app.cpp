#include "armrx/miner_app.hpp"

#include "armrx/argon2.hpp"
#include "armrx/cpu_features.hpp"
#include "armrx/cpu_thermal.hpp"
#include "armrx/memory.hpp"
#include "armrx/mining_engine.hpp"
#include "armrx/stratum_client.hpp"
#include "armrx/tui.hpp"
#include "armrx/pool_manager.hpp"
#include "armrx/metrics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <memory>
#include <thread>
#include <csignal>
#include <signal.h>
#include <sys/mman.h>
#include <cstdlib>

namespace armrx {

namespace {

std::atomic<bool> keep_running{true};
void signal_handler(int) {
    keep_running = false;
}

armrx::Target difficulty_to_target(std::uint64_t diff) {
    armrx::Target target;
    if (diff == 0) {
        diff = 1;
    }
    std::uint64_t remainder = 0;
    for (int i = 31; i >= 0; --i) {
        std::uint64_t val = (remainder << 8) | 0xff;
        target.bytes[static_cast<std::size_t>(i)] = static_cast<std::byte>(val / diff);
        remainder = val % diff;
    }
    return target;
}

std::string hash_to_hex(const std::array<std::byte, 32>& hash) {
    std::string res;
    res.reserve(64);
    for (auto b : hash) {
        static const char hex_chars[] = "0123456789abcdef";
        res.push_back(hex_chars[(static_cast<std::uint8_t>(b) >> 4) & 0xf]);
        res.push_back(hex_chars[static_cast<std::uint8_t>(b) & 0xf]);
    }
    return res;
}

} // namespace

MinerApp::MinerApp(MinerOptions options) : opts_(std::move(options)) {}

void MinerApp::install_signal_handlers() {
    // Use sigaction (not std::signal) so SA_RESTART is explicitly cleared.
    // With SA_RESTART=0, blocking syscalls in the run loops (sleep_for's
    // nanosleep, the MetricsExporter accept()) are interrupted by the signal
    // instead of auto-restarting, so the keep_running loop exits promptly on
    // Ctrl-C. A plain SIGINT handler is async-signal-safe (only an atomic
    // store), so it is safe to run inside the signal context.
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;  // SA_RESTART intentionally NOT set
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
#ifdef SIGPIPE
    ::signal(SIGPIPE, SIG_IGN);
#endif
}

void MinerApp::run_init_cache() {
    std::vector<std::byte> key_bytes;
    key_bytes.reserve(opts_.init_cache_key.size());
    for (const auto character : opts_.init_cache_key) {
        key_bytes.push_back(static_cast<std::byte>(character));
    }
    std::cout << "Initializing 256 MiB Argon2d cache...\n";
    const auto started = std::chrono::steady_clock::now();
    armrx::Argon2dCache cache;
    cache.initialize(key_bytes);
    const auto elapsed = std::chrono::duration<double>{std::chrono::steady_clock::now() - started};
    std::cout << "Cache initialized in " << elapsed.count() << " seconds.\n";
}

#ifdef ARMRX_HAVE_JIT
int MinerApp::run_jit_dump() {
    std::vector<std::byte> key_bytes;
    key_bytes.reserve(opts_.jit_dump_key.size());
    for (const auto c : opts_.jit_dump_key) {
        key_bytes.push_back(static_cast<std::byte>(c));
    }

    // Light mode VM with JIT enabled
    const uint32_t vm_flags = armrx::kRandOMXFlagJit | armrx::kRandOMXFlagHardAes;
    armrx::VirtualMachine vm(vm_flags);
    vm.setJitDumpEnabled();

    armrx::Argon2dCache cache;
    cache.initialize(key_bytes);
    vm.set_cache(&cache);

    // Run one hash with the dump enabled
    alignas(16) std::array<std::byte, 32> hash{};
    const char* input = "JIT dump test input";
    armrx::randomx_calculate_hash(&vm, input, std::strlen(input), hash.data());

    // Dump the JIT code with boundary markers
    vm.dumpJitCode();

    std::cout << "\nHash: ";
    for (auto b : hash) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(b);
    }
    std::cout << std::dec << '\n';
    return 0;
}
#endif

void MinerApp::run_local_benchmark(RandomXMode effective_mode) {
    std::cout << "\nStarting miner benchmark (Target difficulty: " << opts_.difficulty << ")\n";
    armrx::Job job;
    job.job_id = "local_benchmark_job";
    job.block_template.resize(76, std::byte{0});
    std::string seed = "test key 000";
    job.seed_key.reserve(seed.size());
    for (char c : seed) job.seed_key.push_back(static_cast<std::byte>(c));
    job.nonce_offset = 39;
    job.nonce_size   = 4;
    job.target       = difficulty_to_target(opts_.difficulty);

    armrx::MiningEngine engine(effective_mode, opts_.workers);
    engine.set_affinity_mode(opts_.affinity_mode);
    engine.set_rt_priority(opts_.use_rt_priority);
    engine.set_stagger_ms(opts_.stagger_ms);
    if (partial_dataset_) {
        engine.set_partial_dataset(partial_dataset_.get());
    }
    engine.set_job(job);

    std::atomic<std::uint64_t> shares_found{0};
    auto share_callback = [&shares_found](const armrx::Job& j, std::uint64_t nonce,
                                           std::array<std::byte, 32> hash) {
        shares_found.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[Mining] Valid share found! Job: " << j.job_id
                  << " | Nonce: " << std::hex << nonce
                  << " | Hash: " << hash_to_hex(hash) << std::dec << std::endl;
    };

    engine.start(share_callback);
    std::cout << "Mining started. Press Ctrl+C to stop.\n";

    // Optional Prometheus metrics endpoint for benchmark mode
    std::unique_ptr<armrx::MetricsExporter> bench_metrics;
    if (opts_.metrics_port > 0) {
        bench_metrics = std::make_unique<armrx::MetricsExporter>(opts_.metrics_port,
            [&engine, &shares_found]() -> std::string {
                std::string out;
                out += "# HELP armrx_hashrate_total Current total hashrate H/s\n"
                       "# TYPE armrx_hashrate_total gauge\n"
                       "armrx_hashrate_total " +
                       std::to_string(engine.hash_rate()) + "\n";
                out += "# HELP armrx_hashes_total Total hashes computed\n"
                       "# TYPE armrx_hashes_total counter\n"
                       "armrx_hashes_total " +
                       std::to_string(engine.total_hashes()) + "\n";
                out += "# HELP armrx_shares_found Shares found (local)\n"
                       "# TYPE armrx_shares_found counter\n"
                       "armrx_shares_found " +
                       std::to_string(shares_found.load()) + "\n";
                if (const auto zones = armrx::read_cpu_temperatures(); !zones.empty()) {
                    out += "# HELP armrx_cpu_temp_celsius CPU thermal zone temperature\n"
                           "# TYPE armrx_cpu_temp_celsius gauge\n";
                    for (const auto& z : zones) {
                        out += "armrx_cpu_temp_celsius{zone=\"" + z.name + "\"} " +
                               std::to_string(z.temp_c) + "\n";
                    }
                }
                out += "# EOF\n";
                return out;
            });
    }

    auto start_time      = std::chrono::steady_clock::now();
    unsigned elapsed_sec = 0;
    // Snapshot taken after warmup to measure steady-state rate
    armrx::MiningEngine::HashSnapshot snap_warmup;
    bool snap_taken = false;
    const unsigned effective_warmup = (opts_.runtime_seconds > 0 && opts_.warmup_secs >= opts_.runtime_seconds)
                                       ? opts_.runtime_seconds / 2
                                       : opts_.warmup_secs;

    while (keep_running && (opts_.runtime_seconds == 0 || elapsed_sec < opts_.runtime_seconds)) {
        // Interruptible 1s cadence: sleep in short 100ms slices that re-check
        // keep_running, so a SIGINT (which only sets the flag) is observed
        // promptly. std::this_thread::sleep_for swallows EINTR and re-sleeps,
        // so a single 1s sleep would ignore the signal until it elapsed.
        for (int i = 0; i < 10 && keep_running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        elapsed_sec = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count());

        // Take post-warmup snapshot once
        if (!snap_taken && elapsed_sec >= effective_warmup) {
            snap_warmup = engine.snapshot();
            snap_taken = true;
        }

        const double speed         = engine.hash_rate();
        const std::uint64_t total  = engine.total_hashes();
        const std::uint64_t shares = shares_found.load();

        // TUI not supported in benchmark mode
        std::cout << "[Mining] Speed: " << std::fixed << std::setprecision(2) << speed << " H/s"
                  << " | Shares: " << shares
                  << " | Total Hashes: " << total
                  << " | Time: " << elapsed_sec << "s";
        if (const double temp_c = armrx::max_cpu_temperature(); temp_c >= 0.0) {
            std::cout << " | CPU: " << std::fixed << std::setprecision(1) << temp_c << "C";
        }
        std::cout << "\r" << std::flush;
    }
    std::cout << std::endl;

    // Take end snapshot for steady-state computation
    const auto snap_end = engine.snapshot();
    engine.stop();

    // If the partial dataset fill completed before shutdown, wait for fill
    // threads to finish so the destructor can unmap cleanly (avoids detached
    // thread SIGSEGV during process exit). If fill is still in progress,
    // skip the wait and let the OS reclaim the mapping — the process is
    // exiting anyway.
    if (partial_dataset_ && partial_dataset_->item_count() >= 8388608 /* 512 MiB items */) {
        partial_dataset_->wait_for_fill();
    }

    // Compute steady-state rates from snapshot delta
    double steady_total = 0.0;
    std::vector<double> steady_per_worker;
    if (snap_taken) {
        const double delta_secs = std::chrono::duration<double>(snap_end.ts - snap_warmup.ts).count();
        if (delta_secs > 0.5) {
            const std::uint64_t delta_total = (snap_end.total >= snap_warmup.total)
                ? snap_end.total - snap_warmup.total : 0;
            steady_total = static_cast<double>(delta_total) / delta_secs;
            const unsigned nw = engine.num_workers();
            steady_per_worker.resize(nw);
            for (unsigned i = 0; i < nw; ++i) {
                const std::uint64_t dw = (i < snap_end.per_worker.size() &&
                                          i < snap_warmup.per_worker.size() &&
                                          snap_end.per_worker[i] >= snap_warmup.per_worker[i])
                    ? snap_end.per_worker[i] - snap_warmup.per_worker[i] : 0;
                steady_per_worker[i] = static_cast<double>(dw) / delta_secs;
            }
        }
    }

    std::cout << "Mining benchmark complete.\n"
              << "Total Hashes computed: " << engine.total_hashes() << "\n"
              << "Final Shares found: "    << shares_found.load()    << "\n";
    if (snap_taken && steady_total > 0.0) {
        std::cout << std::fixed << std::setprecision(2)
                  << "Steady-state hashrate: " << steady_total << " H/s"
                  << " (measured over " << static_cast<unsigned>(
                         std::chrono::duration<double>(snap_end.ts - snap_warmup.ts).count())
                  << "s post-warmup)\n";
        for (unsigned i = 0; i < steady_per_worker.size(); ++i) {
            std::cout << "  worker[" << i << "]: " << steady_per_worker[i] << " H/s\n";
        }
    }
#ifdef ARMRX_JIT_PROFILE
    std::uint64_t total_compile = engine.total_jit_compile_time_ns();
    std::uint64_t total_execute = engine.total_jit_execute_time_ns();
    if (total_compile + total_execute > 0) {
        double total_time = static_cast<double>(total_compile + total_execute);
        double compile_pct = (static_cast<double>(total_compile) / total_time) * 100.0;
        double execute_pct = (static_cast<double>(total_execute) / total_time) * 100.0;
        std::cout << "JIT Profile: Compile = " << std::fixed << std::setprecision(1) << compile_pct << "%"
                  << ", Exec = " << execute_pct << "%\n";
    }
#endif
}

void MinerApp::run_pool_mining(RandomXMode effective_mode) {
    // Build PoolConfig vector from the flat pool_list
    std::vector<armrx::PoolConfig> pool_configs;
    for (const auto& p : opts_.pool_list) {
        pool_configs.push_back({p.first, p.second, opts_.pool_tls});
    }

    std::cout << "\nStarting pool miner — " << pool_configs.size()
              << " pool(s) configured\n";

    armrx::MiningEngine engine(effective_mode, opts_.workers);
    engine.set_affinity_mode(opts_.affinity_mode);
    engine.set_rt_priority(opts_.use_rt_priority);
    engine.set_stagger_ms(opts_.stagger_ms);
    if (partial_dataset_) {
        engine.set_partial_dataset(partial_dataset_.get());
    }

    std::atomic<std::uint64_t> shares_submitted{0};

    auto pool_mgr = std::make_unique<armrx::PoolManager>(
        pool_configs, opts_.pool_wallet, opts_.pool_password, opts_.pool_tls, opts_.pool_tls_verify);

    // Optional Prometheus metrics endpoint
    std::unique_ptr<armrx::MetricsExporter> metrics;
    if (opts_.metrics_port > 0) {
        metrics = std::make_unique<armrx::MetricsExporter>(opts_.metrics_port,
            [&engine, &pool_mgr, &shares_submitted]() -> std::string {
                std::string out;
                // Hashrate
                out += "# HELP armrx_hashrate_total Current total hashrate H/s\n"
                       "# TYPE armrx_hashrate_total gauge\n"
                       "armrx_hashrate_total " +
                       std::to_string(engine.hash_rate()) + "\n";
                // Total hashes
                out += "# HELP armrx_hashes_total Total hashes computed\n"
                       "# TYPE armrx_hashes_total counter\n"
                       "armrx_hashes_total " +
                       std::to_string(engine.total_hashes()) + "\n";
                // Shares
                out += "# HELP armrx_shares_total Shares submitted\n"
                       "# TYPE armrx_shares_total counter\n"
                       "armrx_shares_submitted " +
                       std::to_string(shares_submitted.load()) + "\n"
                       "armrx_shares_accepted " +
                       std::to_string(pool_mgr->shares_accepted()) + "\n"
                       "armrx_shares_rejected " +
                       std::to_string(pool_mgr->shares_rejected()) + "\n";
                // Pool status
                out += "# HELP armrx_pool_connected Pool connection status\n"
                       "# TYPE armrx_pool_connected gauge\n"
                       "armrx_pool_connected " +
                       std::string(pool_mgr->is_connected() ? "1" : "0") + "\n";
                // JIT profile (optional)
#ifdef ARMRX_JIT_PROFILE
                auto tc = engine.total_jit_compile_time_ns();
                auto te = engine.total_jit_execute_time_ns();
                out += "# HELP armrx_jit_compile_seconds_total JIT compile time\n"
                       "# TYPE armrx_jit_compile_seconds_total counter\n"
                       "armrx_jit_compile_seconds_total " +
                       std::to_string(static_cast<double>(tc) / 1e9) + "\n"
                       "# HELP armrx_jit_execute_seconds_total JIT execute time\n"
                       "# TYPE armrx_jit_execute_seconds_total counter\n"
                       "armrx_jit_execute_seconds_total " +
                       std::to_string(static_cast<double>(te) / 1e9) + "\n";
#endif
                if (const auto zones = armrx::read_cpu_temperatures(); !zones.empty()) {
                    out += "# HELP armrx_cpu_temp_celsius CPU thermal zone temperature\n"
                           "# TYPE armrx_cpu_temp_celsius gauge\n";
                    for (const auto& z : zones) {
                        out += "armrx_cpu_temp_celsius{zone=\"" + z.name + "\"} " +
                               std::to_string(z.temp_c) + "\n";
                    }
                }
                out += "# EOF\n";
                return out;
            });
    }

    pool_mgr->set_job_callback([&](const armrx::Job& job) {
        engine.set_job(job);
    });

    pool_mgr->set_error_callback([&](const std::string& reason) {
        std::cerr << "[Pool] " << pool_mgr->current_pool_name()
                  << ": " << reason << '\n';
    });

    auto share_callback = [&](const armrx::Job& job, std::uint64_t nonce,
                              std::array<std::byte, 32> hash) {
        shares_submitted.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[Pool] Share found! Nonce: " << std::hex << nonce
                  << " Hash: " << hash_to_hex(hash) << std::dec << '\n';
        pool_mgr->submit_share(job, nonce, hash);
    };

    engine.start(share_callback);
    pool_mgr->connect();

    // Take the pool-test start snapshot now (before the loop) so the end
    // snapshot delta covers the whole run.
    if (opts_.pool_test) {
        pool_test_snap_start_ = engine.snapshot();
    }

    // Optional TUI dashboard
    std::unique_ptr<armrx::Tui> tui;
    if (opts_.use_tui) {
        bool color = (opts_.tui_color >= 0) ? static_cast<bool>(opts_.tui_color) : armrx::Tui::detect_color();
        tui = std::make_unique<armrx::Tui>(color);
        // Route worker-thread ARMRX_LOG_* to the ring buffer (off stdout) so
        // they cannot interleave with Tui::render()'s escape sequences (Bug 2:
        // "armrx" header + raw control bytes, overlapping lines). Without this,
        // the ring buffer is dead and logs race render() on std::cout.
        armrx::log::set_tui_mode(true);
    }

    if (!opts_.use_tui) {
        std::cout << "Pool mining started. Press Ctrl+C to stop.\n";
    }

    unsigned elapsed_sec = 0;

    while (keep_running && (!opts_.pool_test || opts_.runtime_seconds == 0 || elapsed_sec < opts_.runtime_seconds)) {
        // Interruptible 1s cadence (see run_local_benchmark for rationale):
        // sleep in 100ms slices that re-check keep_running so SIGINT is seen
        // promptly instead of being swallowed by sleep_for's EINTR restart.
        for (int i = 0; i < 10 && keep_running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ++elapsed_sec;

        // Pool failover handled internally by PoolManager
        pool_mgr->tick();

        const double speed  = engine.hash_rate();
        const auto total    = engine.total_hashes();
        const auto shares   = shares_submitted.load();
        const bool online   = pool_mgr->is_connected();
        const auto retries  = pool_mgr->reconnect_attempts();

        double compile_pct = -1.0;
        double execute_pct = -1.0;
        std::uint64_t total_compile = engine.total_jit_compile_time_ns();
        std::uint64_t total_execute = engine.total_jit_execute_time_ns();
        if (total_compile + total_execute > 0) {
            double total_time = static_cast<double>(total_compile + total_execute);
            compile_pct = (static_cast<double>(total_compile) / total_time) * 100.0;
            execute_pct = (static_cast<double>(total_execute) / total_time) * 100.0;
        }

        if (tui) {
            armrx::TuiSnapshot snap;
            snap.pool_name = pool_mgr->current_pool_name();
            snap.mode = armrx::mode_name(effective_mode);
            snap.status = online ? armrx::TuiSnapshot::Status::mining
                : (retries > 0 ? armrx::TuiSnapshot::Status::reconnecting
                               : armrx::TuiSnapshot::Status::disconnected);
            snap.uptime_sec = elapsed_sec;
            snap.reconnect_attempts = retries;
            snap.total_hash_rate = speed;
            snap.total_hashes = total;
            snap.shares_submitted = shares;
            snap.shares_accepted = pool_mgr->shares_accepted();
            snap.shares_rejected = pool_mgr->shares_rejected();
            snap.jit_compile_pct = compile_pct;
            snap.jit_execute_pct = execute_pct;
            snap.max_cpu_temp_c = armrx::max_cpu_temperature();
            // Collect per-worker rates
            std::vector<double> worker_rates;
            for (unsigned w = 0; w < opts_.workers; ++w)
                worker_rates.push_back(engine.worker_hash_rate(w));
            snap.worker_rates = worker_rates;
            tui->render(snap);
        } else {
            std::cout << "[Pool] " << pool_mgr->current_pool_name()
                      << " Speed: " << std::fixed << std::setprecision(2) << speed << " H/s"
                      << " | Shares: " << shares
                      << " | Total: "     << total
                      << " | Uptime: "    << elapsed_sec << "s";
            if (compile_pct >= 0.0 && execute_pct >= 0.0) {
                std::cout << " | JIT Compile: " << std::fixed << std::setprecision(1) << compile_pct << "%"
                          << " Exec: " << execute_pct << "%";
            }
            if (const double temp_c = armrx::max_cpu_temperature(); temp_c >= 0.0) {
                std::cout << " | CPU: " << std::fixed << std::setprecision(1) << temp_c << "C";
            }
            if (!online) {
                std::cout << " | ";
                if (retries > 0) {
                    std::cout << "\033[33mReconnecting (attempt " << retries << ")...\033[0m";
                } else {
                    std::cout << "\033[33mDisconnected\033[0m";
                }
            }
            std::cout << "\r" << std::flush;
        }
    }
    std::cout << std::endl;

    // TUI mode is ending: restore normal logging (logs go back to stdout)
    // before the post-loop summary / teardown prints below.
    if (tui) armrx::log::set_tui_mode(false);

    // Pool-test mode: print a per-worker steady-state summary (same shape as
    // the --mine benchmark) BEFORE teardown, so the data is captured even if
    // the pool/engine teardown hangs. Uses the full-run snapshot delta.
    if (opts_.pool_test) {
        const auto snap_end = engine.snapshot();
        const double delta_secs =
            std::chrono::duration<double>(snap_end.ts - pool_test_snap_start_.ts).count();
        std::cout << "\n[Pool-test] summary over " << std::fixed << std::setprecision(1)
                  << delta_secs << "s:\n";
        if (delta_secs > 0.5 && snap_end.total >= pool_test_snap_start_.total) {
            const std::uint64_t delta_total = snap_end.total - pool_test_snap_start_.total;
            const double total_rate = static_cast<double>(delta_total) / delta_secs;
            std::cout << "  Total: " << std::fixed << std::setprecision(2) << total_rate << " H/s\n";
            const unsigned nw = engine.num_workers();
            for (unsigned i = 0; i < nw; ++i) {
                const std::uint64_t sw = (i < snap_end.per_worker.size() &&
                                          i < pool_test_snap_start_.per_worker.size() &&
                                          snap_end.per_worker[i] >= pool_test_snap_start_.per_worker[i])
                    ? snap_end.per_worker[i] - pool_test_snap_start_.per_worker[i] : 0;
                std::cout << "  worker[" << i << "]: "
                          << std::fixed << std::setprecision(2)
                          << (static_cast<double>(sw) / delta_secs) << " H/s\n";
            }
        } else {
            std::cout << "  (insufficient run time for steady-state delta)\n";
        }
        if (const double temp_c = armrx::max_cpu_temperature(); temp_c >= 0.0) {
            std::cout << "  CPU max temp: " << std::fixed << std::setprecision(1) << temp_c << "C\n";
        }
        std::cout << std::flush;
        // Restore the terminal (clear TUI frame + show cursor) before
        // self-terminating, otherwise --pool-test --tui leaves the cursor
        // hidden and the dashboard on screen (audit: "skips cursor restore").
        if (tui) tui->shutdown();
        // Self-terminate immediately after capturing the summary. Skipping the
        // pool/engine teardown on purpose: pool_mgr->disconnect()/engine.stop()
        // can block waiting on the network/worker threads, which would prevent
        // clean exit and force a kill -9. For a measurement mode the data above
        // is what matters; leaking the socket on exit is acceptable here.
        std::_Exit(0);
    }

    // Stop the workers FIRST so no worker can enter submit_share() (which takes
    // stratum_mutex_) during teardown — otherwise disconnect() deadlocks waiting
    // on the mutex held by a worker blocked in send(). The SO_SNDTIMEO set on the
    // socket already bounds that send, but stopping workers first makes teardown
    // deterministic regardless of peer state.
    engine.stop();
    pool_mgr->disconnect();

    std::cout << "Pool mining stopped.\n"
              << "Total hashes:     " << engine.total_hashes()     << "\n"
              << "Shares submitted: " << shares_submitted.load()    << "\n";
}

int MinerApp::run() {
    install_signal_handlers();

    const auto cpu    = armrx::detect_cpu_features();
    const auto memory = armrx::available_memory();

    const auto automatic      = armrx::choose_randomx_mode(memory.available_bytes, opts_.workers);
    const auto effective_mode = opts_.mode_is_auto ? automatic.mode : opts_.requested_mode;
    const auto required_bytes = opts_.mode_is_auto
        ? automatic.required_bytes
        : armrx::randomx_shared_memory(opts_.requested_mode) + opts_.workers * armrx::randomx_worker_memory()
              + armrx::kAutoModeSafetyReserve;

    std::cout << "armrx " << (cpu.aarch64 ? "AArch64" : "non-AArch64") << '\n'
              << "AES: "  << (cpu.aes  ? "available" : "unavailable") << '\n'
              << "CRC32: "<< (cpu.crc32 ? "available" : "unavailable") << '\n'
              << "RandomX light shared memory: "
              << armrx::randomx_shared_memory(armrx::RandomXMode::light) / (1024U * 1024U)
              << " MiB\n"
              << "RandomX fast shared memory: "
              << armrx::randomx_shared_memory(armrx::RandomXMode::fast) / (1024U * 1024U)
              << " MiB\n"
              << "Available memory: " << memory.available_bytes / (1024U * 1024U) << " MiB"
              << (memory.constrained_by_cgroup ? " (cgroup-limited)" : "") << '\n'
              << "Selected mode (" << opts_.workers << " workers): " << armrx::mode_name(effective_mode)
              << " (requires " << required_bytes / (1024U * 1024U) << " MiB including reserve)\n";

    // Create partial dataset if configured (Track B hybrid light mode)
    if (opts_.dataset_mb > 0 && effective_mode == RandomXMode::light) {
        const std::size_t item_count = (opts_.dataset_mb * 1024ULL * 1024ULL) / kRandomXDatasetItemBytes;
        partial_dataset_ = std::make_shared<PartialDataset>(item_count);
        std::cout << "Partial dataset: " << item_count << " items ("
                  << opts_.dataset_mb << " MiB)\n";
    }

    // Lock all pages into RAM if requested
    if (opts_.use_mlock) {
        if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            std::cerr << "[Warning] --mlock requires elevated privileges; continuing without locking.\n";
            opts_.use_mlock = false;
        }
    }

    if (!opts_.mode_is_auto && effective_mode == armrx::RandomXMode::fast
        && memory.available_bytes < required_bytes) {
        std::cerr << "Requested fast mode does not fit in available memory.\n";
        return 2;
    }

    if (opts_.should_init_cache) {
        run_init_cache();
    }

#ifdef ARMRX_HAVE_JIT
    if (opts_.jit_dump_mode) {
        return run_jit_dump();
    }
#endif

    if (opts_.should_mine) {
        run_local_benchmark(effective_mode);
    }

    if (opts_.should_connect_pool) {
        if (opts_.pool_wallet.empty()) {
            std::cerr << "Pool mining requires --wallet=<address>\n";
            return 64;
        }
        if (opts_.pool_list.empty()) {
            std::cerr << "Pool mining requires --pool=<host>[:port]\n";
            return 64;
        }
        run_pool_mining(effective_mode);
    }

    return cpu.aarch64 ? 0 : 2;
}

} // namespace armrx
