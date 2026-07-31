#!/bin/sh
# w11_census_device.sh — W1-1 region instruction census, device-side capture.
#
# Runs bench_armrx --full-hash-only --perf-ready (the T1-2 steady-state hook) on
# fast-cluster core 3, attaches perf ONLY after PERF_READY, releases the 500-hash
# measured loop with a fifo go-line, and captures:
#   stat  -> perf stat -e cycles,instructions total (clean instr/hash confirmation)
#   instr -> perf record -F 999 -e instructions:u (sample pass #1)
#   cycles-> perf record -F 999 -e cycles:u       (sample pass #2)
# The record passes also snapshot /proc/<pid>/maps twice during the measured
# window (JIT buffer identification; layout is stable once PERF_READY has printed).
#
# POSIX sh only (BusyBox ash on the device — no bash, no arrays, no [[ ]]).
# Usage on device:  sh w11_census_device.sh <stat|instr|cycles>
# Output files on device:
#   /tmp/census-stat.txt            (stat mode: perf stat text)
#   /tmp/census-<mode>.data         (record modes: perf.data)
#   /tmp/census-maps-<mode>-1/2.txt (record modes: maps snapshots)
#   /tmp/census-<mode>.log          (record modes: bench log)
# The binary profiled must be /tmp/cross/bench_armrx (md5 257e6d14...); verify
# with md5sum before each run — do NOT substitute another binary.
set -e

MODE=$1
case "$MODE" in
    stat|instr|cycles) ;;
    *) echo "usage: $0 <stat|instr|cycles>" >&2; exit 2 ;;
esac

# Device hygiene: refuse to run if a previous bench/perf is still alive.
for p in /proc/[0-9]*; do
    c=$(cat "$p/comm" 2>/dev/null)
    case "$c" in
        bench_armrx|perf) echo "stray process $p ($c) — kill it first" >&2; exit 1 ;;
    esac
done

cd /tmp
FIFO=/tmp/census-go.$$
LOG=/tmp/census-$MODE.log
mkfifo "$FIFO"
trap 'rm -f "$FIFO"' EXIT

taskset -c 3 /tmp/cross/bench_armrx --full-hash-only --perf-ready \
    --perf-ready-timeout=900 <> "$FIFO" > "$LOG" 2>&1 &
BENCH=$!

# Wait for the readiness marker (256 MiB Argon2 init + JIT compile + 30-hash
# warmup all complete; steady-state measured loop is gated on stdin).
i=0
while ! grep -q PERF_READY "$LOG" 2>/dev/null; do
    sleep 1
    i=$((i+1))
    if [ $i -gt 1200 ]; then
        echo "timeout waiting for PERF_READY" >&2
        kill "$BENCH" 2>/dev/null
        exit 1
    fi
done

case "$MODE" in
    stat)
        perf stat -e cycles,instructions -p "$BENCH" > /tmp/census-stat.txt 2>&1 &
        PERFPID=$!
        sleep 0.5
        echo go > "$FIFO"
        wait "$BENCH"
        wait "$PERFPID" || true
        echo "=== perf stat ==="
        cat /tmp/census-stat.txt
        ;;
    instr|cycles)
        EV=instructions
        [ "$MODE" = cycles ] && EV=cycles
        perf record -F 999 -e "$EV":u -p "$BENCH" -o /tmp/census-$MODE.data \
            > /tmp/census-record-$MODE.log 2>&1 &
        PERFPID=$!
        sleep 0.5
        echo go > "$FIFO"
        # Two maps snapshots during the measured window (buffer is stable).
        sleep 2
        cat "/proc/$BENCH/maps" > /tmp/census-maps-$MODE-1.txt
        sleep 25
        cat "/proc/$BENCH/maps" > /tmp/census-maps-$MODE-2.txt
        wait "$BENCH"
        wait "$PERFPID" || true
        echo "=== perf record log ==="
        cat /tmp/census-record-$MODE.log
        ;;
esac

echo "=== bench log tail ==="
tail -6 "$LOG"
