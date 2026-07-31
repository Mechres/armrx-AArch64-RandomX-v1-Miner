#!/bin/sh
# perf_ready_bench.sh — perf stat capture of ONLY the steady-state region of
# bench_armrx --full-hash-only, using the --perf-ready hook (audit T1-2).
# Usage: tools/perf_ready_bench.sh [perf stat args...]
# Example: tools/perf_ready_bench.sh -e cycles,instructions
set -e
FIFO=$(mktemp -u /tmp/bench-go.XXXXXX)
LOG=$(mktemp /tmp/bench-perf.XXXXXX.log)
mkfifo "$FIFO"
trap 'rm -f "$FIFO" "$LOG"' EXIT

./build/bench_armrx --full-hash-only --perf-ready <> "$FIFO" > "$LOG" 2>&1 &
BENCH=$!

# Wait for the readiness marker (cache init + JIT + warmup all done).
i=0
while ! grep -q PERF_READY "$LOG" 2>/dev/null; do
    sleep 0.2
    i=$((i+1))
    if [ $i -gt 500 ]; then echo "timeout waiting for PERF_READY" >&2; kill $BENCH; exit 1; fi
done

# Attach perf, then release the measured loop.
perf stat "$@" -p "$BENCH" > /tmp/perf-ready.txt 2>&1 &
PERFPID=$!
sleep 0.5
echo go > "$FIFO"
wait "$BENCH"
wait "$PERFPID"
cat /tmp/perf-ready.txt
echo "--- bench log tail ---"
tail -5 "$LOG"
