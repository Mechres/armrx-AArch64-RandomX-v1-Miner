// Standalone seed-rotation verifier — runs the SAME logic as
// test_light_mode_seed_rotation_rebuilds_partial_dataset but in a FRESH
// process per seed so no JIT program-cache state is shared between the
// engine's VM and the reference VM (the in-process reference VM in
// test_mining.cpp returned stale hashes, suggesting JIT program reuse across
// VirtualMachine instances in one process — a harness artifact, not a product
// bug). Usage:
//   verify_seed_rotation <mode> <nonce> <seed_hex> <partial_items>
//   mode = engine  -> run MiningEngine, print job2 hash for nonce
//   mode = ref     -> run a fresh light-mode VM with the given seed, print hash
// The test driver compares engine output vs ref output for seed-B across
// separate processes.
#include "armrx/mining_engine.hpp"
#include "armrx/mining_common.hpp"
#include "armrx/dataset.hpp"
#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <chrono>
#include <atomic>
#include <mutex>

static std::vector<std::byte> hex_to_bytes(const char* s) {
    std::vector<std::byte> out;
    for (size_t i = 0; s[i] && s[i+1]; i += 2) {
        unsigned v;
        std::sscanf(s + i, "%02x", &v);
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

static int run_engine(const char* seed_hex, std::uint64_t nonce, std::size_t partial_items, bool rotate) {
    std::vector<std::byte> seed = hex_to_bytes(seed_hex);
    armrx::MiningEngine engine(armrx::RandomXMode::light, 2);
    auto pd = std::make_shared<armrx::PartialDataset>(partial_items);
    engine.set_partial_dataset(pd);

    auto make_job = [&](const std::string& id, const std::vector<std::byte>& sk) {
        armrx::Job job;
        job.job_id = id;
        job.block_template.assign(76, std::byte{0});
        job.nonce_offset = 39;
        job.nonce_size = 4;
        job.seed_key = sk;
        std::fill(job.target.bytes.begin(), job.target.bytes.end(), std::byte{0xff});
        return job;
    };
    // Two distinct seeds so the engine actually rotates.
    std::vector<std::byte> seedA{std::byte{'s'}, std::byte{'e'}, std::byte{'e'}, std::byte{'d'},
                                  std::byte{'A'}, std::byte{'A'}, std::byte{'A'}, std::byte{'A'}};
    auto job1 = make_job("job1", seedA);
    auto job2 = make_job("job2", seed); // the target seed

    std::mutex m;
    std::vector<std::pair<std::string,std::pair<std::uint64_t,std::array<std::byte,32>>>> found;
    engine.set_job(rotate ? job1 : job2);
    engine.start([&](const armrx::Job& j, std::uint64_t n, std::array<std::byte,32> h){
        if (n != nonce) return;
        std::lock_guard<std::mutex> lk(m); found.emplace_back(j.job_id, std::make_pair(n,h));
    });
    // wait for first fill
    auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!pd->fill_complete() && std::chrono::steady_clock::now() < dl)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    // rotate (only in rotate mode)
    if (rotate) engine.set_job(job2);
    // collect job1 AND job2 samples
    std::vector<std::pair<std::uint64_t,std::array<std::byte,32>>> s1, s2;
    auto dl2 = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (s2.empty() && std::chrono::steady_clock::now() < dl2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lk(m);
        for (auto& p : found) {
            if (p.first == "job1") s1.push_back(p.second);
            else if (p.first == "job2") s2.push_back(p.second);
        }
    }
    engine.stop();
    auto hex = [](const std::array<std::byte,32>& h){
        static char buf[65]; static const char* d="0123456789abcdef";
        for (size_t i=0;i<32;++i){buf[2*i]=d[(unsigned char)h[i]>>4];buf[2*i+1]=d[(unsigned char)h[i]&0xf];}
        buf[64]=0; return buf;
    };
    for (auto& [n,h] : s1) printf("JOB1_NONCE=%llu HASH=%s\n", (unsigned long long)n, hex(h));
    for (auto& [n,h] : s2) printf("ENGINE_NONCE=%llu HASH=%s\n", (unsigned long long)n, hex(h));
    return 0;
}

static int run_ref(const char* seed_hex, std::uint64_t nonce) {
    std::vector<std::byte> seed = hex_to_bytes(seed_hex);
    armrx::Argon2dCache cache;
    cache.initialize(seed);
    std::uint32_t flags = armrx::kRandOMXFlagHardAes;
#ifdef ARMRX_HAVE_JIT
    flags |= armrx::kRandOMXFlagJit;
#endif
    armrx::VirtualMachine vm(flags);
    vm.set_cache(&cache);
    std::vector<std::byte> block(76, std::byte{0});
    for (size_t i=0;i<4;++i) block[39+i] = static_cast<std::byte>((nonce>>(8*i))&0xff);
    std::array<std::byte,32> h{};
    armrx::randomx_calculate_hash(&vm, block.data(), block.size(), h.data());
    char buf[65]; static const char* d="0123456789abcdef";
    for (size_t i=0;i<32;++i){buf[2*i]=d[(unsigned char)h[i]>>4];buf[2*i+1]=d[(unsigned char)h[i]&0xf];}
    buf[64]=0; printf("REF_NONCE=%llu HASH=%s\n", (unsigned long long)nonce, buf);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <engine|ref|engine-norotate> <nonce> <seed_hex> [partial_items]\n", argv[0]); return 2; }
    std::uint64_t nonce = std::strtoull(argv[2], nullptr, 10);
    std::string mode = argv[1];
    bool rotate = (mode == "engine");
    if (mode == "engine" || mode == "engine-norotate") {
        std::size_t pi = argc > 4 ? std::strtoull(argv[4],nullptr,10) : 65536;
        return run_engine(argv[3], nonce, pi, rotate);
    } else if (mode == "ref") {
        return run_ref(argv[3], nonce);
    }
    fprintf(stderr, "unknown mode %s\n", argv[1]); return 2;
}
