#include "armrx/tui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <vector>

namespace armrx {

Tui::Tui() {
    std::cout << "\033[?25l"; // hide cursor
    enabled_ = true;
}

Tui::~Tui() { shutdown(); }

void Tui::shutdown() {
    if (!enabled_) return;
    enabled_ = false;
    for (int i = 0; i < prev_lines_; ++i) std::cout << "\033[A\033[2K";
    std::cout << "\033[?25h" << std::flush;
    prev_lines_ = 0;
}

void Tui::render(const std::string& pool_name, const std::string& status,
                 unsigned uptime_sec, double total_hash_rate,
                 std::uint64_t total_hashes, std::uint64_t shares,
                 const std::vector<double>& worker_rates,
                 unsigned workers, const std::string& mode) {
    if (!enabled_) return;

    char tmp[128];
    unsigned h = uptime_sec / 3600, m = (uptime_sec % 3600) / 60, s = uptime_sec % 60;
    std::snprintf(tmp, sizeof(tmp), "%02u:%02u:%02u", h, m, s);
    std::string uptime_str(tmp);

    // Find max rate for bar scaling
    double max_rate = 0.1;
    for (auto r : worker_rates)
        if (r > max_rate) max_rate = r;

    // Build frame
    std::stringstream frame;
    const int bar_w = 20;

    // Header
    frame << "armrx  " << pool_name << "  [" << mode << "]  "
          << "Up: " << uptime_str << "  " << status << "\n";

    // Per-worker bars (max 8)
    unsigned n = std::min(static_cast<unsigned>(worker_rates.size()), 8u);
    for (unsigned i = 0; i < n; ++i) {
        double rate = worker_rates[i];
        int filled = static_cast<int>((rate / max_rate) * bar_w);
        if (filled < 0) filled = 0;
        if (filled > bar_w) filled = bar_w;
        frame << "  W" << i << " ";
        for (int b = 0; b < bar_w; ++b)
            frame << (b < filled ? '#' : '-');
        frame << " " << rate << " H/s\n";
    }
    for (unsigned i = n; i < workers && i < 8; ++i)
        frame << "  W" << i << " -------------------- 0.00 H/s\n";

    // Summary
    frame << "  Total: " << total_hash_rate << " H/s"
          << "  Shares: " << shares
          << "  Hashes: " << total_hashes << "\n";

    int new_lines = 1 + std::min(workers, 8u) + 1;

    // Move cursor up, clear, print new frame
    for (int i = 0; i < prev_lines_; ++i)
        std::cout << "\033[A";
    std::cout << "\033[J";  // clear from cursor to bottom
    std::cout << frame.str() << std::flush;

    prev_lines_ = new_lines;
}

} // namespace armrx
