#include "armrx/argon2.hpp"
#include "armrx/cpu_features.hpp"
#include "armrx/memory.hpp"
#include "armrx/randomx_config.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    const auto cpu = armrx::detect_cpu_features();
    const auto memory = armrx::available_memory();
    const auto workers = std::max(1U, std::thread::hardware_concurrency());
    const auto automatic = armrx::choose_randomx_mode(memory.available_bytes, workers);
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
              << "Auto mode (" << workers << " workers): " << armrx::mode_name(automatic.mode)
              << " (requires " << automatic.required_bytes / (1024U * 1024U) << " MiB including reserve)\n";

    if (argc == 3 && std::string{argv[1]} == "--init-cache") {
        const std::string key{argv[2]};
        std::vector<std::byte> key_bytes;
        key_bytes.reserve(key.size());
        for (const auto character : key) key_bytes.push_back(static_cast<std::byte>(character));
        std::cout << "Initializing 256 MiB Argon2d cache...\n";
        const auto started = std::chrono::steady_clock::now();
        armrx::Argon2dCache cache;
        cache.initialize(key_bytes);
        const auto elapsed = std::chrono::duration<double>{std::chrono::steady_clock::now() - started};
        std::cout << "Cache initialized in " << elapsed.count() << " seconds.\n";
    } else if (argc != 1) {
        std::cerr << "Usage: armrx [--init-cache <key>]\n";
        return 64;
    }
    return cpu.aarch64 ? 0 : 2;
}
