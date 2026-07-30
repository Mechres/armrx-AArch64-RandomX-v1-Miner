# armrx — TUI & Usability Plan

> A focused, execution-ready plan for the operator-facing surface: the `--tui`
> dashboard, CLI ergonomics, config-file handling, log/output discipline, and
> observability hooks. Scope-deliberately narrow — this complements
> [`next_phase_v2.md`](next_phase_v2.md), which owns correctness, performance,
> and architecture. Cross-references to that doc are explicit so the two plans
> don't duplicate work.
>
> **Audience:** the agent that picks this up. Every item names the file and
> line it touches, the size of the change, and a concrete acceptance check.

---

## 0. Relationship to `next_phase_v2.md`

Several TUI/usability items are *already scheduled* in the master plan. This
document does not re-own them; it deepens them and adds the items the master
plan treated as out-of-scope.

| Item | Owned by | Deepened here? |
|---|---|---|
| Structured logger (`log.hpp`) replacing `std::cerr`/`std::cout` | `next_phase_v2.md` §2.5, Phase 2.3 | **§3.1** — logger design from a TUI-lens (ring buffer, level routing, color policy) |
| `stratum_` race / `std::cout` race under `--tui` | `next_phase_v2.md` §4.1, Phase 1.2 | **§3.2** — how the TUI consumes logs without racing |
| SIGTERM handler + `[DEBUG]` removal | `next_phase_v2.md` Phase 1.4 | **§4.2, §4.4** — signal-safe TUI teardown specifically |
| Prometheus metrics endpoint | `next_phase_v2.md` Phase 3.3 | **§6** — metric *surface* design (what to expose) |

**New work owned only by this document:** the `render()` API redesign (§2),
terminal-width awareness (§2.3), share accept/reject state surfacing (§4.1),
CLI ergonomics gaps (§5), config-file parser dedup (§5.3), and benchmark-mode
TUI parity (§4.3).

---

## 1. Current State Assessment

### 1.1 What exists

- **`src/tui.cpp` (91 LOC) + `include/armrx/tui.hpp` (37 LOC)** — a single
  `Tui::render()` method taking **11 positional parameters** (`tui.cpp:28–33`),
  emitting a fixed-layout ANSI frame at ~1 Hz from the main loop
  (`main.cpp:454–464`). Per-worker ASCII bars (`#`/`-`), total H/s, share
  count, optional JIT profile line. Color is used for the status word only
  (`main.cpp:455–456`: green/amber/red).
- **CLI** in `src/main.cpp:114–253` — `--mode`, `--workers`, `--mine`,
  `--difficulty`, `--seconds`, `--pool`, `--wallet`, `--password`, `--tls`,
  `--no-tls`, `--no-verify-tls`, `--tui`, `--no-tui`, `--mlock`,
  `--rt-priority`, `--config`, `--init-cache`, `--help`. Numeric parsing is
  unguarded (`std::stoul`/`std::stoull` at `:136,156,161,176` — throws
  uncaught → `std::terminate`, flagged in `next_phase_v2.md` §4.2 #8).
- **Config file** in `src/config.cpp` — JSON via the ad-hoc parser, three
  lookup locations (`$ARMRX_CONFIG`, `~/.config/armrx/config.json`,
  `./armrx.conf`). `AppConfig` struct at `config.hpp:15–25`. A second CLI
  parser `apply_cli_overrides()` exists at `config.cpp:107–136` **but is never
  called** — `main.cpp` reimplements parsing inline. Two parsers, drift risk.
- **Logging** — none. Every module writes `std::cerr`/`std::cout` directly.
  Stratum reader thread (`stratum_client.cpp`), TLS client, pool manager,
  mining engine, config loader, main loop all compete for `std::cout`.

### 1.2 Operator-facing bugs and gaps (concrete)

| # | Issue | Site | Severity |
|---|---|---|---|
| U1 | **`render()` has 11 positional parameters** — caller must count args; adding a field means editing the call site and the signature in lockstep. Classic "long parameter list" smell. | `tui.hpp:21–26`, `main.cpp:461–464` | Maintainability |
| U2 | **`prev_lines_` cursor math assumes fixed line count** (`tui.cpp:74`: `1 + min(workers,8) + 1`) but a long pool name or narrow terminal wraps a line, so the cursor walks up past the frame top and eats scrollback forever. No `tput cols` / `ioctl(TIOCGWINSZ)` awareness. | `tui.cpp:74,83–84` | **Visual corruption** |
| U3 | **`std::cout` race**: TUI writes from main thread (`tui.cpp:86`); stratum reader writes from its own thread (`stratum_client.cpp` — `[Pool]`/`[Stratum]` lines); share callback writes from worker threads (`main.cpp:412`). Under `--tui` these interleave and corrupt the dashboard. | cross-cutting | **Functional** |
| U4 | **No share accept/reject distinction**: only a `shares_submitted` counter incremented *on local submit* (`main.cpp:411`), never updated with the pool's `result:true/false` reply. A pool rejecting every share still shows "Shares: N↑". The reply is parsed (`stratum_client.cpp:602–618`) via fragile substring match and discarded. | `main.cpp:411`, `stratum_client.cpp:616–618` | **Misleading metric** |
| U5 | **TUI disabled in benchmark mode** (`main.cpp:345` "TUI not supported in benchmark mode"). The local `--mine` path uses a `\r`-overwrite single-line status (`main.cpp:346–349`) — inconsistent with the pool path's dashboard. | `main.cpp:305–367` | UX |
| U6 | **`[DEBUG]` left in production**: `main.cpp:379–380` unconditionally prints `[DEBUG] Connecting to pool: ... wallet=XXXXXXXXXX...`. Leaks wallet prefix to logs. | `main.cpp:379–380` | **Security/info-leak** |
| U7 | **No `--no-color` / NO_COLOR support**. ANSI escapes emitted unconditionally. Piped output (`armrx ... | tee log`) contains raw `\033[…m`. Breaks `grep`, log shippers, `termux` rendering. | `tui.cpp:14,23,24,84–86`, `main.cpp:455–456,478–481` | Accessibility/ops |
| U8 | **No `--version` / build-identification flag**. Impossible to tell which commit is running on a remote device. | `main.cpp:223–249` (help block) | Ops |
| U9 | **Config file parser is dead code** (`config.cpp:107` `apply_cli_overrides` never invoked); `main.cpp:114–253` has its own parser. Divergent flag sets (e.g. `--rt-priority`, `--mlock`, `--no-verify-tls` exist in `main.cpp` but not in `apply_cli_overrides`). | `config.cpp:107–136` vs `main.cpp:114–253` | Maintainability |
| U10 | **No config validation / dry-run**. A typo'd pool port or malformed wallet is discovered only at runtime when the connection fails or shares are rejected. | `config.cpp` | UX |
| U11 | **SIGTERM leaves cursor hidden**. `Tui::shutdown()` only runs from the destructor; a `kill -TERM` skips it and the terminal stays in `\033[?25l` (cursor-hidden) state. (SIGTERM handler itself is `next_phase_v2.md` Phase 1.4.) | `tui.cpp:13–26` | UX |
| U12 | **Worker hash-rate bar baseline is per-frame**: `max_rate` recomputed each render (`tui.cpp:42–44`), so bars jitter in length as the fastest worker's rate fluctuates. Hard to read at a glance. | `tui.cpp:42–44,58` | UX |
| U13 | **No pool latency / last-share time**. Operator cannot tell if the pool is slow or if shares stopped flowing. The reader thread knows reply timing but doesn't surface it. | `stratum_client.cpp` | Observability |

---

## 2. TUI Redesign

### 2.1 Replace the 11-parameter `render()` with a state struct

The current signature (`tui.hpp:21–26`) is the root cause of U1 and makes
every new field a four-site edit (struct, signature, call site, layout). Push
the data into a value type and pass a const ref.

```cpp
// include/armrx/tui.hpp
namespace armrx {

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

    // Shares (U4 fix)
    std::uint64_t shares_accepted = 0;
    std::uint64_t shares_rejected = 0;
    std::uint64_t shares_submitted = 0;     // = accepted + rejected + in-flight

    // Optional (only populated when ARMRX_JIT_PROFILE)
    double jit_compile_pct = -1.0;
    double jit_execute_pct = -1.0;

    // Optional pool telemetry (U13)
    std::chrono::milliseconds pool_latency{-1};   // -1ms = unknown
    std::chrono::steady_clock::time_point last_share_time{};
    bool last_share_known = false;
};

class Tui {
public:
    Tui();
    ~Tui();
    void render(const TuiSnapshot& s);   // <-- one parameter
    void shutdown();
    // ... see §2.2, §2.3
};

} // namespace armrx
```

**Acceptance check:** `main.cpp:454–464` collapses to `tui->render(snap)`
where `snap` is assembled from engine + pool state in one place. Adding a
field is a two-site edit (struct + layout), not four.

**Size:** M. Touches `tui.hpp`, `tui.cpp`, `main.cpp:454–484`. No behavior
change — pure refactor. Land first; everything else in §2 builds on it.

### 2.2 Smooth the worker bars (U12)

Replace per-frame `max_rate` with an exponential moving average baseline so
bar lengths don't flicker:

```cpp
// tui.hpp (private)
double bar_baseline_ema_ = 0.0;
static constexpr double kEmaAlpha = 0.2;   // 1 Hz → ~5 s time constant
```

Each frame: `bar_baseline_ema_ = std::max(0.1, bar_baseline_ema_ + kEmaAlpha * (frame_max - bar_baseline_ema_))`. Bar fill uses the EMA, not the instantaneous max. The numeric H/s readout stays instantaneous.

**Acceptance check:** with 8 workers on the dev device, bar lengths change by
≤ 1 cell frame-to-frame at steady state (currently they jump by 3–5).

**Size:** S.

### 2.3 Terminal width awareness (U2)

Query columns at startup and on `SIGWINCH`:

```cpp
// tui.cpp
#include <sys/ioctl.h>
#include <unistd.h>
unsigned Tui::term_cols() {
    winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;   // sane default for piped / serial / termux
}
```

Then **truncate `pool_name`** to `term_cols() - 40` and **scale `bar_w`** to
`std::min(20u, term_cols() / 3)`. Recompute on each render (cheap) so a
terminal resize mid-run picks up without needing a SIGWINCH handler. The
`prev_lines_` counter (U2 root cause) becomes correct because no line wraps.

**Acceptance check:** in an 80-column terminal with pool
`xmr-herominers-portugal.heroaminers.com:1011` (43 chars), the header no
longer wraps. In a 40-column terminal, bars shrink and the header truncates
with `…`.

**Size:** S. Pure additive.

### 2.4 Color / NO_COLOR policy (U7)

Honor the de-facto standard:

```cpp
// tui.cpp constructor
const char* nocolor = std::getenv("NO_COLOR");
const char* term = std::getenv("TERM");
bool dumb = (term && (std::string(term) == "dumb" || std::string(term) == ""));
use_color_ = (!nocolor || nocolor[0] == '\0') && !dumb
             && ::isatty(STDOUT_FILENO);
```

Add `--no-color` and `--color` CLI flags to force override. Wrap every ANSI
sequence in a tiny helper `const char* Tui::color(Status s)` returning `""`
when `!use_color_`. Also gate the `\033[?25l` / `\033[?25h` cursor sequences
(if `!use_color_` and not a tty, skip them entirely — piped output should be
plain text).

**Acceptance check:** `armrx --mine --seconds=2 2>&1 | cat` produces no `\033`
bytes. `NO_COLOR=1 armrx --tui ...` renders in monochrome. `--color` forces
codes even when piped (for `less -R` workflows).

**Size:** S. Depends on §2.1 landing first (so the color helper lives in one
place).

### 2.5 Signal-safe teardown (U11)

Register an `atexit` handler in the `Tui` constructor (in addition to the
destructor) that calls `shutdown()`. Pair with the SIGTERM handler from
`next_phase_v2.md` Phase 1.4 — that handler flips `keep_running`; the natural
main-loop exit then unwinds and `atexit` fires the cursor-show. Belt and
braces: also install a minimal `signal(SIGTERM, ...)` that *only* writes the
cursor-show sequence via `write(STDOUT_FILENO, "\033[?25h", 6)` (async-signal-safe; no allocations, no `std::cout`).

**Acceptance check:** `kill -TERM $(pidof armrx)` while `--tui` is running
leaves the terminal with a visible cursor and no leftover escape sequences.

**Size:** S.

---

## 3. Output Discipline

### 3.1 Logger design (TUI-lens view of `next_phase_v2.md` §2.5)

The master plan owns the logger's existence. From the TUI's perspective the
non-negotiable contract is:

1. **All log output is captured, not printed**, when `--tui` is on. The logger
   writes to an in-memory ring buffer (e.g. 256 lines × 256 chars = 64 KiB)
   that the TUI renders as a tail panel. Nothing escapes to `std::cout` while
   the TUI owns the terminal.
2. **Severity filtering at the source**: `log::error` from the stratum reader
   (e.g. `stratum_client.cpp` "send_line failed") becomes a red status row,
   not a stray line. `log::info` ("share accepted") updates the snapshot, not
   the console.
3. **Color policy centralizes here**: the logger honors the same
   `use_color_`/`NO_COLOR` rule as the TUI (§2.4), so log lines routed to a
   non-TUI console get ANSI only when appropriate.
4. **When `--tui` is off**, the logger's sink is `std::cerr` (errors/warnings)
   and `std::cout` (info) with a single mutex — exactly the consolidated
   output the master plan calls for.

```cpp
// include/armrx/log.hpp (sketch)
namespace armrx::log {
enum class Level { trace, debug, info, warn, error };

void init(bool use_color, bool tui_mode);   // call once at startup
void set_level(Level min);                   // --log-level=
void sink(Level, std::string msg);           // mutex-guarded

// Macro so file:line capture is free at call sites that don't log
#define ARMRX_LOG(lvl, msg) \
    ::armrx::log::sink(::armrx::log::Level::lvl, \
        std::string{} + "[" #lvl "] " + (msg))
}
```

**Acceptance check (this doc's scope):** with `--tui` on, no `std::cout`/`std::cerr` write from `stratum_client.cpp`, `tls_client.cpp`, `pool_manager.cpp`, `mining_engine.cpp`, or `main.cpp` reaches the terminal directly — it all goes through `log::sink` and into the ring buffer.

**Size:** M. The master plan's item; this doc only pins the TUI contract.

### 3.2 TUI ↔ logger integration (U3)

The race in U3 dissolves once §3.1 lands: the TUI is the only `std::cout`
writer, and it consumes logs via the ring buffer under the logger's mutex.
Specifically:

- `Tui::render()` reads `log::drain_since(last_seq_)` at the top of each
  frame, renders the last N lines in the tail panel, and updates `last_seq__`.
- Share submissions, pool errors, reconnect events arrive as log entries and
  surface in the UI without the main loop passing them as `render()` params.
- The `shares_accepted`/`shares_rejected` counters (U4) become fields on
  `TuiSnapshot` populated by the engine/pool layer, **not** derived from log
  scraping.

**Acceptance check:** run the miner against a mock pool under TSAN with `--tui`
on; zero data-race reports involving `std::cout`.

**Size:** S (on top of §3.1 and §2.1).

---

## 4. Telemetry Surfacing

### 4.1 Share accept/reject state (U4)

Add first-class counters in the pool layer:

```cpp
// include/armrx/pool_manager.hpp (and stratum_client.hpp)
struct ShareStats {
    std::uint64_t submitted = 0;
    std::uint64_t accepted  = 0;
    std::uint64_t rejected  = 0;
    std::uint64_t last_share_job_id_hash = 0;   // for "stale job" detection
};
ShareStats share_stats() const;
```

Wire `stratum_client.cpp:616–618` (currently a substring match on
`"result":true`) to increment `accepted`/`rejected` based on the parsed reply.
The fragile substring match itself is flagged in `next_phase_v2.md` §3.4 as
part of the JSON parser overhaul — the counter wiring lands on top of whatever
parser fix goes in, so order is: parser fix first, then counters.

Pair with a `--reject-log` flag (default off) that `log::info`s each rejection
with the pool's `error` field so the operator can diagnose
(stale/invalid-difficulty/duplicate-nonce).

**Acceptance check:** against a mock pool that returns `{"result":false,"error":"Invalid nonce"}` for every submit, the TUI shows `Shares: 0✓ / 47✗` instead of the current misleading `Shares: 47`.

**Size:** M. Depends on `next_phase_v2.md` Phase 1.3 (JSON parser fix).

### 4.2 Pool latency and last-share time (U13)

Time `mining.submit` round-trips in `stratum_client.cpp`:
record `submit_sent_at = steady_clock::now()` when sending, compute
`reply_recv_at - submit_sent_at` in `handle_reply`. Expose as
`ShareStats::last_submit_latency_ms` and a 1-minute EMA
`submit_latency_ema_ms`. Surface both in the TUI as a `Pool: 142ms (EMA 128ms)`
row. Track `last_share_time` separately (when a *found* share was submitted,
regardless of accept/reject) to support "no share in N minutes" visual
warnings (turn the status amber if `now - last_share_time > 5 min` while
`is_connected()`).

**Acceptance check:** against a real pool, the latency readout tracks network
conditions; throttling the link with `tc qdisc add ... netem delay 500ms`
moves the number as expected.

**Size:** S.

### 4.3 Benchmark-mode TUI parity (U5)

The `--mine` branch (`main.cpp:305–367`) uses a single-line `\r`-overwrite
status and explicitly skips the TUI ("TUI not supported in benchmark mode").
This is an arbitrary restriction — the same `TuiSnapshot` works for both
paths; `pool_name` becomes `"benchmark"`, `status` is always `mining`, and
the pool-telemetry fields are zeroed.

Refactor `main.cpp` so both branches build a `TuiSnapshot` and call
`tui->render(snap)` when `--tui` is set. The benchmark branch's per-second
`std::cout` block (`:346–349`) is deleted; its data moves into the snapshot.

**Acceptance check:** `armrx --mine --tui --seconds=10` renders the dashboard
with worker bars even in benchmark mode.

**Size:** S. Depends on §2.1.

### 4.4 Remove `[DEBUG]` and wallet-prefix leak (U6)

Delete `main.cpp:379–380` outright. The connection attempt is already
announced by the stratum client's own `log::info`. If an operator genuinely
needs the wallet prefix for debugging, expose it behind `--log-level=debug`
via the logger — never at default verbosity, and never the full wallet.

**Acceptance check:** `armrx --pool=... --wallet=45... 2>&1 | grep wallet` returns nothing at default verbosity.

**Size:** XS. Pure deletion.

---

## 5. CLI and Config Ergonomics

### 5.1 Numeric argument validation (U1 in `next_phase_v2.md` §4.2 #8 — TUI-lens)

The `std::stoul`/`std::stoull` throws are owned by the master plan. From this
doc's lens, the validation needs to produce a *usable* error that matches the
flag style:

```
armrx: invalid value 'abc' for --workers (expected positive integer)
Try 'armrx --help' for usage.
```

Wrap each numeric parse in a small `parse_uint<T>(str_view flag, str_view val)`
helper that catches `std::invalid_argument`/`std::out_of_range` and emits the
formatted message + `return 64` (sysexits.h `EX_USAGE`). Exit code 64 is
already the convention at `main.cpp:130,373,376,252`.

**Acceptance check:** `armrx --workers=abc` exits 64 with a one-line message;
no stack-trace/uncaught-exception noise.

**Size:** S.

### 5.2 Add `--version` and build identification (U8)

Embed git SHA + build date via CMake:

```cmake
# CMakeLists.txt — after project()
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE ARMRX_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(TIMESTAMP ARMRX_BUILD_DATE "%Y-%m-%d" UTC)
target_compile_definitions(armrx_core PRIVATE
    ARMRX_VERSION="${PROJECT_VERSION}"
    ARMRX_GIT_SHA="${ARMRX_GIT_SHA}"
    ARMRX_BUILD_DATE="${ARMRX_BUILD_DATE}")
```

```cpp
// main.cpp, in the --help block neighborhood
if (argument == "--version" || argument == "-V") {
    std::cout << "armrx " << ARMRX_VERSION
              << " (" << ARMRX_GIT_SHA << ", built " << ARMRX_BUILD_DATE << ")\n"
              << "  AArch64 JIT: "
#ifdef ARMRX_HAVE_JIT
              << "enabled"
#else
              << "disabled (interpreted VM only)"
#endif
              << "\n  TLS: "
#ifdef ARMRX_HAVE_TLS
              << "enabled"
#else
              << "disabled"
#endif
              << "\n";
    return 0;
}
```

The JIT/TLS build flags are the two a remote operator most often needs to
verify on a device.

**Acceptance check:** `armrx --version` prints one line + two capability lines;
`armrx --help` mentions `--version`.

**Size:** S.

### 5.3 Delete the dead config parser, consolidate flag handling (U9)

`config.cpp:107–136` (`apply_cli_overrides`) is never called and has diverged
from the real parser in `main.cpp` (missing `--rt-priority`, `--mlock`,
`--no-verify-tls`, `--init-cache`, `--config`, `--help`, `--version`). Two
parsers that disagree is worse than one.

Two acceptable resolutions; pick based on appetite:

**Option A (smaller):** delete `apply_cli_overrides`. The `main.cpp` parser is
the only one. Add a comment at `config.hpp:37` saying CLI parsing lives in
`main.cpp` for discoverability.

**Option B (cleaner, larger):** move the `main.cpp:114–253` parser *into*
`config.cpp` as `parse_args(AppConfig defaults, int argc, char** argv)` returning
a struct with both the resolved `AppConfig` and the action flags
(`should_mine`, `should_connect_pool`, `should_init_cache`, `use_tui`, …).
`main.cpp` becomes a thin dispatcher. This also makes the config + CLI unit
testable without `main()` (see §7).

**Recommendation:** Option A now (cheap correctness win), Option B as part of
Phase 2 when the logger refactor reshuffles `main.cpp` anyway.

**Acceptance check (Option A):** `grep -n apply_cli_overrides src/ include/` returns no matches after deletion; full test suite still green.

**Size:** XS (A) / M (B).

### 5.4 Config dry-run / validation (U10)

Add `--check-config` that loads config, applies CLI overrides, and validates
without starting anything:

```cpp
// Validate pool addresses are non-empty and ports are in 1..65535.
// Validate wallet looks Monero-shaped (length 95, starts with 4 or 8 for mainnet).
// Validate mode is one of {auto, light, fast}.
// Validate workers >= 1.
// Print: "config OK: 2 pool(s), 8 workers, mode=auto (selected: light, 256 MiB)"
// Exit 0 on success, 64 with a list of problems on failure.
```

The Monero wallet shape check is heuristic (regex `^[48][0-9A-Za-z]{94}$`) —
it catches typos, not adversarial input. Document it as such.

**Acceptance check:** `armrx --check-config --pool=example.com:notaport --wallet=short` exits 64 with a two-line problem list.

**Size:** S. Depends on §5.3 (Option A or B).

### 5.5 `--config` should be documented and discoverable

`--config=<path>` is parsed (`main.cpp:93–96`) but **not listed in `--help`**
(`:223–247`). Add it. Also add a `--config` (no `=`) form that takes the next
argv, to match `--init-cache`'s style and POSIX conventions.

**Acceptance check:** `armrx --help | grep -- --config` shows the flag and its default-search behavior.

**Size:** XS.

---

## 6. Observability: Prometheus Surface

The endpoint itself is `next_phase_v2.md` Phase 3.3. This section fixes *what
to expose*. The metric set is deliberately small and matches what an operator
actually graphs:

| Metric | Type | Source |
|---|---|---|
| `armrx_hashrate_total_hps` | gauge | `MiningEngine::hash_rate()` |
| `armrx_hashrate_worker_hps{worker=N}` | gauge | `worker_hash_rate(N)` |
| `armrx_hashes_total` | counter | `total_hashes()` |
| `armrx_shares_total{result=accepted\|rejected}` | counter | §4.1 `ShareStats` |
| `armrx_pool_connected` | gauge (0/1) | `PoolManager::is_connected()` |
| `armrx_pool_reconnect_attempts` | gauge | `reconnect_attempts()` |
| `armrx_pool_submit_latency_seconds` | gauge (EMA) | §4.2 latency tracking |
| `armrx_uptime_seconds` | gauge | main loop `elapsed_sec` |
| `armrx_jit_compile_seconds_total` | counter | `total_jit_compile_time_ns()` (only with `ARMRX_JIT_PROFILE`) |
| `armrx_jit_execute_seconds_total` | counter | `total_jit_execute_time_ns()` (only with `ARMRX_JIT_PROFILE`) |

Endpoint: `GET /metrics` on `127.0.0.1:9100` by default (`--metrics-host`,
`--metrics-port` to override). Plain text, Prometheus exposition format. No
dependencies — a 60-line HTTP/0.9 responder is enough (`write(2)` a fixed
header + body, ignore the request line). Bind to loopback only by default.

**Acceptance check:** `curl -s localhost:9100/metrics | grep armrix_shares_total` returns the counter with both `result` labels after a share round-trip.

**Size:** M. Depends on §4.1 (share stats) and the logger landing (so metrics share the consolidated state-read paths, not a third copy of the counters).

---

## 7. Testing Strategy

The TUI is hard to unit-test (terminal output), but the surrounding contracts
are not. Targets:

- **`tests/test_config.cpp`** — feed synthetic JSON config strings; assert
  `AppConfig` fields parse correctly. Catches the `get_string`/`get_raw`
  confusion (e.g. `tui_str = get_raw(...)` returns `"true"` with quotes
  stripped, but `wallet_str = get_string(...)` returns the string content —
  the asymmetry is a latent bug). Catches U10 regressions.
- **`tests/test_cli_parse.cpp`** — requires §5.3 Option B (parser lifted out
  of `main`). Assert that `--workers=abc` exits 64, `--pool=host:notaport`
  fails validation, `--mode=bogus` exits 64.
- **`tests/test_share_stats.cpp`** — feed canned `{"result":true}` and
  `{"result":false,"error":"..."}` frames into a mock `StratumClient`; assert
  `ShareStats` counters update correctly. Catches U4 regressions. Requires the
  JSON parser fix from `next_phase_v2.md` Phase 1.3.
- **`tests/test_tui_snapshot.cpp`** — construct a `TuiSnapshot` with known
  fields, render to a `std::ostringstream`-backed sink (requires the renderer
  to take an output stream, not hardcoded `std::cout` — fold this into §2.1).
  Assert that the rendered frame contains the expected substrings ("Total:",
  "Shares:", worker labels) and that line count matches `prev_lines_`. Catches
  U2 regressions deterministically without a real terminal.
- **Snapshot/golden test**: render a fixed `TuiSnapshot` and compare bytes
  against a committed `tests/golden/tui_basic.txt`. Update via a `--update`
  flag. Catches accidental layout drift.

The signal-safe teardown (§2.5) is not unit-testable; verify by hand per the
§2.5 acceptance check and add a note to the release checklist.

---

## 8. Phased Action Plan

Sequenced to respect dependencies on `next_phase_v2.md` and to keep each phase
independently shippable.

### Phase U1 — TUI foundations (1 week; runs in parallel with master Phase 1)

| # | Item | Depends on | Size |
|---|---|---|---|
| U1.1 | `TuiSnapshot` struct + refactor `render()` to take it (§2.1) | — | M |
| U1.2 | Renderer writes to an injectable `std::ostream&`, not hardcoded `std::cout` (§7) | U1.1 | S |
| U1.3 | Terminal-width awareness + pool-name truncation + bar scaling (§2.3) | U1.1 | S |
| U1.4 | `NO_COLOR` / `--no-color` / `--color` / `isatty` policy (§2.4) | U1.1 | S |
| U1.5 | Worker-bar EMA baseline (§2.2) | U1.1 | S |
| U1.6 | `atexit` + minimal SIGTERM cursor-show (§2.5) | `next_phase_v2.md` P1.4 SIGTERM | S |
| U1.7 | Delete `[DEBUG]` wallet-prefix leak (§4.4) | — | XS |
| U1.8 | `test_tui_snapshot.cpp` golden test (§7) | U1.1, U1.2 | S |

**Exit criteria:** TUI renders correctly in 40/80/200-column terminals; monochrome mode produces no `\033`; `kill -TERM` leaves a clean terminal; golden test green.

### Phase U2 — Output discipline (runs after master Phase 2.3 logger lands)

| # | Item | Depends on | Size |
|---|---|---|---|
| U2.1 | Route all `std::cerr`/`std::cout` in stratum/tls/pool/mining/config/main through `log::sink` (§3.1) | master P2.3 logger | M |
| U2.2 | TUI ring-buffer tail panel consuming `log::drain_since()` (§3.2) | U2.1, U1.1 | S |
| U2.3 | TSAN-clean `--tui` integration test against a mock pool (§3.2 acceptance) | U2.1 | S |

**Exit criteria:** zero `std::cout`/`std::cerr` writes outside `log.hpp` and `tui.cpp`'s stream; TSAN clean under `--tui`.

### Phase U3 — Telemetry and CLI (runs after master Phase 1.3 parser fix)

| # | Item | Depends on | Size |
|---|---|---|---|
| U3.1 | `ShareStats` counters wired from `handle_reply` (§4.1) | master P1.3 parser fix | M |
| U3.2 | Submit-latency timing + EMA (§4.2) | U3.1 | S |
| U3.3 | Benchmark-mode TUI parity (§4.3) | U1.1 | S |
| U3.4 | Numeric-arg validation helper + `EX_USAGE` formatting (§5.1) | — | S |
| U3.5 | `--version` with git SHA + build flags (§5.2) | — | S |
| U3.6 | Document `--config` in `--help`; add `--config <path>` form (§5.5) | — | XS |
| U3.7 | Delete dead `apply_cli_overrides` (§5.3 Option A) | — | XS |
| U3.8 | `--check-config` validation (§5.4) | U3.7 | S |
| U3.9 | `test_config.cpp`, `test_share_stats.cpp` (§7) | U3.1, U3.7 | S |

**Exit criteria:** share accept/reject visible in TUI; `--workers=abc` produces a clean usage error; `--version` prints git SHA; `--check-config` validates a config file without mining.

### Phase U4 — Observability and parser consolidation (after master Phase 2)

| # | Item | Depends on | Size |
|---|---|---|---|
| U4.1 | Prometheus `/metrics` endpoint with the §6 metric set | U3.1, master P2.3 logger | M |
| U4.2 | Lift `main.cpp` parser into `parse_args()` (§5.3 Option B) | U3.7 | M |
| U4.3 | `test_cli_parse.cpp` against `parse_args` (§7) | U4.2 | S |

**Exit criteria:** `curl localhost:9100/metrics` returns the full metric set; CLI parsing is unit-tested without `main()`.

---

## 9. Out-of-Scope (deferred unless a forcing function appears)

- **ncurses / full-screen TUI**: the current ANSI approach is 91 LOC and works
  in every terminal. A curses dependency costs portability (postmarketOS /
  musl / termux) for marginal visual gain. Revisit only if the dashboard grows
  past ~25 rows or interactive keybindings become required.
- **Web dashboard**: a separate binary (`armrx-web`) reading the Prometheus
  endpoint is the right architecture; do not bake HTTP serving into the miner
  beyond `/metrics`.
- **Interactive hotkeys** (e.g. `q` to quit, `r` to force reconnect): requires
  raw-mode terminal input and a non-blocking main loop. The current 1 Hz
  `sleep_for` loop is not structured for it. Defer until there's a concrete
  operator request.
- **Per-worker CPU utilization / temperature**: requires either
  `/proc/stat` scraping (racy, slow) or `perf_event_open` privileges. Belongs
  in the Prometheus metric set if added, not the TUI.
- **Windows / non-ANSI console support**: the project targets AArch64 Linux
  explicitly (`AGENTS.md`). Don't carry a `#ifdef _WIN32` fallback for the TUI.

---

## Quick-reference: ROI table

| Change | Effort | Impact | Risk | Phase |
|---|---|---|---|---|
| Delete `[DEBUG]` wallet leak (`main.cpp:379`) | XS | Security/info-leak | None | U1.7 |
| `--config` in `--help` (§5.5) | XS | Discoverability | None | U3.6 |
| Delete dead `apply_cli_overrides` (§5.3 A) | XS | Maintainability | None | U3.7 |
| `--version` with git SHA (§5.2) | S | Ops / remote debugging | None | U3.5 |
| Numeric-arg validation (§5.1) | S | UX (no more uncaught throws) | None | U3.4 |
| Terminal width + truncation (§2.3) | S | Fixes scrollback corruption | Low | U1.3 |
| NO_COLOR / `--no-color` (§2.4) | S | Accessibility / piped output | Low | U1.4 |
| Signal-safe cursor restore (§2.5) | S | UX (no hidden cursor after kill) | Low | U1.6 |
| Bar EMA baseline (§2.2) | S | Readability | None | U1.5 |
| `TuiSnapshot` refactor (§2.1) | M | Unblocks everything in §2/§4 | Low | U1.1 |
| Share accept/reject (§4.1) | M | Fixes misleading metric | Needs parser fix | U3.1 |
| Benchmark-mode TUI parity (§4.3) | S | UX consistency | Low | U3.3 |
| `--check-config` (§5.4) | S | Catches config typos early | None | U3.8 |
| Prometheus `/metrics` (§6) | M | Operator dashboards | Loopback-only | U4.1 |
| Logger + TUI ring buffer (§3) | M | Fixes U3 race, unblocks §6 | Coordinated with master P2.3 | U2 |

**Non-negotiables before anything else here lands:** the `TuiSnapshot` refactor
(U1.1) is the prerequisite for every other §2 item, and the master plan's
logger (Phase 2.3) is the prerequisite for §3. Everything else is composable
in any order.
