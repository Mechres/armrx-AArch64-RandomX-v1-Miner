// Diagnostic: (1) time the PartialDataset fill, (2) then hash continuously
// for a fixed window, sampling aggregate H/s every 15s, to measure whether the
// hashrate RAMPS after fill (the reported "20-minute warmup") or is instant.
// No pool/job needed. Run on-device: ./time_partial_fill 512

#include "armrx/partial_dataset.hpp"
#include "armrx/argon2.hpp"
#include "armrx/randomx_config.hpp"
#include "armrx/vm.hpp"
#include "armrx/dataset.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <span>
#include <thread>

int main(int argc, char** argv) {
    const std::uint64_t mb = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 512ULL;
    const std::size_t item_count = (mb * 1024ULL * 1024ULL) / armrx::kRandomXDatasetItemBytes;

    auto t0 = std::chrono::steady_clock::now();
    armrx::PartialDataset pd(item_count);
    auto t_alloc = std::chrono::steady_clock::now();

    std::vector<std::byte> seed_key(64, std::byte{0xAB});
    auto cache = std::make_shared<armrx::Argon2dCache>();
    cache->initialize(seed_key);
    auto t_cache = std::chrono::steady_clock::now();

    std::vector<unsigned> core_order;
    for (unsigned i = 0; i < std::thread::hardware_concurrency(); ++i) core_order.push_back(i);
    pd.start_fill(cache, core_order, {});
    pd.wait_for_fill();
    auto t_fill = std::chrono::steady_clock::now();

    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    std::printf("[fill] alloc_ms=%.1f cache_ms=%.1f FILL_ms=%.1f (%.2f s)\n",
                 ms(t0, t_alloc), ms(t_alloc, t_cache), ms(t_cache, t_fill), ms(t_cache, t_fill) / 1000.0);

    // --- continuous hash ramp measurement ---
    const std::uint32_t flags = armrx::kRandOMXFlagHardAes | armrx::kRandOMXFlagJit | armrx::kRandOMXFlagFullMem;
    armrx::VirtualMachine vm(flags);
    vm.set_cache(cache.get());
    // Wire the filled partial dataset (Track B hybrid light mode).
    vm.set_partial_dataset(pd.data(), pd.item_count_atomic());

    constexpr std::size_t kScratch = 2ULL * 1024 * 1024;
    auto scratch = std::make_unique<std::byte[]>(kScratch);
    vm.set_scratchpad(scratch.get(), kScratch);

    std::array<std::byte, 76> block{};
    for (size_t i = 0; i < block.size(); ++i) block[i] = static_cast<std::byte>(i & 0xff);
    std::array<std::byte, 32> out{};

    // Prime once.
    vm.run(block.data());

    const double kWindowS = 15.0;
    const int kWindows = 12; // 3 minutes
    std::uint64_t total_hashes = 0;
    try {
    for (int w = 0; w < kWindows; ++w) {
        auto ws = std::chrono::steady_clock::now();
        std::uint64_t n = 0;
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - ws).count() < kWindowS) {
            // vary the seed a bit so we don't just re-run one cached program path identically
            block[0] = static_cast<std::byte>((total_hashes) & 0xff);
            block[1] = static_cast<std::byte>((total_hashes >> 8) & 0xff);
            vm.run(block.data());
            ++n;
            ++total_hashes;
        }
        auto we = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(we - ws).count();
        std::printf("[ramp] t=%.0fs window_h/s=%.2f cumulative_h/s=%.2f\n",
                     ms(t_fill, we) / 1000.0, n / dt, total_hashes / std::chrono::duration<double>(we - t_fill).count());
    }
    } catch (const std::exception& e) {
        std::printf("[ramp] EXCEPTION: %s\n", e.what());
    }
    std::cout << "done. total_hashes=" << total_hashes << "\n" << std::flush;
    return 0;
}
