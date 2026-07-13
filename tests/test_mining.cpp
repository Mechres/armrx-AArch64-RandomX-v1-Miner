#include "armrx/mining_engine.hpp"
#include "armrx/mining_common.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>

void test_target_comparison() {
    armrx::Target target1;
    std::fill(target1.bytes.begin(), target1.bytes.end(), std::byte{0xff}); // Maximum value

    std::array<std::byte, 32> hash1{};
    std::fill(hash1.begin(), hash1.end(), std::byte{0x7f});

    assert(armrx::meets_target(hash1, target1));

    armrx::Target target2;
    std::fill(target2.bytes.begin(), target2.bytes.end(), std::byte{0x00});
    target2.bytes[0] = std::byte{0x01}; // target is 1

    std::array<std::byte, 32> hash2{};
    std::fill(hash2.begin(), hash2.end(), std::byte{0x00});
    assert(armrx::meets_target(hash2, target2)); // 0 <= 1

    hash2[0] = std::byte{0x02};
    assert(!armrx::meets_target(hash2, target2)); // 2 > 1
    
    // Test boundary: most significant byte
    armrx::Target target3;
    std::fill(target3.bytes.begin(), target3.bytes.end(), std::byte{0x00});
    target3.bytes[31] = std::byte{0x10};

    std::array<std::byte, 32> hash3{};
    std::fill(hash3.begin(), hash3.end(), std::byte{0x00});
    hash3[31] = std::byte{0x0f};
    assert(armrx::meets_target(hash3, target3)); // 0x0f < 0x10

    hash3[31] = std::byte{0x11};
    assert(!armrx::meets_target(hash3, target3)); // 0x11 > 0x10

    std::cout << "[test_mining] test_target_comparison passed!\n";
}

void test_mining_engine_lifecycle() {
    // Initialize light mode engine with 2 threads
    armrx::MiningEngine engine(armrx::RandomXMode::light, 2);

    armrx::Job job;
    job.job_id = "test_job_1";
    job.block_template = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    job.nonce_offset = 3;
    job.nonce_size = 4;
    job.seed_key = {std::byte{0x74}, std::byte{0x65}, std::byte{0x73}, std::byte{0x74}}; // "test"
    
    // Set target to max so that any calculated hash meets it
    std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xff});

    std::atomic<int> share_count{0};
    engine.set_job(job);

    engine.start([&share_count](const armrx::Job&, std::uint64_t, std::array<std::byte, 32>) {
        share_count.fetch_add(1, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    engine.stop();

    assert(engine.total_hashes() > 0);
    assert(share_count.load() > 0);
    std::cout << "[test_mining] test_mining_engine_lifecycle passed with " 
              << engine.total_hashes() << " hashes and " << share_count.load() << " shares!\n";
}

int main() {
    test_target_comparison();
    test_mining_engine_lifecycle();
    std::cout << "ALL MINING TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
