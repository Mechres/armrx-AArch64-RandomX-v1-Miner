// Regression tests for PLAN.md Phase 4 item E.2: fill_aes_1r_x4/fill_aes_4r_x4/
// hash_aes_1r_x4/hash_and_fill_aes_1r_x4 (src/aes_hash.cpp) previously had no
// direct unit test — only indirect coverage via full end-to-end RandomX KAT
// hashes, which prove the whole pipeline is correct but don't localize a
// regression to this specific file if one of these four functions breaks.
//
// Two kinds of checks:
//  1. Golden output pins, captured from the current (KAT-verified-correct)
//     implementation, the same "pin known-good bytes" pattern already used
//     for Argon2 reference dataset items elsewhere in this test suite.
//  2. Structural invariants these functions must hold regardless of the
//     exact AES round constants: determinism, output-prefix consistency
//     across output-buffer sizes, and — most importantly for
//     hash_and_fill_aes_1r_x4 — that its "combined" hash+fill in one pass
//     produces exactly the same results as calling hash_aes_1r_x4() and
//     fill_aes_1r_x4() separately on the same inputs. That decomposition
//     equivalence is the actual contract the combined function exists to
//     provide (it's a hot-path fusion of the other two), and nothing else
//     in the test suite pins it down directly.

#include "armrx/aes_hash.hpp"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace {

armrx::AesState zero_state() {
    armrx::AesState s{};
    s.fill(std::byte{0});
    return s;
}

std::vector<std::byte> zero_buffer(std::size_t n) {
    return std::vector<std::byte>(n, std::byte{0});
}

// Fills a buffer with a simple, non-degenerate repeating pattern (not all
// zero) so tests exercise more than the AES round constants XORed with zero.
std::vector<std::byte> pattern_buffer(std::size_t n) {
    std::vector<std::byte> buf(n);
    for (std::size_t i = 0; i < n; ++i)
        buf[i] = static_cast<std::byte>((i * 37 + 11) & 0xFF);
    return buf;
}

std::string to_hex(const std::byte* data, std::size_t n) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < n; ++i)
        oss << std::setw(2) << static_cast<int>(std::to_integer<unsigned char>(data[i]));
    return oss.str();
}

std::string to_hex(const armrx::AesState& s) { return to_hex(s.data(), s.size()); }
std::string to_hex(const std::vector<std::byte>& v) { return to_hex(v.data(), v.size()); }

} // namespace

void test_fill_aes_1r_x4_golden_and_determinism() {
    // Golden: zero initial state, 128-byte output.
    auto state = zero_state();
    auto out = zero_buffer(128);
    armrx::fill_aes_1r_x4(state, out);

    const std::string got_hex = to_hex(out);
    // Captured from this implementation with a plain zero AesState/output
    // buffer (KAT-verified correct via the full end-to-end RandomX hash
    // tests in armrx_tests) — pins the exact byte sequence so a future
    // regression here is caught locally instead of only showing up as a
    // full-hash KAT mismatch with no indication of where it came from.
    const std::string expected_hex =
        "01f7fe3f5b3423307907e789451ba6e664cc1f0e6e1209e71bb046742dbfc26e"
        "a330406d942cc6cd1d2b92a617b1726c56e28c091f52d9d2eb2f527537f2752a"
        "0e25b95082a0e4fc88cf4960e558d2360414f81e94ffbeb171a3be06ab039d0b"
        "cb149022a2cc6188e44e6b95a3c46d4aa9d56a7344664d6010b55daa60e2c270";
    assert(got_hex == expected_hex);

    // Determinism: same starting state, called again from scratch, must
    // reproduce byte-identical output and final state.
    auto state2 = zero_state();
    auto out2 = zero_buffer(128);
    armrx::fill_aes_1r_x4(state2, out2);
    assert(out == out2);
    assert(state == state2);

    // Prefix consistency: a shorter fill from the same initial state must
    // match the first N bytes of a longer fill — the per-64-byte loop must
    // not depend on the total output size, only on how many blocks it's run.
    auto state3 = zero_state();
    auto out_short = zero_buffer(64);
    armrx::fill_aes_1r_x4(state3, out_short);
    assert(std::memcmp(out_short.data(), out.data(), 64) == 0);

    std::cout << "[test_aes_hash] test_fill_aes_1r_x4_golden_and_determinism passed (output=" << got_hex << ")\n";
}

void test_fill_aes_4r_x4_determinism_and_prefix() {
    auto state = zero_state();
    auto out = pattern_buffer(128); // pre-fill with non-zero to prove it's fully overwritten
    armrx::fill_aes_4r_x4(state, out);

    auto state2 = zero_state();
    auto out2 = zero_buffer(128);
    armrx::fill_aes_4r_x4(state2, out2);
    assert(out == out2); // initial output-buffer contents must not matter
    assert(state == state2);

    auto state3 = zero_state();
    auto out_short = zero_buffer(64);
    armrx::fill_aes_4r_x4(state3, out_short);
    assert(std::memcmp(out_short.data(), out.data(), 64) == 0);

    // fill_aes_1r_x4 and fill_aes_4r_x4 use different round keys/round counts,
    // so from the same zero state they must diverge.
    auto state_1r = zero_state();
    auto out_1r = zero_buffer(64);
    armrx::fill_aes_1r_x4(state_1r, out_1r);
    assert(std::memcmp(out_1r.data(), out.data(), 64) != 0);

    std::cout << "[test_aes_hash] test_fill_aes_4r_x4_determinism_and_prefix passed (output=" << to_hex(out) << ")\n";
}

void test_hash_aes_1r_x4_determinism_and_sensitivity() {
    auto input = pattern_buffer(128);
    armrx::AesState hash{};
    armrx::hash_aes_1r_x4(input, hash);

    armrx::AesState hash2{};
    armrx::hash_aes_1r_x4(input, hash2);
    assert(hash == hash2); // determinism

    // Flipping a single input bit must change the hash (not a security
    // proof, just a sanity check that input actually flows into the state).
    auto input_flipped = input;
    input_flipped[0] ^= std::byte{0x01};
    armrx::AesState hash3{};
    armrx::hash_aes_1r_x4(input_flipped, hash3);
    assert(hash != hash3);

    // Zero-length-equivalent (all-zero) input is a distinct, well-defined case.
    auto zero_input = zero_buffer(128);
    armrx::AesState hash_zero{};
    armrx::hash_aes_1r_x4(zero_input, hash_zero);
    assert(hash_zero != hash);

    std::cout << "[test_aes_hash] test_hash_aes_1r_x4_determinism_and_sensitivity passed (hash=" << to_hex(hash) << ")\n";
}

void test_hash_and_fill_aes_1r_x4_matches_separate_calls() {
    // The core contract of the combined function: given the SAME initial
    // scratchpad content and the SAME initial fill_state, it must produce
    // exactly the hash that hash_aes_1r_x4() would compute over the
    // original (pre-overwrite) scratchpad content, and exactly the fill
    // output that fill_aes_1r_x4() would compute from the same fill_state —
    // i.e. it's a correctness-preserving fusion of the two, not just a
    // superficially similar routine.
    const std::size_t n = 192; // 3 blocks of 64 bytes
    auto original_scratchpad = pattern_buffer(n);

    // Combined path.
    auto combined_scratchpad = original_scratchpad;
    armrx::AesState combined_hash{};
    auto combined_fill_state = zero_state();
    armrx::hash_and_fill_aes_1r_x4(combined_scratchpad, combined_hash, combined_fill_state);

    // Separate path: hash the ORIGINAL content, fill from a fresh identical
    // fill_state into a fresh buffer.
    armrx::AesState separate_hash{};
    armrx::hash_aes_1r_x4(original_scratchpad, separate_hash);

    auto separate_fill_state = zero_state();
    auto separate_fill_output = zero_buffer(n);
    armrx::fill_aes_1r_x4(separate_fill_state, separate_fill_output);

    assert(combined_hash == separate_hash);
    assert(combined_fill_state == separate_fill_state);
    assert(combined_scratchpad == separate_fill_output);

    std::cout << "[test_aes_hash] test_hash_and_fill_aes_1r_x4_matches_separate_calls passed\n";
}

int main() {
    test_fill_aes_1r_x4_golden_and_determinism();
    test_fill_aes_4r_x4_determinism_and_prefix();
    test_hash_aes_1r_x4_determinism_and_sensitivity();
    test_hash_and_fill_aes_1r_x4_matches_separate_calls();
    std::cout << "ALL AES_HASH TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
