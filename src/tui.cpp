#include "armrx/tui.hpp"
#include "armrx/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>
#include <iomanip>
#include <unistd.h>
#include <sys/ioctl.h>

namespace armrx {

void Tui::atexit_show_cursor() {
    // Async-signal-safe: write() is safe, no allocations
    const char seq[] = "\033[?25h";
    write(STDOUT_FILENO, seq, sizeof(seq) - 1);
}

bool Tui::detect_color() {
    // NO_COLOR standard: if env var is set (even to empty), disable color
    const char* nocolor = std::getenv("NO_COLOR");
    if (nocolor && nocolor[0] != '\0') return false;
    // Also check TERM=dumb
    const char* term = std::getenv("TERM");
    if (term && (std::string(term) == "dumb" || term[0] == '\0')) return false;
    // Default: color on if stdout is a tty
    return isatty(STDOUT_FILENO) != 0;
}

unsigned Tui::term_width() {
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

Tui::Tui(bool use_color)
    : use_color_(use_color)
{
    if (use_color_) std::cout << "\033[?25l"; // hide cursor
    enabled_ = true;
    std::atexit(atexit_show_cursor); // belt-and-suspenders: show cursor even if destructor doesn't run
}

Tui::~Tui() { shutdown(); }

void Tui::ansi(const char* code, std::ostream& os) const {
    if (use_color_) os << code;
}

void Tui::shutdown() {
    if (!enabled_) return;
    enabled_ = false;
    {
        // Serialize on the shared sink mutex so a concurrent ARMRX_LOG_* write
        // (worker threads) cannot interleave into this clear/restore sequence.
        std::lock_guard<std::mutex> lock(armrx::log::sink_mutex());
        clear_lines(prev_lines_, std::cout);
        if (use_color_) std::cout << "\033[?25h" << std::flush;
    }
    prev_lines_ = 0;
}

void Tui::clear_lines(int n, std::ostream& os) {
    if (n <= 0) return;
    for (int i = 0; i < n; ++i)
        os << "\033[A\033[2K";
}

void Tui::render(const TuiSnapshot& s, std::ostream& os) {
    if (!enabled_) return;

    // Terminal-width aware: truncate pool_name, scale bar width
    unsigned tw = term_width();
    const int bar_w = std::min(20, static_cast<int>(tw) / 3);

    // Build pool_name display (truncate with ellipsis if needed)
    std::string pool_display(s.pool_name);
    unsigned max_pool_len = tw > 50 ? tw - 40 : 10;
    if (pool_display.size() > max_pool_len && max_pool_len > 3) {
        pool_display.resize(max_pool_len - 1);
        pool_display += "\xe2\x80\xa6"; // UTF-8 ellipsis
    }

    char tmp[128];
    unsigned h = s.uptime_sec / 3600, m = (s.uptime_sec % 3600) / 60, sec = s.uptime_sec % 60;
    std::snprintf(tmp, sizeof(tmp), "%02u:%02u:%02u", h, m, sec);
    std::string uptime_str(tmp);

    // Status word with color
    const char* status_color = "";
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

    // Find max rate for bar scaling — use EMA baseline to smooth jitter
    double frame_max = 0.1;
    for (auto r : s.worker_rates)
        if (r > frame_max) frame_max = r;
    bar_baseline_ema_ += kEmaAlpha * (frame_max - bar_baseline_ema_);
    double max_rate = bar_baseline_ema_;

    // Build frame
    std::stringstream frame;

    // Header
    frame << "armrx  " << pool_display << "  [" << s.mode << "]  "
          << "Up: " << uptime_str << "  ";
    ansi(status_color, frame);
    frame << status_word;
    ansi("\033[0m", frame);
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
        frame << "  W" << i << " " << std::string(static_cast<std::size_t>(bar_w), '-')
              << " 0.00 H/s\n";

    // Summary line
    frame << "  Total: " << s.total_hash_rate << " H/s"
          << "  Shares: " << s.shares_submitted
          << " (acc: " << s.shares_accepted
          << " rej: " << s.shares_rejected << ")"
          << "  Hashes: " << s.total_hashes;
    if (s.max_cpu_temp_c >= 0.0) {
        frame << "  CPU: " << std::fixed << std::setprecision(1) << s.max_cpu_temp_c << "C";
    }
    frame << "\n";

    int new_lines = 1 + 8 + 1; // header + 8 worker bars + summary

    if (s.jit_compile_pct >= 0.0 && s.jit_execute_pct >= 0.0) {
        frame << "  JIT Profile: Compile " << std::fixed << std::setprecision(1) << s.jit_compile_pct << "%"
              << " | Execute " << std::fixed << std::setprecision(1) << s.jit_execute_pct << "%\n";
        new_lines += 1;
    }

    // Move cursor up, clear, print new frame. Serialize on the shared sink
    // mutex so a concurrent ARMRX_LOG_* -> std::cout (worker threads) cannot
    // interleave its bytes into the middle of this escape sequence (Bug 2:
    // "armrx" header + raw control bytes, overlapping lines). With
    // set_tui_mode(true) active, worker logs go to the ring buffer instead of
    // stdout, so this lock is defense-in-depth for the general contract.
    {
        std::lock_guard<std::mutex> lock(armrx::log::sink_mutex());
        clear_lines(prev_lines_, os);
        os << "\033[J";  // clear from cursor to bottom
        os << frame.str() << std::flush;
    }

    prev_lines_ = new_lines;
}

} // namespace armrx
