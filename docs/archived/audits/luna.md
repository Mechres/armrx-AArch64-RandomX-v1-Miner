GPT-5.6 Luna

Audit Status
- Current branch: main
- main is synchronized with origin/main.
- Tiers 1–7 in docs/briefs/2026-08-09-audit-consolidated.md are already merged or explicitly closed.
- Tier 8 is the only remaining audit area.
Findings
T8-1: LTO probe is stale for the target device
Status: Mostly closed.
- CMakeLists.txt:417-455 enables IPO/LTO by default when supported.
- cmake/toolchain-aarch64-musl.cmake:21-27,61 forcibly disables LTO because the cross toolchain lacks liblto_plugin.so.
- docs/experiments/perf-tracking.md:176 records an actual device A/B: LTO enabled 4.32 H/s versus LTO disabled 4.24 H/s, approximately a 1.9% LTO benefit.
- The cross toolchain cannot currently measure this because it cannot perform real cross-LTO.
Conclusion: There is no untested +1.9% code optimization left for the shipping cross-build. The remaining work is toolchain infrastructure:
- Use a cross compiler/binutils package with a working AArch64 LTO plugin.
- Build LTO and non-LTO binaries from identical source.
- Run JIT KATs and pinned on-device perf/H/s tests.
This is a packaging/toolchain experiment, not an armrx source optimization.
T8-2: Superscalar timing-model retuning should remain closed
Status: Closed as a viable code lever.
- src/superscalar.cpp:85-110 contains the x86-derived MacroOp model and superscalar operation descriptions.
- src/superscalar.cpp:497-546 schedules modeled execution ports.
- src/superscalar.cpp:568-639 uses that schedule to generate the superscalar program.
- docs/closed-levers-ledger.md:83-100 records three attempted designs:
- Two designs changed the timing model and broke reference hashes.
- The third design was proven a no-op.
- docs/briefs/2026-08-09-superscalar-disassembly-findings.md:80-128 confirms the emitted A64 operations are already minimum legal sequences and the only extra C* instructions are deliberate E24 padding.
Conclusion: A “real A53 cost model” is not a normal performance-only change. The superscalar generator output is consensus-critical; changing scheduling decisions changes generated programs and therefore hashes. Unless the new model is proven to generate byte-identical programs, it is not deployable. If it is forced to generate identical programs, it cannot improve the timing model.
This should be marked closed by consensus constraints, not merely “one more design remains.”
T8-3: Fleet re-validation is useful, but not a source performance lever
Status: Valid operational work, not an optimization backlog item.
- src/mining_engine.cpp:49-99 uses hwloc and CPU frequency to order cores.
- src/mining_engine.cpp:104-133 uses the sysfs frequency fallback.
- src/mining_engine.cpp:138-169 identifies the “top-frequency” cluster.
- include/armrx/cpu_features.hpp:16-24 correctly obtains online CPU count from sysfs rather than process affinity.
Potential fleet checks:
- Verify cpuinfo_max_freq ordering matches actual related_cpus.
- Verify cluster ordering on Redmi and Unisoc devices.
- Test fast mode only where available RAM permits it.
- Revalidate A55 behavior separately from A53 behavior.
Risk: Frequency-based cluster detection is a heuristic. Devices with equal or missing frequency data are treated as one cluster, and devices with unusual topology may be ordered incorrectly.
Conclusion: This deserves a portability test matrix, but it should not be described as a generic H/s optimization for Lenovo. It is device validation and affinity correctness.
T8-4: AES generator scalar XOR remains the only plausible small code probe
Status: Open but low priority.
- include/armrx/aes.hpp:350-369 contains byte-wise XOR loops in aes_encrypt_round() and aes_decrypt_round().
- src/aes_generator.cpp:44-60 reaches those helpers from AesGenerator4R::next().
- src/vm.cpp:763-772 constructs and fills the AES generator for each VM run.
- docs/experiments/w11-instruction-census.md:94-97 attributes only 359 samples to AesGenerator4R::next, compared with 101,556 samples in the instruction capture.
Conclusion: Disassemble the AArch64 binary once. If the compiler already turns the fixed 16-byte loop into vector loads/XOR/stores, close it. If it emits scalar byte operations, a vectorized implementation may be measurable, but the census suggests the maximum whole-hash effect is below approximately 0.5%.
This is the only remaining T8 source-level performance probe with a plausible, concrete code target.
T8-5: Fresh weighted JIT census is now diagnostic, not an optimization lead
Status: Substantially closed by evidence.
- docs/briefs/2026-08-09-superscalar-disassembly-findings.md:32-53 gives the actual opcode mix.
- :60-83 confirms direct-register operations are exactly one A64 instruction and IMUL_RCP is the minimum two-instruction ldr plus mul.
- :87-111 confirms C* padding is deliberate and removing it caused a measured 7.1% H/s regression.
- :115-128 explicitly concludes there is no safe instruction-redundancy lever.
A weighted multi-seed census could improve documentation confidence, but it is unlikely to discover an optimization because the instruction forms themselves have already been disassembled and compared with minimum legal A64 forms.
Conclusion: Keep the census as audit tooling, not as an active optimization task.
T8-6: Argon2 and Blake2b should remain closed for steady-state optimization
Status: Closed for per-hash performance.
- src/argon2.cpp:100-219 already has NEON Argon2 processing.
- src/argon2.cpp:300-364 shows Argon2 cache initialization is seed setup, not steady-state hash execution.
- src/blake2b.cpp:69-183 has an AArch64 NEON compression path.
- docs/experiments/perf-tracking.md:167-173 records Blake2b parity as an armrx win, not a deficit.
- docs/experiments/w11-instruction-census.md:94-97 attributes negligible Blake2b cost.
Conclusion: Do not rewrite either in assembly for H/s. Only revisit them for seed-rotation latency or if a new profile proves a regression.
Remaining Poorly-Implemented Areas
1. Partial-dataset lifecycle testing remains the weakest area
- The implementation has already needed multiple fixes for teardown, ownership, lost wakeups, and timing-sensitive tests.
- The consolidated brief records the fixes, but the lifecycle stress matrix should include destruction during fill, repeated refill, cancellation, exception teardown, and sanitizer runs.
2. Topology detection remains heuristic
- Frequency sorting is not the same as topology discovery.
- related_cpus, package IDs, cluster IDs, and CPU capacity would be more robust than cpuinfo_max_freq alone.
3. JIT contract validation is improved but still partly hard-coded
- src/jit_compiler_a64.cpp still depends heavily on linker-symbol deltas and manually maintained assembly layout.
- The adopted contract checks are valuable, but future template changes still require coordinated .S and C++ updates.
4. The superscalar timing model contains misleading x86 terminology
- src/superscalar.cpp:85-110 names ports and macro-ops as x86 concepts while generating consensus-critical RandomX programs for AArch64.
- It is not currently a correctness bug, but it makes future optimization work easy to misunderstand.
5. CMake target configuration is complex
- LTO, sanitizer propagation, target-specific definitions, cross-toolchain overrides, and optional dependencies are spread across CMakeLists.txt.
- The build works, but unsupported combinations should fail explicitly rather than silently disabling features.
Recommended Remaining Work
1. Perform the one-time AArch64 disassembly check for the AES generator XOR loops.
2. Acquire or build a cross toolchain with functional AArch64 LTO and run the LTO A/B.
3. Run the fleet topology/fast-mode validation matrix on Redmi and Unisoc.
4. Add lifecycle stress tests for PartialDataset.
5. Mark superscalar retuning and weighted JIT density as closed unless new evidence contradicts the current disassembly.
Bottom Line
There is no remaining high-confidence source optimization in Tier 8. The only plausible code-level probe is the AES generator XOR loop, and it is likely below the measurement threshold. The larger open items are toolchain validation, fleet portability, and lifecycle-test quality rather than new mining hot-path optimizations.

remaining ideas are experimental rather than evidence-backed wins.
1. Specialized post-fill hybrid JIT entry
src/jit_compiler_a64_static.S:637-671 performs a bound check and reloads partial-dataset metadata for every dataset-item call.
After the partial dataset is fully populated, a specialized entry could use immutable count/base values for the remainder of that seed generation.
Potential saving: roughly 4–6 instructions × 16,384 calls, approximately 65k–100k instructions/hash.
Expected whole-hash effect: below 0.1%.
Risk: very high. The previous inline hybrid attempt crashed due to offset-adjustment and register/lifetime bugs.
Gate: same-nonce reference comparison, seed rotation, test_mining, JIT equivalence, long --dataset-mb=512 pool test.
This is only worth testing if partial-dataset mode becomes a primary deployment mode.
2. AArch64 compiler matrix with Clang
Run GCC versus Clang on a non-musl AArch64 environment.
JIT .S and emitted instruction selection will not change much.
Possible effects are limited to Blake2b, AES wrapper code, mining glue, and dataset initialization.
Expected gain: unknown, probably small.
Risk: compiler/runtime incompatibility and different floating-point behavior.
Gate: all JIT differential tests, AES KATs, mining shares, and perf counters.
This is a build experiment, not a likely solution to the instruction-count gap.
3. A55-specific E24 validation
The E24 C* padding was optimized for Cortex-A53’s MAC interlock. A55 has materially different pipeline behavior.
Compare the current three-instruction C* form against the denser literal-load form on an A55.
Keep the current form as the A53 default.
Expected gain: device-specific and unknown.
Risk: consensus behavior must remain byte-identical; only emission form may vary if architecture detection is reliable.
Gate: per-device JIT KATs, deterministic emission, perf stat, and long mining runs.
This should not be attempted as a Lenovo/A53 optimization.
4. Better topology detection
src/mining_engine.cpp:70-99 and :104-133 sort CPUs by cpuinfo_max_freq; this is only a proxy for cluster topology.
A portable topology layer could use:
/sys/devices/system/cpu/cpu*/topology/related_cpus
cluster_id where available
CPU capacity/frequency as a secondary ranking
Potential impact is operational rather than per-hash codegen, but it could improve worker placement on devices where frequency ordering is misleading.
5. Adaptive partial-dataset sizing
Instead of a fixed --dataset-mb=N, select the largest useful prefix based on:
available memory,
number of workers,
fill duration,
measured dataset-hit rate,
interconnect pressure.
The current 512 MiB mode reaches roughly 30–32 H/s but incurs a long startup dead-stop. A smaller prefix may provide a better startup/throughput tradeoff on lower-memory devices.
This is a product/runtime experiment, not a hot-loop optimization.
6. Direct fixed-size block operations audit
The code contains several fixed-size copies:
src/aes_generator.cpp:10-17,45-60
src/aes_hash.cpp:150-161
src/vm.cpp:974-987
The compiler probably already lowers many of these to efficient loads/stores, but an AArch64 disassembly audit could identify missed vectorization or unnecessary stack traffic.
Expected impact is likely below 0.5%; only pursue a change if disassembly shows scalar code where NEON is legal.
7. Harden the superscalar generator model without changing consensus
The execution model in src/superscalar.cpp:497-639 is still expressed using x86-style port names and macro-op terminology. Retuning it changes generated programs and hashes, so it cannot be freely optimized.
A safe experimental direction would be diagnostic only:
model A53 costs,
compare predicted versus measured opcode/body costs,
do not use the model to generate production programs until byte identity is proven.
This can improve understanding, but it is not currently a shippable performance lever.
8. Make the JIT dump a permanent diagnostic tool
The superscalar disassembly work used a non-merged dump branch. Promoting a guarded raw JIT-byte dump would make future audits reproducible.
No runtime cost when disabled.
Useful for opcode-level instruction accounting and regression detection.
Should be restricted to diagnostics and avoid live code patching because of the device I-cache anomaly.
What I would not pursue
Blake2b assembly rewrite: already NEON and negligible in the census.
Argon2 assembly rewrite: setup-time, already NEON.
More JIT scheduler reorderings: repeatedly negative or divergent.
More literal-pool density work: E24 proved that fewer instructions can reduce H/s.
More PRFM/alignment/NOP experiments: closed on this A53.
Generic C++ to assembly conversion: the measured hot path is already A64.
The only genuinely interesting new code experiment for the current Lenovo is the post-fill hybrid JIT specialization. Everything else is either portability work, diagnostic tooling, or a different-device experiment.

The current main audit confirms most closures are technically sound, but several closure records are broader than the experiments support. The main issues are not new performance wins on Lenovo; they are incorrect per-family accounting, stale ledger entries, and a few levers labeled closed even though a distinct design remains untested.
Audit Result
main is current and clean. Most closures are correct for the exact designs tested, but several records overstate family-wide closure.
Findings
 1. Hybrid dead-stop claims three tested variants, but Variant 2 was a no-op
- Severity: Medium
- docs/briefs/2026-08-07-hybrid-deadstop-variants.md:35-41 describes Variant 2 as untested co-located fill.
- :85-88 confirms the branch changed only comments because exclude_cores={} was already present on main.
- The reported 178s/28.53 H/s result is the baseline, not an experiment.
- Conclusion: The final steady-state conclusion is sound, but “three variants failed” is inaccurate. Only Variants 1 and 3 were behavioral attempts.
 2. Superscalar timing-model closure is not family-exhaustive
- Severity: Medium
- docs/closed-levers-ledger.md:83-100 records three failed designs.
- src/superscalar.cpp:497-546 still contains an x86-style port model and :568-639 uses it to generate consensus-critical programs.
- The explicitly different “real A53 cost model” was never implemented.
- Conclusion: The attempted designs are correctly closed. The broader family remains open only as a research exercise, but changing it risks changing generated programs and hashes. It is not a safe performance lever.
 3. The *_M scheduler is closed as implemented, not as a whole family
- Severity: Medium
- docs/closed-levers-ledger.md:51-59 correctly records deterministic divergence for the tested scheduler extension.
- src/jit_compiler_a64.cpp:431-440 intentionally excludes memory operations from long-latency scheduling.
- docs/experiments/next-iteration-plan.md:192-218 still identifies narrower opcode subsets or a different scheduling structure as untested.
- Conclusion: The exact implementation is correctly closed. A different mechanism could still exist, but it is high-risk and correctly de-prioritized because the real-pool gap was not explained by this microbenchmark.
 4. Argon2 copy-elimination closure is too broad
- Severity: Medium
- The failed copy-elimination design is well supported by docs/experiments/argon2-compress-copy-elimination.md:120-147.
- That document explicitly leaves a different memory-layout/cache strategy unexplored at :148-152.
- Current src/argon2.cpp:250-260 still performs full block XOR passes, while :100-219 uses NEON.
- Conclusion: Copy elimination is correctly closed. The entire Argon2 optimization family is not proven closed, although the alternate layout approach is high risk and has no evidence of cache pressure.
 5. N1 load-pair fusion is correctly closed only for current dynamic memory operations
- Severity: Low
- docs/experiments/w22-n1-adjacency-analysis.md:110-122 proves emitMemLoad() and ISTORE cannot use immediate-offset ldp/stp with their current register-indexed addressing.
- The same document identifies adjacent FP loads in the static main loop at :124-132.
- src/jit_compiler_a64_static.S:243-297 shows those loads are intentionally interleaved with integer loads and FP conversion work.
- Conclusion: The JIT memory-op closure is correct. Static FP-load pairing is a distinct, tiny, unmeasured cleanup, not evidence that the original N1 closure was wrong.
 6. Main-thread policy closure is design-specific
- Severity: Low
- docs/closed-levers-ledger.md:101-128 reports nice=10 as measurement-null.
- The same record says SCHED_IDLE was not tested.
- Conclusion: Closing nice=N is correct. The entire main-thread scheduling-policy family is not exhaustively closed, but further testing is not justified without evidence of main-thread contention.
 7. Worker-local buffer reuse remains stale in the ledger
- Severity: Medium documentation defect
- docs/closed-levers-ledger.md:137-141 says worker-local buffer reuse is open and untried.
- Current src/mining_engine.cpp:652-655 reuses next_block instead of declaring it inside the loop, and :690-693 swaps it with block_input.
- README.md:189-191 records the change as adopted.
- Conclusion: The ledger is stale and should not be used to identify this as open work.
 8. The LTO “open +1.9%” record is misleading for the target
- Severity: Low
- CMakeLists.txt:417-455 supports LTO/IPO.
- cmake/toolchain-aarch64-musl.cmake:21-27,61 disables it because the cross toolchain lacks the LTO plugin.
- docs/experiments/perf-tracking.md:176 already records an on-device LTO A/B: 4.32 H/s enabled versus 4.24 H/s disabled.
- Conclusion: LTO is already measured positive on the device-native toolchain. The remaining work is acquiring a usable cross-LTO toolchain, not discovering an untested code optimization.
 9. Hugepage closure needs qualification
- Severity: Low
- src/argon2.cpp:269-290, src/partial_dataset.cpp:35-66, and src/vm.cpp:132-142 already attempt huge pages and prefaulting.
- docs/experiments/perf-tracking.md:161-171 supports closing generic hugepage tuning.
- Other experiment records report approximately +0.8% when hugepage provisioning succeeds.
- Conclusion: Further implementation changes are not justified. The correct statement is “the code’s hugepage strategy is adequate; provisioning can still affect performance,” not “hugepages never matter.”
10. Historical C documentation is contradictory*
- Severity: Medium documentation defect
- docs/experiments/perf-tracking.md:175 describes an LDR-pool form as current and says armrx is ahead.
- Current src/jit_compiler_a64.cpp:1492-1511 uses the E24 three-instruction MOVZ/MOVK + ALU form.
- docs/briefs/2026-08-09-superscalar-disassembly-findings.md:87-111 correctly documents that the LDR form regressed H/s.
- Conclusion: The authoritative current conclusion is E24. The older performance-tracking wording is historical but easy to misread and should be treated as stale.
Closures That Are Correct
- E24 C* padding: correctly retained; disassembly confirms direct operations are minimum legal A64, and removing padding reduced H/s.
- PRFM hints: tested and regressed.
- NOP/alignment padding: tested and regressed.
- Dataset load batching: tested null and correctly understood as dependency-bound.
- Two-way superscalar interleave: measured worse due to I-cache refills.
- Hybrid hash-during-fill on Lenovo: tested variants support the steady-state dead-stop decision, despite Variant 2 accounting error.
- Worker scaling: correctly classified as SoC topology behavior rather than an armrx-specific code defect.
- PGO: adequately closed for the current measured toolchain, though not universal across compilers.
- Blake2b and Argon2 copy elimination: the specific implementations tested are correctly closed.
- PartialDataset ownership, publication wakeups, teardown, JIT contracts, W^X, and C++ RegisterFile copy removal are already adopted and should not be re-opened as pending work.
Bottom Line
The project’s evidence discipline is generally strong, but the closure metadata is not fully normalized. The most important corrections are:
- Mark Hybrid Variant 2 as not run/no-op, not failed.
- Keep superscalar timing-model and *_M scheduling as specific-design failures, not absolute impossibility.
- Mark worker-local buffer reuse as adopted, not open.
- Qualify hugepage and LTO conclusions by toolchain/provisioning.
- Keep Argon2’s tested copy-elimination design closed, but do not claim every future memory-layout design is disproven.
No clearly justified, high-confidence Lenovo H/s optimization was missed by these closure records.