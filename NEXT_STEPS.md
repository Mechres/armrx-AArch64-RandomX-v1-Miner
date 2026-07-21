# Next Steps Plan

**Updated:** 2026-07-20  
**HEAD:** fba761e  
**Devbox:** 192.168.10.156 — flaky SSH connection (EOF timeouts)

---

## Current Baseline (Cortex-A53, light mode, JIT)

| Metric | Pre-AES-fix (buggy) | Post-AES-fix (correct) | Δ |
|--------|:-------------------:|:----------------------:|:-:|
| Single-thread hashrate | 5.18 H/s | **4.34 H/s** | −16.4% |
| Pool (8-thread) | ~29 H/s | **~24.5 H/s** | −15.5% |
| Init scratchpad | 589 μs (0.31%) | 17,554 μs (7.63%) | +29.8× |
| Get final result | 1,023 μs (0.53%) | 17,974 μs (7.81%) | +17.6× |
| Chain execution (VM) | 190,860 μs (98.9%) | 194,692 μs (84.6%) | +2% |
| Branch miss rate | 34.42% | **31.6%** | −2.8pp |

### Key findings

- **NEON AES removal** caused the entire regression. The chain (VM JIT) barely changed (+2%). The old theory ("correct AES → different program entropy") was wrong.
- **NEON AES encrypt re-enable** (AESE+AESMC for encrypt-only ops) was benchmarked as **zero benefit** on Cortex-A53. Per-block NEON load/store overhead cancels the AES speedup. Reverted.
- **Branch-miss profile** (perf record -e branch-misses): **94.85%** of branch misses are in `execute_superscalar` — dataset **generation**, not the hash path. The JIT CBRANCH (`bne+b` fix) is invisible to perf and not a bottleneck. CBRANCH work is **deprioritized**.
- The real hash-path bottleneck: **chain JIT execution** at 84.6% of time, where instruction count and pipeline efficiency dominate.

---

## Re-prioritized Action List

| # | Item | Est. gain | Rationale |
|---|------|:---------:|-----------|
| **1** | **Peephole JIT coalescing** — per-opcode instruction reduction in emitted JIT code. Compare armrx vs XMRig emit sequences for high-frequency opcodes. | **+3–7%** | The only remaining path to close the instruction-count gap. The chain takes 84.6% of time. |
| **2** | **Instruction scheduling for A53** — static FP load scheduling in prologue (`static.S:236-263`), register-offset FP loads in `emitMemLoadFP()` | **+5–10%** | IPC 0.786 vs theoretical 2.0 peak. In-order A53 stalls on load-use and FP pipeline latency. |
| **3** | **Fix CTest executable path** — bench_armrx, bench_opcodes, test_jit_encodings, test_jit_determinism are "Not Run" by CTest (binary search path mismatch) | Cleanup | Low effort, enables automated checks. |

### Frozen / Deprioritized

| Item | Reason |
|------|--------|
| CBRANCH misprediction cost reduction | Profile proves 94.85% of branch misses are in dataset generation, not hash path. The `bne+b` fix handles JIT CBRANCH. |
| NEON AES encrypt re-enable | Zero benefit on Cortex-A53. Reverted. |
| XMRig A/B comparison | Old 33% gap measured with buggy AES. Not actionable until after peephole JIT work. |
| Newton-Raphson FDIV/FSQRT | Frozen — previous attempt segfaulted without KAT proof. |
| PGO retry | Blocked — GCC 15 + musl `__gcov_*` linker crash. |

---

## Devbox Connection Issue

The devbox (192.168.10.156) has intermittent SSH connectivity failures. The MCP bridge reports "read: EOF" errors, suggesting:
- SSH connection drops mid-session
- Possible causes: network timeout, WiFi instability, or SSH keepalive configuration

**To investigate on-device:**
```sh
# Check SSH server config
grep -i 'keepalive\|ClientAlive\|TCPKeepAlive' /etc/ssh/sshd_config

# Test persistent connection
ssh -o ServerAliveInterval=15 -o ServerAliveCountMax=3 mechres@192.168.10.156
```
