#include "armrx/cli_parser.hpp"
#include "armrx/miner_app.hpp"

int main(int argc, char** argv) {
    auto parsed = armrx::CommandLineParser::parse(argc, argv);
    if (parsed.should_exit) {
        return parsed.exit_code;
    }

    armrx::MinerApp app(std::move(parsed.options));
    return app.run();
}
