// W3-2 scheduler swap-budget bisection harness.
//
// Regression guard for the ARMRX_MAX_SWAPS diagnostic hook (see
// src/jit_compiler_a64.cpp scheduleProgram / g_swap_budget). The hook lets a
// bisection driver cap the number of reordering swaps the emitter scheduler
// performs, to isolate which specific swap first causes a JIT/interpreter
// divergence (the mechanism behind the failed memory-op scheduler extension,
// docs/experiments/memory-op-scheduler-attempt.md).
//
// This test does NOT enable any memory-op scheduling. It merely verifies that,
// across a range of swap budgets, the emitted program order remains a valid
// permutation that produces a hash byte-identical to the reference interpreter.
// budget=unset  -> full scheduler, unchanged behavior
// budget=0      -> zero swaps, original order (must still be correct)
// budget=1      -> exactly one swap permitted
// budget=huge   -> effectively unlimited, same as unset
//
// If any budget ever produced a divergence, that itself would be a bug in the
// hook (it must never change which program is executed, only emission order of
// an already-valid reordering). This test would then catch it loudly.

#include "armrx/vm.hpp"
#include "armrx/argon2.hpp"
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

// Run one equivalence sweep with the given ARMRX_MAX_SWAPS setting applied via
// the process environment, returning true if all JIT hashes match the
// interpreter reference.
bool run_budget(const char* budget_setting, int& out_pairs) {
	if (budget_setting) {
		// setenv takes a non-const char* on some libc; cast away for portability.
		if (setenv("ARMRX_MAX_SWAPS", budget_setting, 1) != 0) return false;
	} else {
		// Unset so the hook falls back to its -1 (unlimited) default.
		unsetenv("ARMRX_MAX_SWAPS");
	}

	constexpr int kNumSeeds = 8;
	constexpr int kInputsPerSeed = 2;
	int total = 0;

	for (int si = 0; si < kNumSeeds; ++si) {
		const std::string seed = "jit_bisect_seed_" + std::to_string(si);
		std::vector<std::byte> key;
		key.reserve(seed.size());
		for (char c : seed) key.push_back(static_cast<std::byte>(c));

		armrx::Argon2dCache cache;
		cache.initialize(key);

		for (int ii = 0; ii < kInputsPerSeed; ++ii) {
			const std::string input = "bisect input " + std::to_string(si) + "_" + std::to_string(ii);

			armrx::VirtualMachine vm_jit(armrx::kRandOMXFlagJit | armrx::kRandOMXFlagHardAes);
			vm_jit.set_cache(&cache);
			std::array<std::byte, 32> hash_jit{};
			armrx::randomx_calculate_hash(&vm_jit, input.data(), input.size(), hash_jit.data());

			armrx::VirtualMachine vm_interp(armrx::kRandOMXFlagDefault);
			vm_interp.set_cache(&cache);
			std::array<std::byte, 32> hash_interp{};
			armrx::randomx_calculate_hash(&vm_interp, input.data(), input.size(), hash_interp.data());

			++total;
			if (hash_jit != hash_interp) {
				std::cerr << "FAIL: JIT/interpreter mismatch for budget="
				          << (budget_setting ? budget_setting : "unset")
				          << " seed=\"" << seed << "\" input=\"" << input << "\"\n";
				return false;
			}
		}
	}

	out_pairs = total;
	return true;
}

} // namespace

int main() {
	struct Setting { const char* value; const char* label; };
	const Setting settings[] = {
		{nullptr,   "unset (full scheduler)"},
		{"0",       "0 (no swaps)"},
		{"1",       "1 (single swap)"},
		{"100000",  "100000 (effectively unlimited)"},
	};

	bool all_ok = true;
	for (const auto& s : settings) {
		int pairs = 0;
		const bool ok = run_budget(s.value, pairs);
		if (ok) {
			std::cout << "bisect budget=" << s.label << ": " << pairs
			          << " (seed, input) pairs checked, all byte-identical\n";
		} else {
			all_ok = false;
		}
	}

	if (!all_ok) {
		std::cerr << "FAILED: one or more swap-budget settings diverged from the interpreter\n";
		return 1;
	}
	std::cout << "scheduler bisect: all swap-budget settings produce interpreter-identical hashes\n";
	return 0;
}
