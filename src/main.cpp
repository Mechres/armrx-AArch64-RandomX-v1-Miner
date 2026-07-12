#include "armrx/cpu_features.hpp"
#include "armrx/memory.hpp"
#include "armrx/randomx_config.hpp"

#include <algorithm>
#include <iostream>
#include <thread>

int main() {
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
    return cpu.aarch64 ? 0 : 2;
}
