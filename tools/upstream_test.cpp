#include "randomx.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstring>
#include <array>

int main() {
    const char key[] = "test key 000";
    const char input1[] = "This is a test";
    const char input2[] = "Lorem ipsum dolor sit amet";

    // Force reference Argon2 (no SSE/AVX optimizations) to match AArch64
    // On the host, the upstream library was compiled with SSE/AVX support.
    // We need to explicitly select the reference implementation.
    // The flags for cache: just JIT (so we get the reference Argon2 impl)
    randomx_flags flags = RANDOMX_FLAG_DEFAULT;
    std::cout << "flags=0x" << std::hex << static_cast<unsigned>(flags) << std::dec << std::endl;

    randomx_cache* cache = randomx_alloc_cache(flags);
    if (!cache) { std::cerr << "alloc_cache failed\n"; return 1; }
    randomx_init_cache(cache, key, sizeof(key)-1);

    randomx_vm* vm = randomx_create_vm(flags, cache, nullptr);
    if (!vm) { std::cerr << "create_vm failed\n"; return 1; }

    std::array<char, 32> hash1, hash2;
    randomx_calculate_hash(vm, input1, sizeof(input1)-1, hash1.data());
    randomx_calculate_hash(vm, input2, sizeof(input2)-1, hash2.data());

    auto hex = [](const std::array<char, 32>& h) -> std::string {
        std::ostringstream os;
        for (auto b : h) os << std::hex << std::setw(2) << std::setfill('0')
                            << (static_cast<unsigned>(static_cast<unsigned char>(b)) & 0xFFu);
        return os.str();
    };

    std::cout << "Upstream Input1: " << hex(hash1) << std::endl;
    std::cout << "Upstream Input2: " << hex(hash2) << std::endl;

    randomx_destroy_vm(vm);
    randomx_release_cache(cache);
    return 0;
}
