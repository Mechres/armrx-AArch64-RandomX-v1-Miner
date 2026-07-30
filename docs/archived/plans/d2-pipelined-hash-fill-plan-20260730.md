# Track D2 — Cross-hash Boundary Pipelining

**Status:** Implementation brief for Reasonix
**Estimated effort:** ~3–5 days
**Expected ceiling:** ~0.5–1% total hashrate
**Correctness risk:** Low (no JIT changes, no register-file pressure)

## Problem

`randomx_calculate_hash()` runs phases sequentially:

```
blake2b(input) → fill_aes_1r_x4 (writes 2 MiB) → VM program 8× → hash_aes_1r_x4 (reads 2 MiB) → blake2b(output)
```

`fill_aes_1r_x4` (~12.3% cycles) and `hash_aes_1r_x4` (~12.3% cycles) are both AES T-table traversals over the 2 MiB scratchpad. On this in-order Cortex-A53, each load/store to scratchpad incurs L2 cache miss latency (~11% `ld_dep_stall`). Running them sequentially means the pipeline stalls independently for each.

But hash N's AES-finalization (reading scratchpad N) and hash N+1's fill (writing scratchpad N+1) operate on **different nonces' scratchpads** — no data dependency. By interleaving them at the 64-byte block level, one memory stream can issue loads while the other's computation occupies the pipeline, hiding a fraction of the latency.

## Design

### Core building block: interleaved hash+fill

A new function that reads 64-byte blocks from one scratchpad (for AES hash) and writes to another (for AES fill) in lockstep:

```cpp
void hash_and_fill_aes_interleaved_x4(
    std::span<const std::byte> hash_scratchpad,  // READ: scratchpad N being AES-hashed
    std::span<std::byte> fill_scratchpad,          // WRITE: scratchpad N+1 being AES-filled
    AesState& hash_state,                          // in: reg_.a from final VM run; out: finalized hash
    AesState& fill_state                           // in: blake2b(next_input); out: consumed
);
```

Identical loop to `hash_and_fill_aes_1r_x4` (both scalar and NEON paths, both finalization rounds) — only changed: `sp0..sp3` read from `hash_scratchpad`, `fs0..fs3` written to `fill_scratchpad`. Bit-identical results to separate sequential calls.

### Pipelined hash function

```cpp
void randomx_calculate_hash_pipelined(
    VirtualMachine* machine,
    const void* input, std::size_t input_size, void* output,
    const void* next_input, std::size_t next_input_size,
    std::byte* next_scratchpad,  // 2 MiB buffer for next hash's scratchpad
    void* next_seed_out          // 64-byte output: seed for first run() on next_scratchpad
);
```

Flow:
1. **Part A** — Compute current hash: blake2b(input) → fill VM's current scratchpad → 8× run() + blake2b chain. (Identical to first part of `randomx_calculate_hash`.)
2. **Part B** — Prepare next hash: blake2b(next_input) → seed_N+1
3. **Part C** — Save `next_seed_out = seed_N+1` (caller needs copy for run())
4. **Part D** — Interleave: `hash_and_fill_aes_interleaved_x4(current_scratchpad, next_scratchpad, hash_state_from_reg.a, seed_N+1)`
5. **Part E** — Finalize current hash output: blake2b(register_file_with_aes_hash_result, output)
6. **Part F** — Set VM scratchpad to `next_scratchpad` via `machine->set_scratchpad()`

### Worker loop restructuring

Each worker allocates 2 scratchpad buffers (2 × 2 MiB = 4 MiB) and alternates their roles:

```
Two scratchpads: sp[0], sp[1]
sp_for_hash: which buffer has completed VM execution (needs AES hash)
sp_for_fill: which buffer will be filled (for next hash's VM execution)

Prime: fill sp[0], run VM 8× on sp[0]
       sp_for_hash=0, sp_for_fill=1

Loop:
  1. pipelined_hash(input_current, sp[sp_for_fill], seed_out)
     → AES-hashes sp[sp_for_hash] while filling sp[sp_for_fill]
     → VM's scratchpad now = sp[sp_for_fill]
  
  2. Run VM 8× on sp[sp_for_fill] using seed_out as first seed
     → sp[sp_for_fill] has completed VM execution
  
  3. swap(sp_for_hash, sp_for_fill)
     → sp_for_hash = buffer just executed (needs hash next)
     → sp_for_fill = buffer just hashed (gets filled next)
  
  4. Advance nonce, loop to 1
```

## Files to modify

### 1. `include/armrx/vm.hpp`

Add to public section:
```cpp
void set_scratchpad(std::byte* ptr, std::size_t size) {
    scratchpad_data_ = ptr;
    scratchpad_size_ = size;
}
std::span<const std::byte> scratchpad_span() const {
    return std::span<const std::byte>(scratchpad_data_, scratchpad_size_);
}
```

Declare:
```cpp
void randomx_calculate_hash_pipelined(
    VirtualMachine* machine,
    const void* input, std::size_t input_size, void* output,
    const void* next_input, std::size_t next_input_size,
    std::byte* next_scratchpad, void* next_seed_out
);
```

### 2. `include/armrx/aes_hash.hpp`

Add declaration:
```cpp
void hash_and_fill_aes_interleaved_x4(
    std::span<const std::byte> hash_scratchpad,
    std::span<std::byte> fill_scratchpad,
    AesState& hash_state,
    AesState& fill_state
);
```

### 3. `src/aes_hash.cpp`

Implement `hash_and_fill_aes_interleaved_x4`:

**Structural change from `hash_and_fill_aes_1r_x4` (line 241):**
- Signature: hash_scratchpad (span<const byte>) + fill_scratchpad (span<byte>) instead of single scratchpad
- Inside the for loop: read `sp0..sp3` from `hash_scratchpad[offset]`, write `fs0..fs3` to `fill_scratchpad[offset]`
- Everything else: same T-table transforms, same XOR keys, same finalization rounds, same `#if ARMRX_ENABLE_NEON_TTABLE_AES` guard

**Implementation approach:**
1. Copy the entire body of `hash_and_fill_aes_1r_x4` (lines 242–370)
2. Change function signature and span parameters
3. Replace all reads from `scratchpad[offset+N]` with reads from `hash_scratchpad[offset+N]`
4. Replace all writes to `scratchpad[offset+N]` with writes to `fill_scratchpad[offset+N]`
5. Add `ARMRX_ASSERT` that both spans are the same size and 64-byte aligned

### 4. `src/vm.cpp`

Implement `randomx_calculate_hash_pipelined`:

```cpp
void randomx_calculate_hash_pipelined(
    VirtualMachine* machine,
    const void* input, std::size_t input_size, void* output,
    const void* next_input, std::size_t next_input_size,
    std::byte* next_scratchpad, void* next_seed_out
) {
    fenv_t fpstate;
    std::fegetenv(&fpstate);

    constexpr std::size_t scratchpad_size = 2ULL * 1024 * 1024;

    // ── Part A: Current hash up to final VM run ──
    alignas(16) std::array<std::byte, 64> temp_hash{};
    std::span<const std::byte> input_span(
        reinterpret_cast<const std::byte*>(input), input_size);
    blake2b(input_span, temp_hash.data(), 64);

    machine->init_scratchpad(temp_hash.data());
    machine->reset_rounding_mode();

    alignas(16) std::array<std::byte, sizeof(RegisterFile)> reg_bytes{};
    for (int chain = 0; chain < 7; ++chain) {
        machine->run(temp_hash.data());
        const auto& reg = machine->get_register_file();
        std::memcpy(reg_bytes.data(), &reg, sizeof(reg));
        blake2b(std::span<const std::byte>(reg_bytes), temp_hash.data(), 64);
    }
    machine->run(temp_hash.data());
    // Now: scratchpad has VM execution results, reg_.a has initial AES hash state

    // ── Part B: Prepare next hash's fill seed ──
    alignas(16) std::array<std::byte, 64> next_seed{};
    std::span<const std::byte> next_input_span(
        reinterpret_cast<const std::byte*>(next_input), next_input_size);
    blake2b(next_input_span, next_seed.data(), 64);

    // Save seed for caller's run() sequence (fill modifies the AesState in place)
    std::memcpy(next_seed_out, next_seed.data(), 64);

    // ── Part C: Save hash state before interleave ──
    RegisterFile current_reg = machine->get_register_file();
    AesState hash_state;
    static_assert(sizeof(current_reg.a) == sizeof(AesState),
                  "RegisterFile.a must span exactly 64 bytes");
    std::memcpy(hash_state.data(), &current_reg.a, sizeof(AesState));

    // ── Part D: Interleaved AES hash + fill ──
    hash_and_fill_aes_interleaved_x4(
        machine->scratchpad_span(),
        std::span<std::byte>(next_scratchpad, scratchpad_size),
        hash_state, next_seed    // next_seed is consumed by fill
    );

    // ── Part E: Finalize current hash output ──
    std::memcpy(&current_reg.a, hash_state.data(), sizeof(AesState));
    alignas(16) std::array<std::byte, sizeof(RegisterFile)> final_reg_bytes{};
    std::memcpy(final_reg_bytes.data(), &current_reg, sizeof(RegisterFile));
    blake2b(std::span<const std::byte>(final_reg_bytes),
            static_cast<std::byte*>(output), 32);

    // ── Part F: Prepare VM for next hash's execution ──
    machine->set_scratchpad(next_scratchpad, scratchpad_size);

    std::fesetenv(&fpstate);
}
```

### 5. `src/mining_engine.cpp`

Modify `worker_loop()`:

**Near VM construction (after line 377):**
```cpp
// Track D2: double-buffered scratchpads for pipelined hash+fill
constexpr std::size_t kScratchpadSize = 2ULL * 1024 * 1024;
auto dual_scratchpad = std::make_unique<std::byte[]>(kScratchpadSize * 2);
std::byte* sp[2] = { dual_scratchpad.get(), dual_scratchpad.get() + kScratchpadSize };
int sp_for_hash = 0;  // buffer with VM results (needs AES hash)
int sp_for_fill = 1;  // buffer to fill (for next hash)
// VM starts with sp[0] as active scratchpad
```

**Replace the `randomx_calculate_hash` call (line 496) with a primed pipeline:**

```cpp
// ── Prime: first hash ──
{
    randomx_calculate_hash(&vm, block_input.data(), block_input.size(), hash.data());
    ++local_hashes;
    meets_target... // check share

    std::uint64_t next_nonce = local_nonce;  // already advanced
    local_nonce += num_threads_;  // advance again — two nonces in flight
    vm.set_scratchpad(sp[1], kScratchpadSize);
    sp_for_hash = 1;  // sp[1] has VM results
    sp_for_fill = 0;  // sp[0] will be refilled
}

// ── Main pipelined loop ──
while (running_) {
    // Prepare next nonce's block_template
    alignas(16) std::array<std::byte, 32> hash_output{};
    alignas(16) std::array<std::byte, 64> next_seed{};
    std::vector<std::byte> next_block = block_input;  // template copy
    update_nonce_in_template(next_block, local_nonce, offset, size);
    
    randomx_calculate_hash_pipelined(
        &vm,
        block_input.data(), block_input.size(), hash_output.data(),
        next_block.data(), next_block.size(),
        sp[sp_for_fill],  // fill target
        next_seed.data()  // output: first run seed
    );
    // After: VM scratchpad = sp[sp_for_fill] (filled, ready for VM execution)
    //        sp[sp_for_hash] has been AES-hashed (result in hash_output)
    
    // Check share for current nonce
    ++local_hashes;
    if (meets_target(hash_output, local_job.target))
        share_callback_(local_job, current_nonce, hash_output);
    
    // Run VM 8× on the freshly-filled scratchpad
    {
        vm.reset_rounding_mode();
        alignas(16) std::array<std::byte, sizeof(RegisterFile)> reg_buf{};
        
        // First run uses next_seed (= blake2b of next_block)
        vm.run(next_seed.data());
        
        for (int chain = 0; chain < 7; ++chain) {
            auto reg = vm.get_register_file();
            std::memcpy(reg_buf.data(), &reg, sizeof(RegisterFile));
            blake2b(std::span<const std::byte>(reg_buf), next_seed.data(), 64);
            vm.run(next_seed.data());
        }
    }
    // Now: sp[sp_for_fill] has completed 8× VM runs
    //      sp[sp_for_hash] (the hashed buffer) is ready to be refilled
    
    // Swap roles for next iteration
    std::swap(sp_for_hash, sp_for_fill);
    // VM's scratchpad is STILL sp[sp_for_fill] (what we just ran on) = now sp_for_hash
    // The other buffer (sp_for_fill after swap) was the hash target and gets refilled
    
    // Advance nonces
    local_nonce += num_threads_;
    block_input = std::move(next_block);
    
    // Flush counters periodically (existing code, line 505)
    ...
}
```

**Important:** The `local_nonce` management changes. Currently each iteration processes one nonce. The pipelined version processes one nonce per loop iteration (the one that was in the pipeline from the previous iteration's fill). The priming step consumed the first nonce normally, and the first pipelined call processes the second, etc. So:

1. Prime: nonce = thread_id → normal hash → advance to thread_id + num_threads
2. Loop iteration: the pipelined call finalizes the hash for the CURRENT nonce while preparing the NEXT nonce's fill
3. The CURRENT nonce was fed to the pipelined call as `block_input`, the NEXT nonce as `next_block`
4. After the loop, the VM has just finished running on the filled scratchpad (which was prepared for the "next" nonce)
5. On the next iteration, that "next" nonce becomes the current one

The priming step should set up `block_input` for nonce = `thread_id`, compute it, then prepare the next `block_input` for the first pipelined call.

## Verification

1. **Bit-exact hash equivalence:** Run the same set of nonces through both `randomx_calculate_hash` and the pipelined path. Output must be identical for every nonce.
   - Create a test that hashes 100 consecutive nonces with baseline (sequential) and with D2, comparing hash outputs.

2. **Regression benchmark:** Run `bench_armrx --micro-only` to verify no regression on non-pipelined paths (AES fill, hash, fill+hash fused).

3. **KATs:** Must pass existing KAT tests.

4. **8-worker A/B on device:** `taskset -c 0-7 ./armrx ...` with D2 on vs off, sustained 3-minute runs.

## Clean-room note

This is a purely structural change — no RandomX specification knowledge beyond what's already in the codebase. The AES operations are the same standard AES T-table transforms already implemented. No XMRig code was consulted.

## Constraints

- **Do NOT modify** the JIT compiler (`src/jit_compiler_a64.cpp`, `jit_compiler_a64_static.S`) — D2 lives entirely in C++ AES operations and the worker loop.
- **Do NOT modify** `scratch_vm_study/` — that's upstream reference only.
- **Do NOT touch** existing `hash_aes_1r_x4`, `fill_aes_1r_x4`, or `hash_and_fill_aes_1r_x4` signatures — they're used by other paths. Add new function only.
- **Do NOT touch** the `override_scratchpad_for_bench` method — use the new `set_scratchpad` instead.
