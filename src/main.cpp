#include "armrx/argon2.hpp"
#include "armrx/cpu_features.hpp"
#include "armrx/memory.hpp"
#include "armrx/randomx_config.hpp"
#include "armrx/mining_engine.hpp"

#include <algorithm>
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
        target.bytes[i] = static_cast<std::byte>(val / diff);
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

    const auto cpu = armrx::detect_cpu_features();
    const auto memory = armrx::available_memory();
    auto workers = std::max(1U, std::thread::hardware_concurrency());
    bool mode_is_auto = true;
    armrx::RandomXMode requested_mode = armrx::RandomXMode::light;
    bool should_init_cache = false;
    std::string init_cache_key;

    bool should_mine = false;
    std::uint64_t difficulty = 100;
    unsigned int runtime_seconds = 10;

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

        if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: armrx [options]\n"
                      << "Options:\n"
                      << "  --mode=auto|light|fast     Select execution mode (default: auto)\n"
                      << "  --workers=N                Set thread count (default: all online cores)\n"
                      << "  --init-cache <key>         Perform Argon2d cache initialization benchmark\n"
                      << "  --mine                     Start local RandomX miner benchmark\n"
                      << "  --difficulty=N             Set miner target difficulty (default: 100)\n"
                      << "  --seconds=S                Duration to run benchmark in seconds, 0 for infinite (default: 10)\n"
                      << "  -h, --help                 Display this help menu\n";
            return 0;
        }

        std::cerr << "Unknown argument: " << argument << '\n';
        return 64;
    }

    const auto automatic = armrx::choose_randomx_mode(memory.available_bytes, workers);
    const auto effective_mode = mode_is_auto ? automatic.mode : requested_mode;
    const auto required_bytes = mode_is_auto
        ? automatic.required_bytes
        : armrx::randomx_shared_memory(requested_mode) + workers * armrx::randomx_worker_memory()
              + armrx::kAutoModeSafetyReserve;

    std::cout << "armrx " << (cpu.aarch64 ? "AArch64" : "non-AArch64") << '\n'
              << "AES: " << (cpu.aes ? "available" : "unavailable") << '\n'
              << "CRC32: " << (cpu.crc32 ? "available" : "unavailable") << '\n'
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

    if (!mode_is_auto && effective_mode == armrx::RandomXMode::fast && memory.available_bytes < required_bytes) {
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

    if (should_mine) {
        std::cout << "\nStarting miner benchmark (Target difficulty: " << difficulty << ")\n";
        armrx::Job job;
        job.job_id = "local_benchmark_job";
        // 76-byte block template (typical Monero block size)
        job.block_template.resize(76, std::byte{0});
        // Seed value for "test key 000"
        std::string seed = "test key 000";
        job.seed_key.reserve(seed.size());
        for (char c : seed) {
            job.seed_key.push_back(static_cast<std::byte>(c));
        }
        job.nonce_offset = 39;
        job.nonce_size = 4;
        job.target = difficulty_to_target(difficulty);

        armrx::MiningEngine engine(effective_mode, workers);
        engine.set_job(job);

        std::atomic<std::uint64_t> shares_found{0};
        auto share_callback = [&shares_found](const armrx::Job& j, std::uint64_t nonce, std::array<std::byte, 32> hash) {
            shares_found.fetch_add(1, std::memory_order_relaxed);
            std::cout << "[Mining] Valid share found! Job: " << j.job_id
                      << " | Nonce: " << std::hex << nonce
                      << " | Hash: " << hash_to_hex(hash) << std::dec << std::endl;
        };

        engine.start(share_callback);
        std::cout << "Mining started. Press Ctrl+C to stop.\n";

        auto start_time = std::chrono::steady_clock::now();
        unsigned int elapsed_seconds = 0;

        while (keep_running && (runtime_seconds == 0 || elapsed_seconds < runtime_seconds)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            elapsed_seconds = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start_time).count());

            double speed = engine.hash_rate();
            std::uint64_t total = engine.total_hashes();
            std::uint64_t shares = shares_found.load();

            std::cout << "[Mining] Speed: " << std::fixed << std::setprecision(2) << speed << " H/s"
                      << " | Shares: " << shares
                      << " | Total Hashes: " << total
                      << " | Time: " << elapsed_seconds << "s\r" << std::flush;
        }
        std::cout << std::endl;

        engine.stop();
        std::cout << "Mining benchmark complete.\n"
                  << "Total Hashes computed: " << engine.total_hashes() << "\n"
                  << "Final Shares found: " << shares_found.load() << "\n";
    }

    return cpu.aarch64 ? 0 : 2;
}
