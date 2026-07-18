#include "armrx/tui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <vector>
#include <iomanip>

namespace armrx {

Tui::Tui() {
    std::cout << "\033[?25l"; // hide cursor
    enabled_ = true;
}

Tui::~Tui() { shutdown(); }

void Tui::shutdown() {
    if (!enabled_) return;
    enabled_ = false;
    clear_lines(prev_lines_, std::cout);
    std::cout << "\033[?25h" << std::flush;
    prev_lines_ = 0;
}

void Tui::clear_lines(int n, std::ostream& os) {
    if (n <= 0) return;
    for (int i = 0; i < n; ++i)
        os << "\033[A\033[2K";
}

void Tui::render(const TuiSnapshot& s, std::ostream& os) {
    if (!enabled_) return;

    char tmp[128];
    unsigned h = s.uptime_sec / 3600, m = (s.uptime_sec % 3600) / 60, sec = s.uptime_sec % 60;
    std::snprintf(tmp, sizeof(tmp), "%02u:%02u:%02u", h, m, sec);
    std::string uptime_str(tmp);

    // Status word with color
    const char* status_color = "";
    const char* status_reset = "";
    const char* status_word = "";
    switch (s.status) {
        case TuiSnapshot::Status::mining:
            status_color = "\033[32m"; status_word = "MINING"; break;
        case TuiSnapshot::Status::connecting:
            status_color = "\033[33m"; status_word = "CONNECTING"; break;
        case TuiSnapshot::Status::reconnecting:
            status_color = "\033[33m"; status_word = "RECONNECTING"; break;
        case TuiSnapshot::Status::disconnected:
            status_color = "\033[31m"; status_word = "DISCONNECTED"; break;
    }

    // Find max rate for bar scaling
    double max_rate = 0.1;
    for (auto r : s.worker_rates)
        if (r > max_rate) max_rate = r;

    // Build frame
    std::stringstream frame;
    const int bar_w = 20;

    // Header
    frame << "armrx  " << s.pool_name << "  [" << s.mode << "]  "
          << "Up: " << uptime_str << "  "
          << status_color << status_word << status_reset;
    if (s.reconnect_attempts > 0)
        frame << " (attempt " << s.reconnect_attempts << ")";
    frame << "\n";

    // Per-worker bars (max 8)
    unsigned n = std::min(static_cast<unsigned>(s.worker_rates.size()), 8u);
    for (unsigned i = 0; i < n; ++i) {
        double rate = s.worker_rates[i];
        int filled = static_cast<int>((rate / max_rate) * bar_w);
        if (filled < 0) filled = 0;
        if (filled > bar_w) filled = bar_w;
        frame << "  W" << i << " ";
        for (int b = 0; b < bar_w; ++b)
            frame << (b < filled ? '#' : '-');
        frame << " " << rate << " H/s\n";
    }
    for (unsigned i = n; i < 8; ++i)
        frame << "  W" << i << " -------------------- 0.00 H/s\n";

    // Summary line
    frame << "  Total: " << s.total_hash_rate << " H/s"
          << "  Shares: " << s.shares_submitted
          << " (acc: " << s.shares_accepted
          << " rej: " << s.shares_rejected << ")"
          << "  Hashes: " << s.total_hashes << "\n";

    int new_lines = 1 + 8 + 1; // header + 8 worker bars + summary

    if (s.jit_compile_pct >= 0.0 && s.jit_execute_pct >= 0.0) {
        frame << "  JIT Profile: Compile " << std::fixed << std::setprecision(1) << s.jit_compile_pct << "%"
              << " | Execute " << std::fixed << std::setprecision(1) << s.jit_execute_pct << "%\n";
        new_lines += 1;
    }

    // Move cursor up, clear, print new frame
    clear_lines(prev_lines_, os);
    os << "\033[J";  // clear from cursor to bottom
    os << frame.str() << std::flush;

    prev_lines_ = new_lines;
}

} // namespace armrx
