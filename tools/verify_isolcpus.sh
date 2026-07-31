#!/bin/sh
# Verify isolcpus-aware worker pinning on device.
# Usage: sh verify_isolcpus.sh   (run on device, miner NOT yet running)
set -e
cd ~/armrx/build

# 1. Launch miner (no taskset — engine must self-pin)
nohup ./armrx --mine --mode=light > /tmp/mine-verify.log 2>&1 &
MINER=$!
echo "miner pid: $MINER"

# 2. Wait for mining to start producing hashes (up to 90s for cache init)
for i in $(seq 1 18); do
    sleep 5
    if grep -q 'H/s' /tmp/mine-verify.log; then break; fi
done

# 3. Show key log lines
echo '=== LOG (isolcpus + workers + hashrate) ==='
grep -iE 'isolcpus|Selected mode|H/s' /tmp/mine-verify.log | head -8

# 4. Thread placement
echo '=== THREAD PLACEMENT ==='
PID=$(pgrep -f 'armrx --min[e]' | head -1)
echo "PID: $PID"
for t in $(ls /proc/$PID/task/); do
    psr=$(awk '{print $39}' /proc/$PID/task/$t/stat 2>/dev/null)
    aff=$(taskset -pc $t 2>/dev/null | grep -o 'list:.*')
    echo "tid $t -> cpu $psr | aff $aff"
done

# 5. Kill miner
kill $MINER 2>/dev/null
echo '=== DONE ==='
