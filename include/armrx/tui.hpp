#pragma once

#include "armrx/config.hpp"
#include <chrono>
#include <cstdint>
#include <iostream>
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
    /**
     * @param use_color  true to emit ANSI color codes, false for plain text.
     *                   If omitted, checks NO_COLOR env var and isatty().
     */
    explicit Tui(bool use_color = detect_color());

    ~Tui();

    /** Render one frame. Call at ~1 Hz. Output goes to the given stream. */
    void render(const TuiSnapshot& s, std::ostream& os = std::cout);

    /** Hide cursor and clear on exit. */
    void shutdown();

    /** Override color policy after construction. */
    void set_color(bool c) { use_color_ = c; }

    /** Detect whether color should be used by default (checks NO_COLOR, TERM, isatty). */
    static bool detect_color();

private:
    bool enabled_ = false;
    bool use_color_ = true;
    int prev_lines_ = 0;
    double bar_baseline_ema_ = 0.0;
    static constexpr double kEmaAlpha = 0.2;

    /** atexit callback: always show cursor on exit. */
    static void atexit_show_cursor();

    /** Query terminal width via ioctl, default 80. */
    static unsigned term_width();

    /** Emit ANSI color code if use_color_ is true. */
    void ansi(const char* code, std::ostream& os) const;

    void clear_lines(int n, std::ostream& os);
};

} // namespace armrx
