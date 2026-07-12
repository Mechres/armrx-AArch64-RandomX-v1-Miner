#include "armrx/argon2.hpp"
#include "armrx/cpu_features.hpp"
#include "armrx/memory.hpp"
#include "armrx/randomx_config.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const auto cpu = armrx::detect_cpu_features();
    const auto memory = armrx::available_memory();
    auto workers = std::max(1U, std::thread::hardware_concurrency());
    bool mode_is_auto = true;
    armrx::RandomXMode requested_mode = armrx::RandomXMode::light;
    bool should_init_cache = false;
    std::string init_cache_key;

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

        if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: armrx [--mode=auto|light|fast] [--workers=N] [--init-cache <key>]\n";
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
        for (const auto character : init_cache_key) key_bytes.push_back(static_cast<std::byte>(character));
        std::cout << "Initializing 256 MiB Argon2d cache...\n";
        const auto started = std::chrono::steady_clock::now();
        armrx::Argon2dCache cache;
        cache.initialize(key_bytes);
        const auto elapsed = std::chrono::duration<double>{std::chrono::steady_clock::now() - started};
        std::cout << "Cache initialized in " << elapsed.count() << " seconds.\n";
    }
    return cpu.aarch64 ? 0 : 2;
}
