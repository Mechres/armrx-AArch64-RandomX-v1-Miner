#include "armrx/argon2.hpp"
#include "armrx/cpu_features.hpp"
#include "armrx/memory.hpp"
#include "armrx/randomx_config.hpp"
#include "armrx/mining_engine.hpp"
#include "armrx/stratum_client.hpp"
#include "armrx/config.hpp"
#include "armrx/tui.hpp"
#include "armrx/pool_manager.hpp"
#include "armrx/log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <csignal>
#include <iomanip>
#include <memory>
#include <sys/mman.h>

namespace {

std::atomic<bool> keep_running{true};
bool jit_dump_mode = false;
std::string jit_dump_key = "test key 000";
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

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const auto cpu    = armrx::detect_cpu_features();
    const auto memory = armrx::available_memory();
    auto workers      = std::max(1U, std::thread::hardware_concurrency());
    bool mode_is_auto = true;
    armrx::RandomXMode requested_mode = armrx::RandomXMode::light;
    bool should_init_cache = false;
    std::string init_cache_key;

    bool should_mine = false;
    std::uint64_t difficulty = 100;
    unsigned int runtime_seconds = 10;

    // Pool / stratum options
    bool should_connect_pool = false;
    std::vector<std::pair<std::string, std::uint16_t>> pool_list;
    std::string pool_wallet;
    std::string pool_password = "x";
    bool pool_tls = false;
    bool pool_tls_verify = true;
    bool use_tui = false;
    int tui_color = -1;  // -1 = auto-detect, 0 = no-color, 1 = color
    bool use_mlock = false;
    bool use_rt_priority = false;
    unsigned stagger_ms = 0;
    unsigned int current_pool_idx = 0;

    // Load config from file (CLI overrides below)
    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a.rfind("--config=", 0) == 0) {
            config_path = std::string{a.substr(9)};
            break;
        }
    }
    {
        auto cfg = armrx::load_config_with_fallback(config_path);
        for (auto& p : cfg.pools) pool_list.push_back({p.host, p.port});
        if (!cfg.wallet.empty()) pool_wallet = cfg.wallet;
        if (cfg.password != "x") pool_password = cfg.password;
        pool_tls = cfg.pool_tls;
        if (cfg.workers > 0) workers = cfg.workers;
        if (cfg.mode != "auto") {
            mode_is_auto = false;
            if (cfg.mode == "light") requested_mode = armrx::RandomXMode::light;
            else if (cfg.mode == "fast") requested_mode = armrx::RandomXMode::fast;
        }
        difficulty = cfg.difficulty;
        runtime_seconds = cfg.seconds;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        // Debug: uncomment to see parsed args
        // std::cerr << "[DEBUG] arg[" << i << "] = \"" << argument << "\"\n";
        if (argument.rfind("--mode=", 0) == 0) {
            const auto mode_text = argument.substr(7);
            if (mode_text == "auto") {
                mode_is_auto = true;
            } else if (mode_text == "light") {
                requested_mode = armrx::RandomXMode::light;
                mode_is_auto = false;
            } else if (mode_text == "fast") {
                requested_mode = armrx::RandomXMode::fast;
                mode_is_auto = false;
            } else {
                std::cerr << "Invalid mode: " << mode_text << '\n';
                return 64;
            }
            continue;
        }

        if (argument.rfind("--workers=", 0) == 0) {
            try {
                workers = std::max(1U, static_cast<unsigned>(std::stoul(std::string{argument.substr(10)})));
            } catch (...) {
                std::cerr << "Invalid --workers value: " << argument.substr(10) << '\n';
                return 64;
            }
            continue;
        }

        if (argument == "--init-cache") {
            if (i + 1 >= argc) {
                std::cerr << "--init-cache requires a key\n";
                return 64;
            }
            should_init_cache = true;
            init_cache_key = argv[++i];
            continue;
        }

        if (argument == "--mine") {
            should_mine = true;
            continue;
        }

        if (argument.rfind("--difficulty=", 0) == 0) {
            try {
                difficulty = std::stoull(std::string{argument.substr(13)});
            } catch (...) {
                std::cerr << "Invalid --difficulty value: " << argument.substr(13) << '\n';
                return 64;
            }
            continue;
        }

        if (argument.rfind("--seconds=", 0) == 0) {
            try {
                runtime_seconds = static_cast<unsigned>(std::stoul(std::string{argument.substr(10)}));
            } catch (...) {
                std::cerr << "Invalid --seconds value: " << argument.substr(10) << '\n';
                return 64;
            }
            continue;
        }

        // ── Pool / stratum arguments ──────────────────────────────────────
        if (argument.rfind("--pool=", 0) == 0) {
            // Format: --pool=host:port  or  --pool=host  (default port 3333)
            // Multiple --pool flags are accepted for failover
            should_connect_pool = true;
            std::string addr{argument.substr(7)};
            const auto colon = addr.rfind(':');
            std::string host;
            std::uint16_t port = 3333;
            if (colon != std::string::npos) {
                host = addr.substr(0, colon);
                try {
                    port = static_cast<std::uint16_t>(std::stoul(addr.substr(colon + 1)));
                } catch (...) {
                    std::cerr << "Invalid --pool port: " << addr.substr(colon + 1) << '\n';
                    return 64;
                }
            } else {
                host = std::move(addr);
            }
            pool_list.emplace_back(std::move(host), port);
            continue;
        }

        if (argument.rfind("--wallet=", 0) == 0) {
            pool_wallet = std::string{argument.substr(9)};
            continue;
        }

        if (argument.rfind("--password=", 0) == 0) {
            pool_password = std::string{argument.substr(11)};
            continue;
        }

        if (argument == "--tls") {
            pool_tls = true;
            continue;
        }
        if (argument == "--no-tls") {
            pool_tls = false;
            continue;
        }
        if (argument == "--no-verify-tls") {
            pool_tls_verify = false;
            continue;
        }
        if (argument == "--tui") {
            use_tui = true;
            continue;
        }
        if (argument == "--no-tui") {
            use_tui = false;
            continue;
        }
        if (argument == "--jit-dump") {
            jit_dump_mode = true;
            continue;
        }
        if (argument.rfind("--jit-dump=", 0) == 0) {
            jit_dump_mode = true;
            jit_dump_key = std::string{argument.substr(11)};
            continue;
        }
        if (argument.rfind("--log-level=", 0) == 0) {
            std::string lv{argument.substr(12)};
            if (lv == "trace") armrx::log::set_level(armrx::log::Level::trace);
            else if (lv == "debug") armrx::log::set_level(armrx::log::Level::debug);
            else if (lv == "info")  armrx::log::set_level(armrx::log::Level::info);
            else if (lv == "warn")  armrx::log::set_level(armrx::log::Level::warn);
            else if (lv == "error") armrx::log::set_level(armrx::log::Level::error);
            else {
                std::cerr << "Invalid --log-level: " << lv << " (choose: trace, debug, info, warn, error)\n";
                return 64;
            }
            continue;
        }
        if (argument == "--mlock") {
            use_mlock = true;
            continue;
        }
        if (argument == "--rt-priority") {
            use_rt_priority = true;
            continue;
        }
        if (argument == "--no-color") {
            tui_color = false;
            continue;
        }
        if (argument == "--color") {
            tui_color = true;
            continue;
        }
        if (argument.rfind("--stagger-ms=", 0) == 0) {
            try {
                stagger_ms = static_cast<unsigned>(std::stoul(std::string{argument.substr(13)}));
            } catch (...) {
                std::cerr << "Invalid --stagger-ms value: " << argument.substr(13) << '\n';
                return 64;
            }
            continue;
        }

        if (argument == "--help" || argument == "-h") {
            std::cout
                << "Usage: armrx [options]\n"
                << "Options:\n"
                << "  --mode=auto|light|fast     Select execution mode (default: auto)\n"
                << "  --workers=N                Set thread count (default: all online cores)\n"
                << "  --init-cache <key>         Perform Argon2d cache initialization benchmark\n"
                << "\n"
                << "Local benchmark:\n"
                << "  --mine                     Start local RandomX miner benchmark\n"
                << "  --difficulty=N             Set miner target difficulty (default: 100)\n"
                << "  --seconds=S                Duration to run benchmark in seconds, 0 for infinite (default: 10)\n"
                << "\n"
                << "Pool mining (Stratum V1):\n"
                << "  --pool=host[:port]         Pool address (default port: 3333); multiple allowed for failover\n"
                << "  --wallet=<address>         Monero wallet address (worker login)\n"
                << "  --password=<pw>            Worker password (default: x)\n"
                << "  --tls / --no-tls          Enable TLS encryption (default: off, requires OpenSSL)\n"
                << "  --no-verify-tls          Skip TLS certificate verification (default: verify)\n"
                << "  --config=<path>           Config file path (default: ~/.config/armrx/config.json)\n"
                << "  --tui / --no-tui          Terminal UI dashboard (default: off)\n"
                << "  --no-color                Disable ANSI color in TUI output\n"
                << "  --color                   Force ANSI color even when piped\n"
                << "  --mlock                   Lock all pages into RAM (prevents swapping)\n"
                << "  --rt-priority             Set SCHED_FIFO real-time priority for workers\n"
                << "  --stagger-ms=<ms>         Startup stagger per worker (ms) to reduce memory contention\n"
                << "  --log-level=<level>       Log verbosity: trace, debug, info, warn, error (default: info)\n"
                << "\n"
                << "  --help, -h               Display this help menu\n"
                << "\n"
                << "JIT introspection:\n"
                << "  --jit-dump[=<seed>]      Compile one program and dump JIT code with opcode boundaries\n";
            return 0;
        }

        std::cerr << "Unknown argument: " << argument << '\n';
        return 64;
    }

    const auto automatic     = armrx::choose_randomx_mode(memory.available_bytes, workers);
    const auto effective_mode = mode_is_auto ? automatic.mode : requested_mode;
    const auto required_bytes = mode_is_auto
        ? automatic.required_bytes
        : armrx::randomx_shared_memory(requested_mode) + workers * armrx::randomx_worker_memory()
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
              << "Selected mode (" << workers << " workers): " << armrx::mode_name(effective_mode)
              << " (requires " << required_bytes / (1024U * 1024U) << " MiB including reserve)\n";

    // Lock all pages into RAM if requested
    if (use_mlock) {
        if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            std::cerr << "[Warning] --mlock requires elevated privileges; continuing without locking.\n";
            use_mlock = false;
        }
    }

    if (!mode_is_auto && effective_mode == armrx::RandomXMode::fast
        && memory.available_bytes < required_bytes) {
        std::cerr << "Requested fast mode does not fit in available memory.\n";
        return 2;
    }

    if (should_init_cache) {
        std::vector<std::byte> key_bytes;
        key_bytes.reserve(init_cache_key.size());
        for (const auto character : init_cache_key) {
            key_bytes.push_back(static_cast<std::byte>(character));
        }
        std::cout << "Initializing 256 MiB Argon2d cache...\n";
        const auto started = std::chrono::steady_clock::now();
        armrx::Argon2dCache cache;
        cache.initialize(key_bytes);
        const auto elapsed = std::chrono::duration<double>{std::chrono::steady_clock::now() - started};
        std::cout << "Cache initialized in " << elapsed.count() << " seconds.\n";
    }

    // ── JIT dump mode ─────────────────────────────────────────────────────
    if (jit_dump_mode) {
        std::vector<std::byte> key_bytes;
        key_bytes.reserve(jit_dump_key.size());
        for (const auto c : jit_dump_key) {
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

    // ── Local benchmark ──────────────────────────────────────────────────────
    if (should_mine) {
        std::cout << "\nStarting miner benchmark (Target difficulty: " << difficulty << ")\n";
        armrx::Job job;
        job.job_id = "local_benchmark_job";
        job.block_template.resize(76, std::byte{0});
        std::string seed = "test key 000";
        job.seed_key.reserve(seed.size());
        for (char c : seed) job.seed_key.push_back(static_cast<std::byte>(c));
        job.nonce_offset = 39;
        job.nonce_size   = 4;
        job.target       = difficulty_to_target(difficulty);

        armrx::MiningEngine engine(effective_mode, workers);
        engine.set_rt_priority(use_rt_priority);
        engine.set_stagger_ms(stagger_ms);
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

        auto start_time      = std::chrono::steady_clock::now();
        unsigned elapsed_sec = 0;

        while (keep_running && (runtime_seconds == 0 || elapsed_sec < runtime_seconds)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            elapsed_sec = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count());

            const double speed       = engine.hash_rate();
            const std::uint64_t total  = engine.total_hashes();
            const std::uint64_t shares = shares_found.load();

            // TUI not supported in benchmark mode
            std::cout << "[Mining] Speed: " << std::fixed << std::setprecision(2) << speed << " H/s"
                      << " | Shares: " << shares
                      << " | Total Hashes: " << total
                      << " | Time: " << elapsed_sec << "s\r" << std::flush;
        }
        std::cout << std::endl;
        engine.stop();
        std::cout << "Mining benchmark complete.\n"
                  << "Total Hashes computed: " << engine.total_hashes() << "\n"
                  << "Final Shares found: "    << shares_found.load()    << "\n";
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

    // ── Pool mining ──────────────────────────────────────────────────────────
    if (should_connect_pool) {
        if (pool_wallet.empty()) {
            std::cerr << "Pool mining requires --wallet=<address>\n";
            return 64;
        }
        if (pool_list.empty()) {
            std::cerr << "Pool mining requires --pool=<host>[:port]\n";
            return 64;
        }

        // Connecting to pool (handled by PoolManager::connect)

        // Build PoolConfig vector from the flat pool_list
        std::vector<armrx::PoolConfig> pool_configs;
        for (const auto& p : pool_list) {
            pool_configs.push_back({p.first, p.second, pool_tls});
        }

        std::cout << "\nStarting pool miner — " << pool_configs.size()
                  << " pool(s) configured\n";

        armrx::MiningEngine engine(effective_mode, workers);
        engine.set_rt_priority(use_rt_priority);
        engine.set_stagger_ms(stagger_ms);

        std::atomic<std::uint64_t> shares_submitted{0};
        std::atomic<std::uint64_t> total_hashes_snapshot{0};

        auto pool_mgr = std::make_unique<armrx::PoolManager>(
            pool_configs, pool_wallet, pool_password, pool_tls, pool_tls_verify);

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

        // Optional TUI dashboard
        std::unique_ptr<armrx::Tui> tui;
        if (use_tui) {
            bool color = (tui_color >= 0) ? static_cast<bool>(tui_color) : armrx::Tui::detect_color();
            tui = std::make_unique<armrx::Tui>(color);
        }

        if (!use_tui) {
            std::cout << "Pool mining started. Press Ctrl+C to stop.\n";
        }

        auto start_time      = std::chrono::steady_clock::now();
        unsigned elapsed_sec = 0;

        while (keep_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
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
                // Collect per-worker rates
                std::vector<double> worker_rates;
                for (unsigned w = 0; w < workers; ++w)
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

        pool_mgr->disconnect();
        engine.stop();

        std::cout << "Pool mining stopped.\n"
                  << "Total hashes:     " << engine.total_hashes()     << "\n"
                  << "Shares submitted: " << shares_submitted.load()    << "\n";
    }

    return cpu.aarch64 ? 0 : 2;
}
