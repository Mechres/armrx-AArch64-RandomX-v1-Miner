#pragma once

#include "armrx/cli_parser.hpp"
#include "armrx/randomx_config.hpp"
#include "armrx/partial_dataset.hpp"

#include <memory>

namespace armrx {

// Owns the resolved miner configuration and runs one of: cache-init
// benchmark, JIT dump, local mining benchmark, or pool mining. Installs
// SIGINT/SIGTERM handling so any of the run loops can be stopped cleanly.
class MinerApp {
public:
    explicit MinerApp(MinerOptions options);

    /// Runs the miner according to the resolved options and returns the
    /// process exit code.
    int run();

private:
    void install_signal_handlers();
    void run_init_cache();
#ifdef ARMRX_HAVE_JIT
    int run_jit_dump();
#endif
    void run_local_benchmark(RandomXMode effective_mode);
    void run_pool_mining(RandomXMode effective_mode);

    MinerOptions opts_;
    // Validated item count for the partial dataset (Track B), 0 = disabled.
    // Deliberately NOT a shared PartialDataset instance: run_local_benchmark()
    // and run_pool_mining() are independent mining sessions that can BOTH run
    // in one process invocation (`--mine` and `--pool` are independent flags,
    // see run()), each with its own MiningEngine that calls stop() on exit.
    // PartialDataset::cancel() (called by MiningEngine::stop()) is permanent
    // and one-way by design (see its doc comment) -- sharing one instance
    // across two sequential sessions would leave the second session's
    // engine holding a PartialDataset that can never fill again, silently
    // disabling the optimization with no warning (reviewer-identified gap).
    // Each run_* function that needs one constructs its own fresh instance
    // from this validated count instead.
    std::size_t partial_dataset_items_ = 0;

    // Start snapshot for --pool-test steady-state delta (taken right before
    // the mining loop begins). Only meaningful when opts_.pool_test is set.
    MiningEngine::HashSnapshot pool_test_snap_start_{};
};

} // namespace armrx
