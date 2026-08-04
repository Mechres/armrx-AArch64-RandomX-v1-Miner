# Testing & Measurement Baseline — armrx

**Applies to:** Era II (post-"Stall Mirage"). Every performance claim in this repo must be
backed by a measurement made with the commands in this file. If a number wasn't produced by
the harness below, treat it as unverified.

Device: MSM8929 / 8× Cortex-A53 @ 765 MHz (fixed, no cpufreq), postmarketOS, BusyBox ash.
Cross-build host: x86_64 + `aarch64-linux-musl-g++` (GCC 16.1.0). On-device GCC 15.2.0
native builds are ~7.9% slower than the cross build — **always benchmark the cross binary**
(the shippable artifact) unless explicitly comparing toolchains.

---

## 1. The one valid instruction/op/IPC source: `--perf-ready`

For any instruction-count, cycles, or IPC number, use the **gated 500-hash window**:

```sh
# on-device, cross-built binary
bench_armrx --full-hash-only --perf-ready --workers=<N>
```

- `--perf-ready` gates an *exactly 500-hash* window (deterministic, replay-invariant).
- `--workers=N` runs N worker threads but the window is **per-worker-equivalent** (the
  500-hash count is the harness's own count — do NOT divide by N for "total"; the reported
  median hash time already accounts for it). For a TRUE N-worker per-hash number, see §3.
- This is the ONLY authoritative source for instr/hash, cyc/hash, IPC. The 1w census
  (`docs/experiments/w11-instruction-census.md`) used exactly this and is the reference
  baseline: **armrx 89.5M instr/hash @ IPC 0.547 vs XMRig 101.4M @ 0.654 (1 worker)**.

**NEVER use `--mine` for measurement.** The pool miner idles ~41% of the time (worker
waiting on stratum/accept, not hashing) so cycles/elapsed ≠ 765 MHz and every derived rate
is corrupted. `--mine` is for live-pool soak only.

---

## 2. PMU events (perf v7.1.3, no root, `:u` userspace qualifier)

Only these 7 events are validated on this kernel/PMU. Other event names (e.g.
`dtlb_store_miss`, `l2d_cache`) **error out and abort the whole perf run** — do not add them.

```
cycles:u  instructions:u  ld_dep_stall:u  other_interlock_stall:u
agu_dep_stall:u  l1d_cache_refill:u  l2d_cache_refill:u
```

Attach by PID (the pool/miner runs forever; `perf stat` prints at child exit, so you must
kill the child to flush — see §4):

```sh
perf stat -e cycles:u,instructions:u,ld_dep_stall:u,other_interlock_stall:u,agu_dep_stall:u,l1d_cache_refill:u,l2d_cache_refill:u \
  -- /path/to/binary <args> > perf.out 2>&1
```

Normalize per-hash: `event_total / hash_count`. For `--perf-ready` the hash_count = 500.
For pool runs, read `Total: N` from stdout (exact).

---

## 3. True N-worker measurement (when you need real 8w per-hash)

The bench `--perf-ready` is per-worker-equivalent, NOT a true N-worker aggregate. For a real
N-worker per-hash number (e.g. to compare armrx vs XMRig at 8w, see `m1-miner-to-miner-pmu-diff.md`):

- Run the pool miner for a fixed ~60–120s window under perf (both miners, same pool, same
  seed → identical workload).
- Hash count = `Total: N` from stdout (armrx) or H/s × time (XMRig — its console is
  block-buffered, see §4).
- Normalize events by that count.

This is how M1 was done. It is valid **only** when both miners run the identical workload
(RandomX light mode, same pool/job).

---

## 4. Device quirks (learned the hard way — read before every on-device run)

1. **`/tmp` is `noexec` and a 512M tmpfs.** Ship binaries to a writable exec dir (e.g.
   `/tmp/ab_off_test/` created by scp as a dir). A bare `/tmp/<name>` may also end up a
   directory if you scp multiple sources to one target. Verify with `ls -la` before running.
2. **The pool miner used to ignore SIGINT — FIXED (2026-08-06, `1e5fc52`).** It now exits
   cleanly on Ctrl-C / `kill -INT` (graceful teardown, no `kill -9` needed). If a process
   genuinely won't die, `kill -9 <pid>` still works. `perf stat` still only prints at child
   exit, so to flush perf you may still need `kill -9 <child_pid>` (or kill the parent `perf`
   wrapper).
3. **XMRig console is block-buffered** — no output until exit. Run it under perf and `kill -9`
   the child to flush; its hash count is then estimated from H/s × elapsed (±5%).
4. **Cross-build `build-cross` gets poisoned by stale objects** after any E2x session where a
   clock-jump leaves `.o` mtimes in the future (make skips recompilation → links old code →
   **segfaults at startup**). Symptom: a freshly-built binary segfaults while an older binary
   of the same HEAD runs fine. **Fix: `rm -rf build-cross` and rebuild pristine** before any
   measurement you intend to trust.
5. **`rm -rf build-cross` + configure needs `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF`** on
   this toolchain (the LTO capability probe otherwise fails and aborts configure). The
   cross toolchain file already forces `ARMRX_DISABLE_LTO=ON`; just add the IPO=OFF flag.
6. **ALWAYS `pgrep -af armrx|xmrig` before launching** — stale workers from a previous run
   (that ignored SIGINT) will compete for cores and corrupt rates. Kill them first.
7. **Timing math:** 765 MHz fixed clock. For a sanity check, `cycles ≈ 765M × seconds`. If
   `cycles/elapsed` is far from 765M (e.g. ~315M), the worker was idle (you used `--mine`) —
   discard the run.

---

## 8. Device execution discipline (learned the hard way — 2026-08-06)

These rules exist because violating them **wedged the device** (unresponsive, hard reboot
required) and because an SSH mistake spawned **two** copies of a 20-minute stress test that
competed for the weak A53. Every on-device run must follow them.

### 8.1 Run tests ONE AT A TIME — confirm the previous one is FINISHED, not still running
- The slow gates (`test_jit_scheduler_stress` 450 pairs ~20 min, `test_jit_superscalar_scheduler_stress`
  200 pairs ~35 min, `test_mining` KAT ~7 min, `--perf-ready` 500-hash windows) **peg all 8 cores**.
  Running two at once (or starting the next while the prior is still going) oversubscribes the
  box and can make it unresponsive.
- **Before launching any test, confirm nothing from the prior run is alive:**
  ```sh
  ssh mechres@192.168.10.156 'pgrep -af "test_jit|bench_armrx|armrx|miner"'
  ```
  If anything is still running, wait or `kill -9` it first (see §8.3). Only then start the next.
- Do NOT bundle the suite: `ctest` / "run all tests" is forbidden on-device. One binary, one
  invocation, per session.

### 8.2 scp and ssh are SEPARATE commands — never chain them
- **WRONG (this is what wedged things):** `ssh host 'mkdir ...' && scp build/* host:/dir/`
  The `&&` joins `ssh` and `scp` as one host-side line; `scp` is a separate program, not an
  argument to `ssh`. The leading `ssh` opens a dangling session (seen on-device as a stuck
  `sshd-session ... mechres@notty`) that lingers after the command "times out".
- **RIGHT — one `scp` per binary, no leading `ssh`:**
  ```sh
  scp -o ConnectTimeout=20 build-cross/test_jit_equivalence mechres@192.168.10.156:/tmp/cross-dag/
  ```
  Create the target dir in its own quick `ssh` first if needed (not chained with `&& scp`):
  ```sh
  ssh -o ConnectTimeout=20 mechres@192.168.10.156 'mkdir -p /tmp/cross-dag && rm -rf /tmp/cross-dag/*'
  ```
- Ship **one binary at a time**. The full test set is ~8 binaries; pushing them in one `scp`
  is unnecessary load and a single point of failure.

### 8.3 An SSH *timeout* does NOT kill the remote process
- When a foreground `ssh ... ./test_...` "times out" (client drops after N seconds), the
  **remote `ash -c` + binary keep running on the device** — the connection drop does not send
  SIGINT/SIGTERM to the remote process. If you then launch a second `ssh` run, you get **two**
  copies competing for cores.
- If a run "timed out" on your side, assume it is STILL RUNNING on-device. Verify, then kill,
  before restarting:
  ```sh
  ssh mechres@192.168.10.156 'pgrep -af test_jit_scheduler_stress'   # is it still there?
  ssh mechres@192.168.10.156 'pkill -9 -f test_jit_scheduler_stress' # kill ALL copies
  ```
  ⚠️ `pkill -f <pattern>` also matches your own `ssh` command line if it contains the pattern —
  run the `pkill` as its own bare `ssh` (no other mention of the binary in the same command),
  or it will drop your own session.
- Preferred pattern for long tests: redirect output to a device-side log and run in the
  background, then `ssh ... 'cat /tmp/cross-dag/stress450.log'` to read the result — avoids a
  hung foreground pipe:
  ```sh
  ssh mechres@192.168.10.156 'cd /tmp/cross-dag && ./test_jit_scheduler_stress > /tmp/cross-dag/stress450.log 2>&1; echo DONE_EXIT=$? >> /tmp/cross-dag/stress450.log'
  ```

### 8.4 qemu is NOT a substitute for on-device verification
- qemu-aarch64 does **not** model the Cortex-A53 in-order pipeline / memory-latency wall. Perf
  numbers from qemu are meaningless, and the slow JIT tests just cook the host. qemu may catch
  a *compile* error or a *crash*, but it does **not** clear any correctness or perf gate —
  especially the `*_M` hazard class, where qemu and silicon diverge (that is exactly the W3-2
  trap). All four gates in §5 must run on the real device.

### 8.5 Core pinning (fast cluster 0-3 vs slow cluster 4-7)
MSM8929 has two 4-core clusters: **0-3 = fast L2 domain, 4-7 = slow L2 domain (~50% throughput
under contention)**. Both are Cortex-A53 at the same 765 MHz firmware clock (no cpufreq) — the gap
is the L2/interconnect domain, not frequency.
- **Correctness gates** (`test_jit_equivalence`, `test_jit_determinism`, `test_jit_dataset_2way`,
  `test_jit_scheduler_stress`, `test_jit_superscalar_scheduler_stress`): their result is
  **byte-identical hashes — core-invariant**. Pinning to the fast cluster (`taskset -c 0-3`) is
  safe AND speeds the run up (avoids the slow-cluster penalty). Prefer pinning correctness tests
  to 0-3. It never changes a pass/fail outcome.
- **Perf A/B (§5 gate 4):** pin **both** variants **identically** — e.g. both
  `bench_armrx --full-hash-only --perf-ready` under `taskset -c 3` (the existing methodology
  already pins perf runs to core 3). **Never pin only one side** of an A/B: asymmetric pinning
  measures cluster placement, not the change under test. If you pin the DAG variant to 0-3, pin
  the default variant to 0-3 too.
- Do NOT pin a run that is already in flight (killing + restarting risks a duplicate per §8.3).
  Let a running test finish, then apply pinning to subsequent runs.

---

## 5. The gate sequence (every code change must pass this)

In order. A change that fails any gate is reverted (per "revert-on-failure" discipline).

1. **KAT — `test_jit_equivalence`** (16 seed/input pairs, byte-identical hash vs interpreter).
   Fast (~1 min on-device). This is the correctness floor. Run on-device, not cross-host
   (the JIT is target-specific).
2. **Scheduler stress — `test_jit_scheduler_stress`** (450 pairs, ~20 min on-device, light).
   The W3-2 death gate — it caught every prior replay-breaking change.
3. **Superscalar stress — `test_jit_superscalar_scheduler_stress`** (200 pairs, ~35 min).
   Secondary gate for the superscalar emission path.
4. **1w + 8w H/s** via `bench_armrx --full-hash-only --perf-ready` (1w) and the §3 pool method
   (8w). Compare against the baselines in STRATEGY.md. A real win = improves the *target
   metric* (the one Phase 0 locks) without regression on the other.

All four must pass before a change is "shipped" (committed as a real optimization, not a
gated experiment).

---

## 6. Reversible A/B pattern (how to experiment without forking the tree)

Gate any candidate change behind a compile flag, default OFF, so HEAD always builds the
shipping behavior:

```cpp
#ifdef ARMRX_NO_E24_PAD   // example from the E24 A/B
    if (imm < (1u << 16)) { emit32(MOVZ..., code, k); codePos=k; return; }
#endif
// ... default (shipping) path below ...
```

Build both variants (`-DARMRX_NO_E24_PAD=ON` vs default), KAT + stress both, measure both,
compare. This is how `ab-e24-8w.md` was produced — the flag is now a permanent, documented
A/B switch and costs nothing when OFF. Use this pattern for EVERY trial-and-error experiment
in Era II.

---

## 7. Baselines to beat (from the referenced measurements)

| metric | armrx | XMRig | source |
|---|---:|---:|---|
| 1w H/s | 5.11 | 5.04 | cross build + E24 |
| 8w H/s | 26.65 | 28.0 | real pool, 1209s |
| 8w parity | **95.2%** | 100% | — |
| **instr/hash (gated 500-hash window, 1w AND 8w)** | **113.8M** | 98.9M (M1 est ±5%) | p0-instruction-count-resolution.md / m1-miner-to-miner-pmu-diff.md |
| 1w IPC (gated) | 0.662 | 0.654 | p0 / w11 census |

NOTE: the w11 census figure "armrx 89.5M / IPC 0.547" is **superseded** by Phase 0's gated-window
measurement (113.8M, IPC 0.662) — the census used a mis-divided hash count. armrx is **+15%
heavier** than XMRig on instructions; the 95.2% gap is cluster-contention throughput loss, not a
per-worker instr/IPC difference. See STRATEGY.md / p0-instruction-count-resolution.md.

See STRATEGY.md / p0-instruction-count-resolution.md — Phase 0 resolved the lever to
**instruction count** (armrx +15% heavier); the IPC branch is deprioritized.
