#pragma once

#include "armrx/config.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <span>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace armrx {

/**
 * Snapshot of all state needed to render one TUI frame.
 * Passed by const ref to render() — one parameter instead of 11.
 */
struct TuiSnapshot {
    // Identity / mode
    std::string_view pool_name;
    std::string_view mode;            // "light" | "fast"

    // Status
    enum class Status { mining, connecting, reconnecting, disconnected };
    Status status = Status::disconnected;
    unsigned uptime_sec = 0;
    unsigned reconnect_attempts = 0;

    // Throughput
    double total_hash_rate = 0.0;
    std::uint64_t total_hashes = 0;
    std::span<const double> worker_rates;   // per-worker H/s

    // Shares
    std::uint64_t shares_accepted = 0;
    std::uint64_t shares_rejected = 0;
    std::uint64_t shares_submitted = 0;

    // Optional (only populated when ARMRX_JIT_PROFILE)
    double jit_compile_pct = -1.0;
    double jit_execute_pct = -1.0;
};

/**
 * Simple ANSI TUI dashboard for the miner.
 * Renders an in-place updating status screen at 1 Hz.
 * Zero dependencies — pure ANSI escape codes.
 */
class Tui {
public:
    Tui();
    ~Tui();

    /** Render one frame. Call at ~1 Hz. Output goes to the given stream. */
    void render(const TuiSnapshot& s, std::ostream& os = std::cout);

    /** Hide cursor and clear on exit. */
    void shutdown();

private:
    bool enabled_ = false;
    int prev_lines_ = 0;
    void clear_lines(int n, std::ostream& os);
};

} // namespace armrx
