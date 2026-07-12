#include "armrx/aes.hpp"
#include "armrx/aes_generator.hpp"
#include "armrx/argon2.hpp"
#include "armrx/dataset.hpp"
#include "armrx/blake2b.hpp"
#include "armrx/memory.hpp"

#include <array>
#include <algorithm>
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

std::string hex(std::span<const std::byte> bytes) {
    std::ostringstream result;
    for (const auto byte : bytes) {
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
    assert(hex(armrx::blake2b(empty, 32)) ==
           "0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8");
    const auto hprime = armrx::argon2_hprime(empty, 1024);
    assert(hprime.size() == 1024U);
    assert(hprime == armrx::argon2_hprime(empty, 1024));
    armrx::Argon2Block zero_block{};
    assert(armrx::argon2_compress(zero_block, zero_block) == zero_block);
    armrx::Argon2Block input_block{};
    input_block[0] = 1;
    const auto transformed = armrx::argon2_compress(input_block, zero_block);
    assert(transformed != zero_block);
    assert(transformed == armrx::argon2_compress(input_block, zero_block));
    constexpr std::array<std::byte, 3> cache_key{std::byte{'k'}, std::byte{'e'}, std::byte{'y'}};
    armrx::Argon2dCache tiny_cache{8, 1};
    tiny_cache.initialize(cache_key);
    assert(tiny_cache.blocks().size() == 8U);
    assert(tiny_cache.blocks()[0] != zero_block);
    constexpr auto mib = 1024ULL * 1024ULL;
    const auto small = armrx::choose_randomx_mode(2ULL * 1024ULL * mib, 1);
    assert(small.mode == armrx::RandomXMode::light);
    const auto ample = armrx::choose_randomx_mode(3ULL * 1024ULL * mib, 4);
    assert(ample.mode == armrx::RandomXMode::fast);

    assert(armrx::randomx_dataset_item_count() == 34078720ULL);
    const auto seed0 = armrx::dataset_seed_registers(0);
    assert(seed0[0] == 6364136223846793005ULL);
    assert(seed0[1] == 15662221698380390097ULL);
    assert(seed0[2] == 18384096042004490091ULL);
    assert(seed0[3] == 15670078305396536945ULL);
    assert(seed0[4] == 1233090996314240335ULL);
    assert(seed0[5] == 14584374767940244257ULL);
    assert(seed0[6] == 8609653616329052757ULL);
    assert(seed0[7] == 15912571922823092835ULL);

    armrx::Argon2dCache dataset_cache{8, 1};
    constexpr std::array<std::byte, 3> dataset_key{std::byte{'k'}, std::byte{'e'}, std::byte{'y'}};
    dataset_cache.initialize(dataset_key);
    const auto item0 = armrx::generate_dataset_item(dataset_cache, 0);
    const auto item1 = armrx::generate_dataset_item(dataset_cache, 1);
    assert(item0.size() == armrx::kRandomXDatasetItemBytes);
    assert(item0 != item1);
    assert(item0 != armrx::DatasetItem{});

    std::array<std::byte, armrx::kRandomXDatasetItemBytes * 2U> dataset_output{};
    armrx::initialize_dataset(dataset_output, dataset_cache, 0, 2);
    assert(std::equal(dataset_output.begin(),
                      dataset_output.begin() + static_cast<std::ptrdiff_t>(armrx::kRandomXDatasetItemBytes),
                      item0.begin(), item0.end()));
    assert(std::equal(dataset_output.begin() + static_cast<std::ptrdiff_t>(armrx::kRandomXDatasetItemBytes),
                      dataset_output.end(), item1.begin(), item1.end()));

    std::array<std::byte, armrx::kRandomXDatasetItemBytes * 3U> dataset_output_offset{};
    armrx::initialize_dataset(dataset_output_offset, dataset_cache, 5, 3);
    assert(armrx::dataset_output_bytes(3) == armrx::kRandomXDatasetItemBytes * 3U);
    const auto item5 = armrx::generate_dataset_item(dataset_cache, 5);
    const auto item6 = armrx::generate_dataset_item(dataset_cache, 6);
    assert(std::equal(dataset_output_offset.begin(),
                      dataset_output_offset.begin() + static_cast<std::ptrdiff_t>(armrx::kRandomXDatasetItemBytes),
                      item5.begin(), item5.end()));
    assert(std::equal(dataset_output_offset.begin() + static_cast<std::ptrdiff_t>(armrx::kRandomXDatasetItemBytes),
                      dataset_output_offset.begin() + static_cast<std::ptrdiff_t>(armrx::kRandomXDatasetItemBytes * 2U),
                      item6.begin(), item6.end()));

    // FIPS-197 AES-128 known-answer test: state after its initial AddRoundKey,
    // then the first encryption round with round key 1.
    constexpr armrx::AesBlock initial{
        std::byte{0x00}, std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
        std::byte{0x40}, std::byte{0x50}, std::byte{0x60}, std::byte{0x70},
        std::byte{0x80}, std::byte{0x90}, std::byte{0xa0}, std::byte{0xb0},
        std::byte{0xc0}, std::byte{0xd0}, std::byte{0xe0}, std::byte{0xf0}};
    constexpr armrx::AesBlock round_key{
        std::byte{0xd6}, std::byte{0xaa}, std::byte{0x74}, std::byte{0xfd},
        std::byte{0xd2}, std::byte{0xaf}, std::byte{0x72}, std::byte{0xfa},
        std::byte{0xda}, std::byte{0xa6}, std::byte{0x78}, std::byte{0xf1},
        std::byte{0xd6}, std::byte{0xab}, std::byte{0x76}, std::byte{0xfe}};
    constexpr armrx::AesBlock expected{
        std::byte{0x89}, std::byte{0xd8}, std::byte{0x10}, std::byte{0xe8},
        std::byte{0x85}, std::byte{0x5a}, std::byte{0xce}, std::byte{0x68},
        std::byte{0x2d}, std::byte{0x18}, std::byte{0x43}, std::byte{0xd8},
        std::byte{0xcb}, std::byte{0x12}, std::byte{0x8f}, std::byte{0xe4}};
    const auto encrypted = armrx::aes_encrypt_round(initial, round_key);
    assert(encrypted == expected);
    assert(armrx::aes_decrypt_round(encrypted, round_key) == initial);

    armrx::AesState seed{};
    armrx::AesGenerator1R single_step{seed};
    armrx::AesGenerator1R buffered{seed};
    const auto generated = single_step.next();
    std::array<std::byte, 64> output{};
    buffered.fill(output);
    assert(generated == output);
    assert(buffered.state() == output);
    assert(generated != seed);

    armrx::AesGenerator4R secure_step{seed};
    armrx::AesGenerator4R secure_buffered{seed};
    const auto secure_generated = secure_step.next();
    secure_buffered.fill(output);
    assert(secure_generated == output);
    assert(secure_buffered.state() == output);
    assert(secure_generated != seed);
    return 0;
}
