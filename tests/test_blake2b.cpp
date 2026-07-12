#include "armrx/blake2b.hpp"
#include "armrx/memory.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

std::string hex(const armrx::Hash512& hash) {
    std::ostringstream result;
    for (const auto byte : hash) {
        result << std::hex << std::setw(2) << std::setfill('0')
               << std::to_integer<unsigned>(byte);
    }
    return result.str();
}

} // namespace

int main() {
    constexpr std::array<std::byte, 0> empty{};
    assert(hex(armrx::blake2b_512(empty)) ==
           "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
           "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");
    constexpr auto mib = 1024ULL * 1024ULL;
    const auto small = armrx::choose_randomx_mode(2ULL * 1024ULL * mib, 1);
    assert(small.mode == armrx::RandomXMode::light);
    const auto ample = armrx::choose_randomx_mode(3ULL * 1024ULL * mib, 4);
    assert(ample.mode == armrx::RandomXMode::fast);
    return 0;
}
