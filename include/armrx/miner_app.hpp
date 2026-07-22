#pragma once

#include "armrx/cli_parser.hpp"
#include "armrx/randomx_config.hpp"

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
};

} // namespace armrx
