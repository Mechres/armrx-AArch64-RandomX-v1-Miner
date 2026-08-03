#!/usr/bin/env python3
"""E3b harness: aggregate armrx per-opcode JIT emission size across many seeds.

Runs `armrx --jit-dump=<seed>` for N seeds, parses the two boundary tables
(main-VM "Opcode boundary table" and superscalar "Superscalar opcode boundary
table"), and aggregates average bytes/occurrence per opcode. Bytes/4 = instructions.

Output: per-opcode avg instructions, total occurrences, share of total bytes.
Flagged: opcodes whose avg instr exceeds a structural minimum (hand-annotated).
"""
import subprocess, re, sys, statistics

SEEDS = [f"{i:032X}"[:32] for i in range(40)]  # 40 deterministic-ish seeds
BIN = "/tmp/cl_test/armrx"

# Structural minimum instructions for each RandomX opcode on AArch64 (best case).
# 1 = single mov/alu; 2 = two-op; 3 = common (swap, CBRANCH-min); etc.
# These are the FEWEST AArch64 instructions to implement the RandomX semantic.
MIN_INSTR = {
    "IADD_R":1,"IADD_M":3,"IADD_RS":2,"IADD_C":2,
    "ISUB_R":1,"ISUB_M":3,
    "IMUL_R":1,"IMUL_M":3,
    "IMULH_R":1,"IMULH_M":3,
    "ISMULH_R":1,"ISMULH_M":3,
    "IMUL_RCP":2,           # load 1.0 constant + mul
    "INEG_R":1,             # sub dst,xzr,dst
    "IXOR_R":1,"IXOR_M":3,
    "IXOR_C":2,             # load const + xor
    "IROR_R":1,"IROR_M":3,
    "IROL_R":1,"IROL_M":3,
    "ISWAP_R":3,            # 3-mov swap (minimum on AArch64 without xor-swap)
    "FSWAP_R":1,
    "FADD_R":1,"FADD_M":3,
    "FSUB_R":1,"FSUB_M":3,
    "FMUL_R":1,"FMUL_M":3,
    "FDIV_R":2,             # NEON fdiv is 1 op; allow 2
    "FSQRT_R":1,
    "FSCAL_R":1,
    "CBRANCH":2,            # conditional swap = 2-3 instr minimum
    "ISTORE":3,"FSTORE":3,  # compute addr + store (min 2-3)
    "NOP":1,
}

def parse_dump(text):
    """Return (mainvm: {name:(occ,bytes)}, ss: {name:(occ,bytes)}, total_instr)."""
    main = {}
    ss = {}
    total = None
    # total instructions
    m = re.search(r"Total instructions:\s*(\d+)", text)
    if m: total = int(m.group(1))
    # two boundary tables; distinguish by header
    sections = re.split(r"--- (?:Opcode|Superscalar opcode) boundary table ---", text)
    # sections[1] = main-vm, sections[2] = superscalar (if present)
    def grab(block):
        d = {}
        # row:  #  | opcode_id | name        | offset  | size
        # actual: "0000 | 000000107 | 00000IROR_R | 0001e0 |    4"
        for mm in re.finditer(r"\d+\s*\|\s*\d+\s*\|\s*(\S+)\s*\|\s*[0-9a-fA-F]+\s*\|\s*(\d+)", block):
            name, size = mm.group(1), int(mm.group(2))
            d[name] = d.get(name, [0,0]); d[name][0]+=1; d[name][1]+=size
        return d
    if len(sections) >= 2: main = grab(sections[1])
    if len(sections) >= 3: ss = grab(sections[2])
    return main, ss, total

def main():
    main_agg = {}   # name -> [occ, bytes]
    ss_agg = {}
    totals = []
    for i,s in enumerate(SEEDS):
        try:
            r = subprocess.run([BIN, f"--jit-dump={s}"], capture_output=True, text=True, timeout=25)
            out = r.stdout
            if r.returncode != 0:
                print(f"seed {s}: rc={r.returncode}", file=sys.stderr); continue
        except subprocess.TimeoutExpired as e:
            print(f"seed {s}: TIMEOUT", file=sys.stderr)
            if e.stdout: out = e.stdout
            else: continue
        m, ss, t = parse_dump(out)
        for n,(o,b) in m.items():
            main_agg.setdefault(n,[0,0]); main_agg[n][0]+=o; main_agg[n][1]+=b
        for n,(o,b) in ss.items():
            ss_agg.setdefault(n,[0,0]); ss_agg[n][0]+=o; ss_agg[n][1]+=b
        if t: totals.append(t)
        if (i+1) % 5 == 0: print(f"  ...{i+1} seeds, avg_instr={statistics.mean(totals):.0f}", file=sys.stderr)

    print(f"Seeds parsed: {len(totals)}")
    print(f"Avg total instructions/program: {statistics.mean(totals):.0f}  (range {min(totals)}-{max(totals)})")
    print()
    print("=== MAIN-VM opcode emission (avg instr/occurrence, flag if > structural min) ===")
    print(f"{'opcode':12} {'occ':6} {'avgB':6} {'avgI':6} {'min':4} {'flag':6}")
    for n in sorted(main_agg, key=lambda x:-main_agg[x][1]):
        o,b = main_agg[n]; avgb=b/o; avgi=b/o/4; mn=MIN_INSTR.get(n,1)
        flag = "OVER" if avgi > mn+0.5 else ""
        print(f"{n:12} {o:6} {avgb:6.0f} {avgi:6.1f} {mn:<4} {flag:6}")
    print()
    print("=== SUPERSCALAR opcode emission ===")
    print(f"{'opcode':12} {'occ':6} {'avgB':6} {'avgI':6} {'min':4} {'flag':6}")
    for n in sorted(ss_agg, key=lambda x:-ss_agg[x][1]):
        o,b = ss_agg[n]; avgb=b/o; avgi=b/o/4; mn=MIN_INSTR.get(n,1)
        flag = "OVER" if avgi > mn+0.5 else ""
        print(f"{n:12} {o:6} {avgb:6.0f} {avgi:6.1f} {mn:<4} {flag:6}")

if __name__ == "__main__":
    main()
