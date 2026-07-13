#pragma once

#include "armrx/config.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace armrx {

/**
 * Simple ANSI TUI dashboard for the miner.
 * Renders an in-place updating status screen at 1 Hz.
 * Zero dependencies — pure ANSI escape codes.
 */
class Tui {
public:
    Tui();
    ~Tui();

    /** Render one frame. Call at ~1 Hz. */
    void render(const std::string& pool_name, const std::string& status,
                unsigned uptime_sec, double total_hash_rate,
                std::uint64_t total_hashes, std::uint64_t shares,
                const std::vector<double>& worker_rates,
                unsigned workers, const std::string& mode);

    /** Hide cursor and clear on exit. */
    void shutdown();

private:
    bool enabled_ = false;
    int prev_lines_ = 0;
    void clear_lines(int n);
};

} // namespace armrx
