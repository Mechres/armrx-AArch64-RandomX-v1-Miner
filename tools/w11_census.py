#!/usr/bin/env python3
"""
w11_census.py — W1-1 region instruction census: classify `perf script` samples
into the five region buckets and report M/hash against the confirmed total.

Self-analysis only: reads armrx's own process maps + perf samples; no other
miner's code is involved (clean-room boundary, same as jit_correlate.py).

Inputs (all produced on the device by tools/w11_census_device.sh):
  --samples   `perf script -F ip,dso,sym -i perf.data` text (one line per sample:
              `0x<ip> <dso> <sym>`; [unknown] dso = JIT code)
  --maps      /proc/<pid>/maps snapshot taken during the measured window
  --total     confirmed total instructions (or cycles) for the whole window,
              from the perf stat pass (used for M/hash conversion)
  --hashes    number of hashes in the measured window (default 500)

Region model (buffer geometry is baked into the census binary, derived from
its own symtab — `nm` on the deployed binary):
  CodeSize = randomx_init_dataset_aarch64_end - randomx_program_aarch64
           = 0x3697c - 0x2a040 = 0xC93C = 51516 bytes
  The JIT buffer (one per worker VM) is an executable anonymous mapping of
  CodeSize + CalcDatasetItemSize bytes; offsets < CodeSize are the per-hash
  main-VM program region (regenerated every hash), offsets >= CodeSize are the
  superscalar/dataset-derivation region (compiled once per seed).
"""
import argparse
import collections
import re
import sys

# Geometry of the deployed census binary (nm-verified 2026-08-01):
#   randomx_program_aarch64            0x2a040
#   randomx_init_dataset_aarch64_end   0x3697c  -> CodeSize = 51516
CODE_SIZE = 0xC93C  # 51516 bytes

# Named C++ symbol buckets (bucket 4 sub-attribution).
AES_SYMS = ("hash_aes", "fill_aes", "aes_", "soft_aes", "AesHash", "AesGenerator",
            "encrypt", "decrypt", "round", "ttable", "TTable", "AES", "aes")
BLAKE_SYMS = ("blake2b", "Blake2b")
ARGON_SYMS = ("argon2", "Argon2", "Argon")
VM_SYMS = ("calculate_hash", "randomx_calculate_hash", "execute", "VirtualMachine",
           "vm_", "generateSuperscalar", "Superscalar", "superscalar", "dataset",
           "Dataset", "init_cache", "initCache", "Cache", "cache")


def parse_maps(path):
    """Return list of (start, end, perms, path) for executable mappings."""
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
            path = parts[-1] if len(parts) > 5 and not parts[-1].startswith("0x") else ""
            regions.append((start, end, perms, path))
    return regions


def find_jit_buffers(regions, min_size=0x8000):
    """Executable anonymous mappings (no file path) of plausible JIT size."""
    bufs = []
    for start, end, perms, path in regions:
        if path:
            continue
        size = end - start
        if size < min_size:
            continue
        bufs.append((start, end, perms))
    return bufs


def sym_bucket(sym):
    if not sym or sym == "[unknown]":
        return "other/unknown"
    s = sym
    for pat in AES_SYMS:
        if pat in s:
            return "AES"
    for pat in BLAKE_SYMS:
        if pat in s:
            return "Blake2b"
    for pat in ARGON_SYMS:
        if pat in s:
            return "Argon2"
    for pat in VM_SYMS:
        if pat in s:
            return "VM/superscalar glue"
    return "other C++"


def parse_samples(path):
    """perf script -F ip,dso,sym output on perf 7.x produces lines of the form
    `     aaaab36d33f8 armrx::fill_aes_1r_x4(...) (/tmp/cross/bench_armrx)`
    i.e. `ip sym (dso)` — the dso is in trailing parentheses and the symbol
    may itself contain spaces/parens (C++ template/mangled names). Returns a
    list of (ip, dso, sym)."""
    samples = []
    line_re = re.compile(r"^\s*([0-9a-fA-F]+)\s+(.*?)\s+\(([^)]+)\)\s*$")
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.strip().startswith("#"):
                continue
            m = line_re.match(line)
            if not m:
                continue
            ip = int(m.group(1), 16)
            sym = m.group(2)
            dso = m.group(3)
            samples.append((ip, dso, sym))
    return samples


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--samples", required=True)
    ap.add_argument("--maps", required=True)
    ap.add_argument("--total", required=True, type=float,
                    help="perf stat total (instructions or cycles) for the window")
    ap.add_argument("--hashes", type=int, default=500)
    ap.add_argument("--label", default="instructions",
                    help="label for the pass (instructions|cycles)")
    args = ap.parse_args()

    regions = parse_maps(args.maps)
    bufs = find_jit_buffers(regions)
    if not bufs:
        print("error: no executable anonymous JIT buffer found in maps", file=sys.stderr)
        sys.exit(1)

    samples = parse_samples(args.samples)
    if not samples:
        print("error: no samples parsed", file=sys.stderr)
        sys.exit(1)

    buckets = collections.Counter()          # region buckets
    sub4 = collections.Counter()             # bucket-4 symbol sub-buckets
    out_of_buffer = 0

    for ip, dso, sym in samples:
        base = None
        for s, e, _perms in bufs:
            if s <= ip < e:
                base = s
                break
        if base is None:
            # Not in a JIT buffer: named C++ / libc / unattributed.
            if dso and dso != "[unknown]":
                # executable file dso (bench binary or libc)
                if "bench_armrx" in dso or "armrx" in dso:
                    buckets["4_named_cpp"] += 1
                    sub4[sym_bucket(sym)] += 1
                elif "libc" in dso or "ld-musl" in dso or "libm" in dso:
                    buckets["4_named_cpp"] += 1
                    sub4["libc"] += 1
                else:
                    buckets["5_unattributed"] += 1
            else:
                buckets["5_unattributed"] += 1
            out_of_buffer += 1
            continue
        rel = ip - base
        if rel < CODE_SIZE:
            buckets["3_main_vm_jit"] += 1
        else:
            buckets["1_2_superscalar_region"] += 1

    total = len(samples)
    per_hash = args.total / args.hashes
    print(f"Pass: {args.label}  |  total samples: {total}  |  "
          f"window total: {args.total:.0f} over {args.hashes} hashes "
          f"({per_hash/1e6:.2f} M/hash)")
    print(f"JIT buffer(s) found in maps: "
          + ", ".join(f"0x{s:x}-0x{e:x} ({e-s} B, {p})" for s, e, p in bufs))
    print(f"CodeSize (main-VM / superscalar boundary): {CODE_SIZE} bytes")
    print()
    header = f"{'bucket':<28} {'samples':>10} {'share%':>8} {'M/hash':>10} {'%total':>7}"
    print(header)
    order = ["1_2_superscalar_region", "3_main_vm_jit", "4_named_cpp", "5_unattributed"]
    for b in order:
        n = buckets[b]
        print(f"{b:<28} {n:>10} {n/total*100:>7.2f}% {n/total*per_hash/1e6:>9.2f}M {n/total*100:>6.2f}%")
    print()
    print("bucket 4 sub-attribution (named symbols):")
    for k, n in sub4.most_common():
        print(f"  {k:<24} {n:>10}  {n/total*100:>6.2f}% of total  "
              f"{n/total*per_hash/1e6:>7.2f}M/hash")


if __name__ == "__main__":
    main()
