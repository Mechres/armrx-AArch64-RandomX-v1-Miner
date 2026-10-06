#include "armrx/cpu_thermal.hpp"
#include <iostream>
#include <chrono>

using namespace armrx;

int main() {
    auto start = std::chrono::high_resolution_clock::now();
    size_t total_zones = 0;
    for (int i = 0; i < 1000; ++i) {
        auto zones = read_cpu_temperatures();
        total_zones += zones.size();
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "Time: " << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() << " us" << std::endl;
    std::cout << "Total zones read: " << total_zones << std::endl;
    return 0;
}
