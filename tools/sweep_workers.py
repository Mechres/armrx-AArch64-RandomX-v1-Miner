#!/usr/bin/env python3
"""
Worker-count sweep for armrx big.LITTLE devbox.

Topology (MSM8916 Snapdragon 410 — all Cortex-A53, two clock domains):
  Cluster 0 = cpu0-cpu3  (HIGH-freq, "big" in user's framing)
  Cluster 1 = cpu4-cpu7  (LOW-freq,  "little" in user's framing)
  Measured ratio: cpu0 ~4.04 H/s / cpu4 ~2.27 H/s ≈ 1.78× per-core

Configs:
  C        : taskset -c 0-3   --workers=4  (4 big, 0 little)
  B5       : taskset -c 0-4   --workers=5  (4 big, 1 little)
  B6       : taskset -c 0-5   --workers=6  (4 big, 2 little)
  B7       : taskset -c 0-6   --workers=7  (4 big, 3 little)
  A_pinned : taskset -c 0-7   --workers=8  (4 big, 4 little, fully pinned)
  A_unpin  : (no taskset)     --workers=8  (OS-scheduled, 8 workers)

Each config: REPEATS x DURATION seconds.  3 repeats x 180s = 9 min/config.
Total: 6 configs x 9 min ~ 54 minutes.
"""

import subprocess
import re
import statistics
import sys
import time
import datetime

BINARY      = "./armrx"
DURATION    = 180     # seconds per run
WARMUP      = 40      # seconds before steady-state snapshot is taken
REPEATS     = 3

CONFIGS = [
    # (label,  taskset_cpus or None,  workers)
    ("C",          "0-3",  4),
    ("B5",         "0-4",  5),
    ("B6",         "0-5",  6),
    ("B7",         "0-6",  7),
    ("A_pinned",   "0-7",  8),
    ("A_unpin",    None,   8),
]


def run_one(cpus, workers, duration, warmup):
    """Run one armrx benchmark. Returns (steady_total, per_worker_list) or (None, None)."""
    cmd = []
    if cpus is not None:
        cmd += ["taskset", "-c", cpus]
    cmd += [
        BINARY,
        "--mine",
        "--mode=light",
        f"--workers={workers}",
        f"--seconds={duration}",
        f"--warmup={warmup}",
    ]
    print(f"  $ {' '.join(cmd)}", flush=True)
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=duration + 120)
        out = result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        print("    [TIMEOUT]", flush=True)
        return None, None

    # Parse "Steady-state hashrate: 17.34 H/s (measured over 140s post-warmup)"
    m = re.search(r"Steady-state hashrate:\s*([\d.]+)\s*H/s", out)
    if not m:
        # Fallback: compute from Total Hashes / time, less accurate
        mh = re.search(r"Total Hashes computed:\s*(\d+)", out)
        mt = re.search(r"Time:\s*(\d+)s", out)
        if mh and mt:
            total = int(mh.group(1))
            t = int(mt.group(1))
            steady = total / t if t > 0 else 0.0
            print(f"    [fallback] {steady:.2f} H/s from {total} hashes in {t}s", flush=True)
            return steady, []
        print(f"    [PARSE ERROR] output:\n{out[-1000:]}", flush=True)
        return None, None

    steady = float(m.group(1))

    # Parse per-worker lines: "  worker[N]: X.XX H/s"
    pw = []
    for wm in re.finditer(r"worker\[(\d+)\]:\s*([\d.]+)\s*H/s", out):
        pw.append((int(wm.group(1)), float(wm.group(2))))
    pw.sort(key=lambda x: x[0])
    per_worker = [x[1] for x in pw]

    print(f"    steady={steady:.2f} H/s  per_worker={[f'{v:.2f}' for v in per_worker]}", flush=True)
    return steady, per_worker


def sweep():
    results = {}

    for label, cpus, workers in CONFIGS:
        big_count    = min(workers, 4)
        little_count = max(workers - 4, 0)
        print(f"\n{'='*60}", flush=True)
        print(f"CONFIG: {label}  workers={workers}  (big={big_count} little={little_count})", flush=True)
        print(f"{'='*60}", flush=True)

        totals          = []
        per_worker_runs = []

        for rep in range(1, REPEATS + 1):
            print(f"\n  --- Repeat {rep}/{REPEATS} ---", flush=True)
            t, pw = run_one(cpus, workers, DURATION, WARMUP)
            if t is not None:
                totals.append(t)
                per_worker_runs.append(pw)
            else:
                print("  [SKIPPED due to error]", flush=True)
            if rep < REPEATS:
                print("  [cooldown 30s]", flush=True)
                time.sleep(30)

        results[label] = {
            "workers": workers,
            "big": big_count,
            "little": little_count,
            "totals": totals,
            "per_worker_runs": per_worker_runs,
        }

    # Summary table
    print("\n\n" + "=" * 70, flush=True)
    print("SWEEP RESULTS -- armrx big.LITTLE worker-count sweep", flush=True)
    print(f"Duration: {DURATION}s/run | Warmup: {WARMUP}s | Repeats: {REPEATS}", flush=True)
    print("=" * 70, flush=True)

    print(f"\n{'Label':<12} {'W':>3} {'Big':>4} {'Lit':>4}  {'Mean H/s':>10} {'Stddev':>8} {'Min':>8} {'Max':>8}", flush=True)
    print("-" * 70, flush=True)

    csv_lines = ["label,workers,big,little,mean_hs,stddev_hs,min_hs,max_hs,runs"]
    best_label, best_mean = None, 0.0

    for label, cpus, workers in CONFIGS:
        r = results[label]
        totals = r["totals"]
        if not totals:
            print(f"{label:<12} {workers:>3} {r['big']:>4} {r['little']:>4}  {'N/A':>10}", flush=True)
            continue
        mean = statistics.mean(totals)
        sd   = statistics.stdev(totals) if len(totals) > 1 else 0.0
        mn   = min(totals)
        mx   = max(totals)
        print(f"{label:<12} {workers:>3} {r['big']:>4} {r['little']:>4}  {mean:>10.2f} {sd:>8.2f} {mn:>8.2f} {mx:>8.2f}", flush=True)
        csv_lines.append(f"{label},{workers},{r['big']},{r['little']},{mean:.2f},{sd:.2f},{mn:.2f},{mx:.2f},{len(totals)}")
        if mean > best_mean:
            best_mean, best_label = mean, label

    print("-" * 70, flush=True)
    print(f"\nBEST POLICY: {best_label}  ({best_mean:.2f} H/s mean)", flush=True)

    # Per-worker breakdown
    print("\n\nPER-WORKER BREAKDOWN (mean over repeats):", flush=True)
    for label, cpus, workers in CONFIGS:
        r = results[label]
        runs = r.get("per_worker_runs", [])
        if not runs or not runs[0]:
            continue
        n_workers = len(runs[0])
        worker_means = []
        for wi in range(n_workers):
            vals = [run[wi] for run in runs if wi < len(run)]
            if vals:
                worker_means.append(statistics.mean(vals))
        cluster_boundary = 4
        big_workers    = worker_means[:cluster_boundary]
        little_workers = worker_means[cluster_boundary:]
        big_mean = statistics.mean(big_workers)    if big_workers    else 0.0
        lit_mean = statistics.mean(little_workers) if little_workers else 0.0
        print(f"  {label:<12}: big-avg={big_mean:.2f} H/s/thread  "
              f"little-avg={lit_mean:.2f} H/s/thread  "
              f"per-thread={[f'{v:.2f}' for v in worker_means]}", flush=True)

    # Write CSV
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_path = f"sweep_results_{ts}.csv"
    with open(csv_path, "w") as f:
        f.write("\n".join(csv_lines) + "\n")
    print(f"\nCSV written to: {csv_path}", flush=True)

    return results


if __name__ == "__main__":
    sweep()
