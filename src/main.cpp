#include "armrx/argon2.hpp"
#include "armrx/cpu_features.hpp"
#include "armrx/memory.hpp"
#include "armrx/randomx_config.hpp"
#include "armrx/mining_engine.hpp"
#include "armrx/stratum_client.hpp"

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

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);

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
    std::string pool_host;
    std::uint16_t pool_port = 3333;
    std::string pool_wallet;
    std::string pool_password = "x";

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
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
            workers = std::max(1U, static_cast<unsigned>(std::stoul(std::string{argument.substr(10)})));
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
            difficulty = std::stoull(std::string{argument.substr(13)});
            continue;
        }

        if (argument.rfind("--seconds=", 0) == 0) {
            runtime_seconds = static_cast<unsigned>(std::stoul(std::string{argument.substr(10)}));
            continue;
        }

        // ── Pool / stratum arguments ──────────────────────────────────────
        if (argument.rfind("--pool=", 0) == 0) {
            // Format: --pool=host:port  or  --pool=host  (default port 3333)
            should_connect_pool = true;
            std::string addr{argument.substr(7)};
            const auto colon = addr.rfind(':');
            if (colon != std::string::npos) {
                pool_host = addr.substr(0, colon);
                pool_port = static_cast<std::uint16_t>(std::stoul(addr.substr(colon + 1)));
            } else {
                pool_host = std::move(addr);
            }
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
                << "  --pool=host[:port]         Pool address (default port: 3333)\n"
                << "  --wallet=<address>         Monero wallet address (worker login)\n"
                << "  --password=<pw>            Worker password (default: x)\n"
                << "\n"
                << "  -h, --help                 Display this help menu\n";
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
    }

    // ── Pool mining ──────────────────────────────────────────────────────────
    if (should_connect_pool) {
        if (pool_wallet.empty()) {
            std::cerr << "Pool mining requires --wallet=<address>\n";
            return 64;
        }
        if (pool_host.empty()) {
            std::cerr << "Pool mining requires --pool=<host>[:port]\n";
            return 64;
        }

        std::cout << "\nStarting pool miner — connecting to "
                  << pool_host << ':' << pool_port << '\n';

        armrx::MiningEngine engine(effective_mode, workers);

        // Share callback: forward found shares to pool
        armrx::StratumClient stratum(pool_host, pool_port, pool_wallet, pool_password);

        std::atomic<std::uint64_t> shares_submitted{0};
        std::atomic<std::uint64_t> total_hashes_snapshot{0};

        auto share_callback = [&](const armrx::Job& job, std::uint64_t nonce,
                                  std::array<std::byte, 32> hash) {
            shares_submitted.fetch_add(1, std::memory_order_relaxed);
            std::cout << "[Pool] Share found! Nonce: " << std::hex << nonce
                      << " Hash: " << hash_to_hex(hash) << std::dec << '\n';
            stratum.submit_share(job, nonce, hash);
        };

        // Job callback: push new jobs from pool into the mining engine
        stratum.set_job_callback([&](const armrx::Job& job) {
            engine.set_job(job);
        });

        // Error callback: print disconnect/error messages
        stratum.set_error_callback([](const std::string& reason) {
            std::cerr << "[Stratum] Disconnected: " << reason << '\n';
        });

        engine.start(share_callback);

        // Initial connection — if it fails, the reconnect loop handles retries
        try {
            stratum.connect();
        } catch (const std::exception& ex) {
            std::cerr << "[Stratum] Initial connection failed: " << ex.what() << '\n';
            std::cerr << "[Stratum] Will retry with backoff...\n";
        }

        std::cout << "Pool mining started. Press Ctrl+C to stop.\n";

        auto start_time      = std::chrono::steady_clock::now();
        unsigned elapsed_sec = 0;

        while (keep_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            elapsed_sec = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count());

            const double speed  = engine.hash_rate();
            const auto total    = engine.total_hashes();
            const auto shares   = shares_submitted.load();
            const bool online   = stratum.is_connected();
            const auto retries  = stratum.reconnect_attempts();

            std::cout << "[Pool] Speed: " << std::fixed << std::setprecision(2) << speed << " H/s"
                      << " | Shares: " << shares
                      << " | Total: "     << total
                      << " | Uptime: "    << elapsed_sec << "s";
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
        std::cout << std::endl;

        stratum.disconnect();
        engine.stop();

        std::cout << "Pool mining stopped.\n"
                  << "Total hashes:     " << engine.total_hashes()     << "\n"
                  << "Shares submitted: " << shares_submitted.load()    << "\n";
    }

    return cpu.aarch64 ? 0 : 2;
}
