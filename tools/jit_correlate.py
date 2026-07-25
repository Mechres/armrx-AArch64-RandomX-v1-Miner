#!/usr/bin/env python3
"""
Correlates `perf record` sample addresses against armrx's own superscalar
JIT opcode boundary table (PLAN.md Phase 6 item 14) to get real
opcode-level cycle attribution *inside* the JIT-generated
dataset-derivation code -- something neither `perf report`'s generic
symbolization nor a static instruction count alone can produce.

Self-analysis only: this reads armrx's own `--jit-dump` output and its
own process's `/proc/<pid>/maps`. No other miner's code or binaries are
involved (see PLAN.md's clean-room boundary note, Phase 6, after item 13).

Why this works: `generateSuperscalarHash()` compiles once per seed
rotation and its output is reused unmodified for the rest of that seed's
duration (unlike the per-hash VM program, which is regenerated every
hash and can't be correlated the same way against a single dump). So a
single `--jit-dump` snapshot's offset table validly describes every
`perf` sample landing in the superscalar sub-region, for as long as the
mining run being profiled didn't rotate seeds.

Usage:
    ./armrx --jit-dump > jit_dump.txt
    perf record -F 999 -g -o perf.data -- ./armrx --mine --workers=N --seconds=S --warmup=W &
    sleep <past warmup>
    # `pgrep -f` is unreliable here -- it matches the *invoking shell's*
    # command line too if that also contains "./armrx --mine" (e.g. when
    # this whole sequence is itself passed to `sh -c '...'`). Match on
    # /proc/<pid>/comm instead, which is exactly "armrx" for the real
    # process and unaffected by how it was invoked:
    for p in /proc/[0-9]*; do
        [ "$(cat "$p/comm" 2>/dev/null)" = "armrx" ] && cat "$p/maps" > maps.txt && break
    done
    wait
    perf script -i perf.data -F ip > samples.txt
    python3 tools/jit_correlate.py --jit-dump jit_dump.txt --maps maps.txt --samples samples.txt
"""
import argparse
import bisect
import re
import sys
from collections import defaultdict


def parse_jit_dump(path):
    """Returns (total_buf_size, code_size, entries) where entries is a
    list of (offset, size, opcode_name) sorted by offset, describing the
    superscalar/dataset-derivation sub-region only."""
    total_buf_size = None
    code_size = None
    entries = []
    in_table = False
    row_re = re.compile(
        r"^\s*\d+\s*\|\s*(\d+)\s*\|\s*(\S+)\s*\|\s*([0-9a-fA-F]+)\s*\|\s*(\d+)\s*$"
    )
    with open(path) as f:
        for line in f:
            if total_buf_size is None:
                m = re.search(r"Total allocated buffer size.*:\s*(\d+)\s*bytes", line)
                if m:
                    total_buf_size = int(m.group(1))
                    continue
            if code_size is None:
                m = re.search(r"^CodeSize \(.*\):\s*(\d+)\s*bytes", line)
                if m:
                    code_size = int(m.group(1))
                    continue
            if "Superscalar opcode boundary table" in line:
                in_table = True
                continue
            if in_table:
                m = row_re.match(line)
                if m:
                    _opcode_id, name, offset_hex, size = m.groups()
                    entries.append((int(offset_hex, 16), int(size), name))
    entries.sort(key=lambda e: e[0])
    return total_buf_size, code_size, entries


def parse_maps(path, expected_size, tolerance, page_size=4096):
    """Returns a list of (start, end) for every individual worker JIT
    buffer found in `path`'s executable anonymous mappings.

    mmap/mprotect round up to whole pages, so the real per-buffer size is
    `expected_size` rounded up to `page_size`. THP (`always` policy on
    this device) frequently merges several adjacent, same-permission
    worker buffers into one contiguous VMA -- observed directly: buffers
    of 1x, 2x, and 4x the per-buffer size all appear in the same /proc/maps
    snapshot. A region whose size is a whole multiple of the per-buffer
    size is therefore split into that many equal sub-regions, each
    treated as an independent buffer base -- otherwise samples from the
    2nd/3rd/4th worker sharing a merged region would be computed against
    the wrong (region-start, not buffer-start) base and misattributed.
    """
    buf_size = ((expected_size + page_size - 1) // page_size) * page_size
    regions = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            addr_range, perms = parts[0], parts[1]
            if "x" not in perms:
                continue
            try:
                start_s, end_s = addr_range.split("-")
                start, end = int(start_s, 16), int(end_s, 16)
            except ValueError:
                continue
            size = end - start
            if size < buf_size * (1 - tolerance):
                continue
            n = round(size / buf_size)
            if n >= 1 and abs(size - n * buf_size) <= buf_size * tolerance:
                for k in range(n):
                    regions.append((start + k * buf_size, start + (k + 1) * buf_size))
    # de-dup identical ranges (maps snapshotted from multiple pids may overlap/repeat)
    return sorted(set(regions))


def parse_samples(path):
    """`perf script -F ip` output. Because perf record used `-g`
    (call-graph), each sample is a *block* of one or more address lines
    (leaf frame first, then unwound callers) separated by blank lines --
    not one address per line. Only the first address line of each block
    is the actual sampled PC; the rest are ancestor frames and would
    inflate/skew the count if counted as independent samples."""
    addrs = []
    at_block_start = True
    with open(path) as f:
        for line in f:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                at_block_start = True
                continue
            if at_block_start:
                tok = stripped.split()[-1]
                try:
                    addrs.append(int(tok, 16))
                except ValueError:
                    pass
                at_block_start = False
    return addrs


def find_region_base(addr, regions):
    # regions list is small (one per worker); linear scan is fine
    for start, end in regions:
        if start <= addr < end:
            return start
    return None


def make_opcode_lookup(entries):
    offsets = [e[0] for e in entries]

    def lookup(rel_offset):
        idx = bisect.bisect_right(offsets, rel_offset) - 1
        if idx < 0:
            return None
        off, size, name = entries[idx]
        if off <= rel_offset < off + size:
            return name
        return None

    return lookup


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jit-dump", required=True, help="output of `./armrx --jit-dump`")
    ap.add_argument("--maps", required=True, help="concatenated /proc/<pid>/maps snapshot(s)")
    ap.add_argument("--samples", required=True, help="output of `perf script -F ip`")
    ap.add_argument("--tolerance", type=float, default=0.20,
                     help="fractional size tolerance when matching maps regions (default 0.20)")
    args = ap.parse_args()

    total_buf_size, code_size, entries = parse_jit_dump(args.jit_dump)
    if total_buf_size is None:
        print("error: could not find 'Total allocated buffer size' in --jit-dump output "
              "(rebuild with the item-14 dumpJitCode() extension)", file=sys.stderr)
        sys.exit(1)
    if not entries:
        print("error: no superscalar opcode boundary table entries found in --jit-dump output",
              file=sys.stderr)
        sys.exit(1)

    regions = parse_maps(args.maps, total_buf_size, args.tolerance)
    if not regions:
        print(f"error: no /proc/maps executable regions matched expected buffer size "
              f"{total_buf_size} bytes (+/-{args.tolerance*100:.0f}%)", file=sys.stderr)
        sys.exit(1)

    addrs = parse_samples(args.samples)
    if not addrs:
        print("error: no samples parsed from perf script output", file=sys.stderr)
        sys.exit(1)

    lookup_opcode = make_opcode_lookup(entries)

    outside_buffer = 0
    # PLAN.md Phase 7 item 2 narrowing (2026-07-25): code_size is the exact
    # buffer-absolute boundary between the main per-hash VM program region
    # (offsets < code_size) and the superscalar/dataset-derivation region
    # (offsets >= code_size) -- dumpJitCode() says so explicitly ("offsets
    # below this are NOT superscalar") and generateSuperscalarHash() starts
    # its own codePos at exactly CodeSize, so superscalar_jit_dump_ entries'
    # offsets are already buffer-absolute, directly comparable to rel. This
    # was parsed but never used to classify samples -- splitting on it turns
    # the old single "in_buffer_unattributed" bucket (originally described as
    # "static wrapper / per-hash VM program region, or un-instrumented fixed
    # superscalar template bytes" -- three different things conflated) into
    # two precisely-separated ones.
    main_vm_region = 0
    ss_region_unattributed = 0
    attributed = defaultdict(int)

    for addr in addrs:
        base = find_region_base(addr, regions)
        if base is None:
            outside_buffer += 1
            continue
        rel = addr - base
        if code_size is not None and rel < code_size:
            main_vm_region += 1
            continue
        name = lookup_opcode(rel)
        if name:
            attributed[name] += 1
        else:
            ss_region_unattributed += 1

    total = len(addrs)
    in_buffer = total - outside_buffer
    in_buffer_unattributed = main_vm_region + ss_region_unattributed
    ss_total = sum(attributed.values())
    ss_region_total = ss_region_unattributed + ss_total

    print(f"Total samples parsed:            {total}")
    print(f"Superscalar buffer size (ground truth): {total_buf_size} bytes"
          + (f"  (CodeSize={code_size})" if code_size else ""))
    print(f"Matched worker buffers in maps:   {len(regions)}")
    for s, e in regions:
        print(f"    0x{s:x}-0x{e:x}  ({e - s} bytes)")
    print()
    print(f"Outside any worker JIT buffer (named C++, libc, etc.): "
          f"{outside_buffer:>8} ({outside_buffer/total*100:6.2f}% of total)")
    print(f"Inside a worker JIT buffer, total:                     "
          f"{in_buffer:>8} ({in_buffer/total*100:6.2f}% of total)")
    print(f"  -- main per-hash VM program region (offset < CodeSize, "
          f"regenerated every hash, not opcode-attributable by this method): "
          f"{main_vm_region:>8} ({main_vm_region/total*100:6.2f}% of total, "
          f"{main_vm_region/in_buffer*100 if in_buffer else 0:6.2f}% of in-buffer)")
    print(f"  -- superscalar/dataset-derivation region (offset >= CodeSize), total: "
          f"{ss_region_total:>8} ({ss_region_total/total*100:6.2f}% of total, "
          f"{ss_region_total/in_buffer*100 if in_buffer else 0:6.2f}% of in-buffer)")
    print(f"       of which, matched a superscalar opcode entry:     "
          f"{ss_total:>8} ({ss_total/total*100:6.2f}% of total, "
          f"{ss_total/ss_region_total*100 if ss_region_total else 0:6.2f}% of ss-region)")
    print(f"       of which, unattributed (fixed wrapper chunks -- entry/prefetch/"
          f"jump/mix/reg-update/store, not individually dumped -- or un-instrumented "
          f"fixed template bytes): "
          f"{ss_region_unattributed:>8} ({ss_region_unattributed/total*100:6.2f}% of total, "
          f"{ss_region_unattributed/ss_region_total*100 if ss_region_total else 0:6.2f}% of ss-region)")
    print()
    print(f"{'opcode':<12} {'samples':>10} {'% of ss-attributed':>19} {'% of total':>12}")
    for name, count in sorted(attributed.items(), key=lambda kv: -kv[1]):
        pct_ss = count / ss_total * 100 if ss_total else 0
        pct_total = count / total * 100 if total else 0
        print(f"{name:<12} {count:>10} {pct_ss:>18.2f}% {pct_total:>11.2f}%")


if __name__ == "__main__":
    main()
