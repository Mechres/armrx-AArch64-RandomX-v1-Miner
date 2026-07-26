#!/usr/bin/env python3
"""
armrx devbox MCP server.

A single-file, stdlib-only MCP (Model Context Protocol) server that bridges to
an AArch64 development device over SSH, exposing curated build/test/bench tools
plus unrestricted shell. Runs wherever ZCode runs; talks to the device via the
system ssh/rsync/scp (no Paramiko, no pip deps).

Protocol: newline-delimited JSON-RPC 2.0 over stdio.
  - IMPORTANT: nothing may be written to stdout except protocol messages.
    All diagnostics go to stderr.

Configuration: read from $DEVBOX_CONFIG (a JSON file). See devbox.example.json.
"""

from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "armrx-devbox"
SERVER_VERSION = "1.0.0"

# Repo root = parent of the tools/devbox/ directory holding this file.
REPO_ROOT = Path(__file__).resolve().parents[2]


# ─────────────────────────────────────────────────────────────────────────────
# Logging — stderr only. stdout is the protocol channel.
# ─────────────────────────────────────────────────────────────────────────────

def log(msg: str) -> None:
    print(f"[devbox] {msg}", file=sys.stderr, flush=True)


# ─────────────────────────────────────────────────────────────────────────────
# Configuration
# ─────────────────────────────────────────────────────────────────────────────

DEFAULTS: dict[str, Any] = {
    "host": None,           # required
    "user": None,           # required
    "port": 22,
    "remote_dir": "~/armrx",
    "ssh_key": None,        # None = use ssh default / agent
    "ssh_password": None,   # None = key auth (recommended); else via sshpass -e
    "build_flags": ["-DARMRX_ENABLE_NATIVE=ON", "-DARMRX_BUILD_TESTS=ON"],
    "build_jobs": None,     # None = all available cores (-j)
    "timeout_s": {"default": 120, "build": 1800, "bench": 600, "test": 900},
    "destructive_guard": True,
}


class Config:
    """Loads and validates devbox.json."""

    def __init__(self) -> None:
        path = os.environ.get("DEVBOX_CONFIG")
        if not path:
            raise RuntimeError(
                "DEVBOX_CONFIG env var not set. Point it at a devbox.json "
                "(see devbox.example.json)."
            )
        p = Path(path).expanduser()
        if not p.is_file():
            raise RuntimeError(f"DEVBOX_CONFIG file not found: {p}")
        data = json.loads(p.read_text())
        merged = {**DEFAULTS, **data}
        if not merged["host"] or not merged["user"]:
            raise RuntimeError(
                "devbox.json must define 'host' and 'user'."
            )
        for k, v in merged.items():
            setattr(self, k, v)
        # normalize
        self.remote_dir = str(self.remote_dir)
        if not self.remote_dir.startswith("~") and not self.remote_dir.startswith("/"):
            self.remote_dir = "~/" + self.remote_dir

    def ssh_target(self) -> str:
        return f"{self.user}@{self.host}"

    def timeout(self, kind: str = "default") -> int:
        t = self.timeout_s or {}
        return int(t.get(kind, t.get("default", DEFAULTS["timeout_s"]["default"])))


# ─────────────────────────────────────────────────────────────────────────────
# SSH / rsync / scp helpers
# ─────────────────────────────────────────────────────────────────────────────

# Tool calls now run off the main stdio-read thread (see handle_request), so
# the server can keep answering `ping`/tools/list while a build/test/bench
# runs for minutes. That means multiple tool calls could otherwise overlap on
# the *device* (e.g. a build racing a test reading the same build/ dir) --
# this lock serializes actual remote command execution back to the previous
# one-at-a-time behavior, without blocking the JSON-RPC read loop itself.
_device_lock = threading.Lock()

class RunResult(dict):
    """Result of a remote/local command, dict-like for easy JSON serialization."""

    __getattr__ = dict.get  # type: ignore[assignment]


def _base_ssh_cmd(cfg: Config) -> list[str]:
    cmd = ["ssh", "-F", "/dev/null",
           "-p", str(cfg.port),
           "-o", "BatchMode=yes",
           "-o", "StrictHostKeyChecking=accept-new",
           "-o", "ConnectTimeout=10"]
    if cfg.ssh_key:
        cmd += ["-i", os.path.expanduser(cfg.ssh_key)]
    cmd.append(cfg.ssh_target())
    return cmd


def _needs_sshpass(cfg: Config) -> bool:
    return bool(cfg.ssh_password)


def _run_remote(cfg: Config, command: str, timeout: int | None = None,
                log_prefix: str = "remote") -> RunResult:
    """
    Run a shell command on the device. Returns {ok, exit_code, stdout, stderr, timed_out}.
    """
    if timeout is None:
        timeout = cfg.timeout()
    start = time.time()

    if _needs_sshpass(cfg):
        # sshpass -p is insecure (visible in argv); -e reads from env.
        env = {**os.environ, "SSHPASS": cfg.ssh_password or ""}
        base = ["sshpass", "-e"] + _base_ssh_cmd(cfg)
        full = base + [command]
    else:
        env = None
        full = _base_ssh_cmd(cfg) + [command]

    try:
        with _device_lock:
            proc = subprocess.run(
                full, capture_output=True, text=True, timeout=timeout, env=env,
            )
        return RunResult(
            ok=(proc.returncode == 0),
            exit_code=proc.returncode,
            stdout=proc.stdout,
            stderr=proc.stderr,
            timed_out=False,
            duration_s=round(time.time() - start, 2),
        )
    except subprocess.TimeoutExpired as e:
        return RunResult(
            ok=False,
            exit_code=None,
            stdout=(e.stdout or "") if isinstance(e.stdout, str) else "",
            stderr=(e.stderr or "") + f"\n[timed out after {timeout}s]",
            timed_out=True,
            duration_s=round(time.time() - start, 2),
        )
    except FileNotFoundError:
        return RunResult(
            ok=False, exit_code=None, stdout="", stderr="ssh not found on PATH",
            timed_out=False, duration_s=0,
        )


def _rsync(cfg: Config, extra_args: list[str]) -> RunResult:
    """Run rsync. Caller builds the argv tail."""
    if _needs_sshpass(cfg):
        env = {**os.environ, "SSHPASS": cfg.ssh_password or ""}
        ssh_shell = "sshpass -e ssh -F /dev/null -p {port} -o StrictHostKeyChecking=accept-new".format(
            port=cfg.port)
        if cfg.ssh_key:
            ssh_shell += f" -i {os.path.expanduser(cfg.ssh_key)}"
    else:
        env = None
        ssh_shell = "ssh -F /dev/null -p {port} -o BatchMode=yes -o StrictHostKeyChecking=accept-new".format(
            port=cfg.port)
        if cfg.ssh_key:
            ssh_shell += f" -i {os.path.expanduser(cfg.ssh_key)}"

    cmd = ["rsync", "-az", "--delete", "-e", ssh_shell] + extra_args
    try:
        with _device_lock:
            proc = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=cfg.timeout(), env=env)
        return RunResult(
            ok=(proc.returncode == 0),
            exit_code=proc.returncode,
            stdout=proc.stdout,
            stderr=proc.stderr,
            timed_out=False,
        )
    except subprocess.TimeoutExpired as e:
        return RunResult(ok=False, exit_code=None, stdout="", stderr=f"rsync timed out",
                         timed_out=True)


# ─────────────────────────────────────────────────────────────────────────────
# Audit log + destructive guard
# ─────────────────────────────────────────────────────────────────────────────

AUDIT_LOG = Path(__file__).parent / "devbox-audit.log"

# A minimal hard floor of catastrophic commands that can brick or lock the box
# and never appear in real dev work. Override with "destructive_guard": false.
DESTRUCTIVE_PATTERNS = [
    r"\b(reboot|shutdown|poweroff|halt)\b",
    r"\bdd\b.*\bof=/dev/(sd|nvme|mmcblk|vd|xvd)",
    r"\bmkfs\b",
    r":\(\)\s*\{",                 # fork bomb
    r"\brm\s+-rf?\s+/(?:\s|$)",    # rm -rf /
    r"\bshutdown\b",
]


def _guard(command: str, cfg: Config) -> str | None:
    """Return a reason string if the command is blocked, else None."""
    if not cfg.destructive_guard:
        return None
    for pat in DESTRUCTIVE_PATTERNS:
        if re.search(pat, command):
            return (f"Blocked by destructive_guard (pattern: {pat!r}). "
                    "This command can brick or lock the device. If you genuinely "
                    "need it, ask the user to run it directly or set "
                    '"destructive_guard": false in devbox.json.')
    return None


def _audit(command: str, result: RunResult | None = None,
           blocked: str | None = None) -> None:
    try:
        ts = time.strftime("%Y-%m-%d %H:%M:%S")
        line = f"[{ts}] cmd={command!r}"
        if blocked:
            line += f" BLOCKED: {blocked}"
        elif result:
            line += f" exit={result.exit_code} dur={result.duration_s}s"
        AUDIT_LOG.parent.mkdir(parents=True, exist_ok=True)
        with AUDIT_LOG.open("a") as f:
            f.write(line + "\n")
    except OSError:
        pass  # audit logging must never break the tool


# ─────────────────────────────────────────────────────────────────────────────
# Output parsers — the agent-facing value
# ─────────────────────────────────────────────────────────────────────────────

def parse_ctest(output: str) -> dict[str, Any]:
    """
    Parse `ctest --output-on-failure` output.
    Returns {passed, failed, total, duration_s, failures:[{name, output_excerpt}]}.
    """
    result: dict[str, Any] = {
        "passed": None, "failed": None, "total": None,
        "duration_s": None, "failures": [],
    }
    # "100% tests passed, 0 tests failed out of 3"
    m = re.search(r"(\d+)% tests passed, (\d+) tests failed out of (\d+)", output)
    if m:
        result["passed"] = int(m.group(1))
        result["failed"] = int(m.group(2))
        result["total"] = int(m.group(3))
    # "Total Test time (real) = 26.54 sec"
    m = re.search(r"Total Test time \(real\)\s*=\s*([\d.]+)\s*sec", output)
    if m:
        result["duration_s"] = float(m.group(1))

    # Per-test failures: CTest prints a header line then the test's own output.
    # Split on the marker that precedes each failed test block.
    blocks = re.split(r"\n(?=\d+/\d+ Test\s+#\d+:.*\.\.\.*\s*\*\*\*Failed)",
                      output)
    for blk in blocks[1:]:
        name_match = re.search(r"Test\s+#\d+:\s*(.*?)\s*\.\.\.", blk)
        name = name_match.group(1).strip() if name_match else "<unknown>"
        # Truncate the captured output to keep payloads reasonable.
        excerpt = blk.strip()
        if len(excerpt) > 1500:
            excerpt = excerpt[:1500] + "\n...[truncated]"
        result["failures"].append({"name": name, "output_excerpt": excerpt})
    return result


def parse_bench(output: str) -> dict[str, Any]:
    """
    Parse bench_armrx output. Lines look like:
      'blake2b (64-in, 64-out)            0.12 μs/op     8333.33 ops/s'
      'full hash (light, JIT)             34.50 μs/hash   28.99 hashes/s'
    Plus: 'Gap to XMRig:             -6.8%'
    Returns {benchmarks:[{name, us_per_op, throughput, unit}], gap_pct}.
    """
    benchmarks: list[dict[str, Any]] = []
    # name (left-justified, 40) then value <unit>/op|hash then throughput <unit>/s.
    # The benchmark() template prints "μs/op"; the two full-hash lines print
    # "μs/hash" — so accept any per-op unit, and any throughput unit ending /s.
    line_re = re.compile(
        r"^(.+?)\s+([\d.]+)\s+(\S+)\s+([\d.]+)\s+(\S+/s)\s*$"
    )
    for line in output.splitlines():
        m = line_re.match(line)
        if m:
            name = m.group(1).strip()
            us = float(m.group(2))
            per_op_unit = m.group(3)      # e.g. "μs/op" or "μs/hash"
            throughput = float(m.group(4))
            per_s_unit = m.group(5)       # e.g. "ops/s" or "hashes/s"
            benchmarks.append({
                "name": name,
                "us_per_op": us,
                "per_op_unit": per_op_unit,
                "throughput": throughput,
                "throughput_unit": per_s_unit,
            })
    gap = None
    m = re.search(r"Gap to XMRig:\s+([-+]?[\d.]+)%", output)
    if m:
        gap = float(m.group(1))
    jit_hps = next((b["throughput"] for b in benchmarks
                    if "JIT" in b["name"]), None)
    interp_hps = next((b["throughput"] for b in benchmarks
                       if "interpreted" in b["name"]), None)
    return {
        "benchmarks": benchmarks,
        "gap_pct": gap,
        "jit_hashes_per_s": jit_hps,
        "interpreted_hashes_per_s": interp_hps,
    }


def parse_build(output: str, exit_code: int | None) -> dict[str, Any]:
    """Parse cmake build output. Returns {ok, errors:[...], warning_count}."""
    errors: list[str] = []
    for line in output.splitlines():
        s = line.strip()
        if re.search(r"\berror:\s", s) or s.startswith("error:"):
            if len(errors) < 30:
                errors.append(s[:300])
    warn_count = len(re.findall(r"\bwarning:\s", output))
    return {
        "ok": (exit_code == 0),
        "errors": errors,
        "warning_count": warn_count,
    }


def parse_perf_stat(output: str) -> dict[str, Any]:
    """Parse output from perf stat."""
    stats = {}
    for line in output.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) >= 2:
            val_str = parts[0].replace(",", "")
            if val_str.isdigit():
                val = int(val_str)
                event_name = parts[1].split(":")[0]  # strip :u, :k, etc.
                event_name = event_name.replace("-", "_")
                stats[event_name] = val
                
    if "instructions" in stats and "cycles" in stats and stats["cycles"] > 0:
        stats["ipc"] = round(stats["instructions"] / stats["cycles"], 3)
    if "branches" in stats and "branch_misses" in stats and stats["branches"] > 0:
        stats["branch_miss_rate_pct"] = round((stats["branch_misses"] / stats["branches"]) * 100, 2)
        
    return stats


# ─────────────────────────────────────────────────────────────────────────────
# On-device log files (durable across connection drops)
# ─────────────────────────────────────────────────────────────────────────────

def _remote_log_path(cfg: Config, kind: str) -> str:
    """Where on-device to stash the last log of a given kind."""
    return f"{cfg.remote_dir}/build/.devbox-{kind}.log"


def _stash_and_run(cfg: Config, command: str, kind: str, timeout: int) -> RunResult:
    """Run `command`, tee'ing combined output to a durable on-device log.

    NOTE: remote_dir/logpath are interpolated unquoted (not shlex.quote()'d) so
    that a `~`-relative remote_dir (the documented default, e.g. "~/armrx")
    tilde-expands under the remote shell. shlex.quote() single-quotes its
    argument, which suppresses tilde expansion entirely -- that previously
    caused `~/armrx/build/...` to resolve to a literal, disconnected `~`
    subdirectory under $HOME instead of the real repo's build/ directory,
    silently losing every build/test/bench log. remote_dir is a trusted
    config-file value, not attacker-controlled input, so this is safe.
    """
    logpath = _remote_log_path(cfg, kind)
    wrapped = (
        f"mkdir -p {cfg.remote_dir}/build && "
        f"{{ {command} ; }} > {logpath} 2>&1 ; "
        f"rc=$?; tail -n 4000 {logpath}; exit $rc"
    )
    res = _run_remote(cfg, wrapped, timeout=timeout)
    res["log_path"] = logpath
    return res


# ─────────────────────────────────────────────────────────────────────────────
# Tool implementations
# ─────────────────────────────────────────────────────────────────────────────

def _local_git_head() -> str:
    try:
        proc = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, timeout=10,
        )
        return proc.stdout.strip() if proc.returncode == 0 else "?"
    except Exception:
        return "?"


def tool_status(cfg: Config) -> dict[str, Any]:
    """Connectivity + revision + device info. No side effects."""
    info_cmd = (
        "echo \"== uname ==\"; uname -a; "
        "echo \"== mem ==\"; grep MemAvailable /proc/meminfo; "
        "echo \"== cpu ==\"; grep -m1 -E '^model name|^Hardware|^Processor' /proc/cpuinfo; "
        "nproc; "
        "echo \"== lscpu ==\"; lscpu 2>/dev/null || echo \"lscpu not available\"; "
        "echo \"== deployed ==\"; cat .devbox-revision 2>/dev/null || echo none; "
        "echo \"== maxfreq ==\"; "
        "for f in /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq /sys/devices/system/cpu/cpufreq/policy*/cpuinfo_max_freq; do "
        "if [ -f \"$f\" ]; then cat \"$f\"; fi; done | sort -u"
    )
    # remote_dir interpolated unquoted so a `~`-relative path (the documented
    # default) tilde-expands under the remote shell -- see _stash_and_run's
    # docstring for why shlex.quote() here would silently cd into a
    # disconnected literal `~` directory instead of the real repo.
    res = _run_remote(cfg, f"cd {cfg.remote_dir} && {info_cmd}",
                      timeout=20)
    return {
        "reachable": res["ok"],
        "ssh_target": cfg.ssh_target(),
        "remote_dir": cfg.remote_dir,
        "local_head": _local_git_head(),
        "device_output": res["stdout"] if res["ok"] else res["stderr"],
        "destructive_guard": cfg.destructive_guard,
    }


def tool_sync(cfg: Config) -> dict[str, Any]:
    """rsync source to device, excluding build/ and .git/. Updates revision marker."""
    head = _local_git_head()
    excludes = ["--exclude=build/", "--exclude=.git/",
                "--exclude=tools/devbox/devbox.json",
                "--exclude=tools/devbox/devbox-audit.log",
                "--exclude=.devbox-revision"]
    src = str(REPO_ROOT) + "/"
    dst = f"{cfg.ssh_target()}:{cfg.remote_dir}/"
    res = _rsync(cfg, excludes + [src, dst])
    if res["ok"]:
        # write the revision marker so status can diff
        _run_remote(cfg, f"echo {shlex.quote(head)} > {cfg.remote_dir}/.devbox-revision",
                    timeout=10)
    return {
        "ok": res["ok"],
        "error": res["stderr"] if not res["ok"] else None,
        "synced_revision": head,
    }



# Any process launched without explicit CPU affinity inherits the *default*
# affinity mask -- which isolcpus restricts to the non-isolated cores (core 0
# only on this device). Confirmed to make on-device builds and parallel ctest
# runs silently serialize onto that one core (all spawned gmake/cc1plus
# children inherit the parent shell's affinity) -- see
# docs/experiments/isolcpus-rt-priority-win.md's "Second footgun" section.
# taskset overrides this explicitly and is a harmless no-op when isolcpus
# isn't set (it just pins to the full online range, which is the default
# anyway), so it's safe to always apply here.
_TASKSET_ALL_CORES = 'taskset -c "$(cat /sys/devices/system/cpu/online 2>/dev/null || echo 0)"'


def tool_build(cfg: Config, reconfigure: bool, clean: bool, extra_flags: list[str] | None = None, parallel: int | None = None) -> dict[str, Any]:
    """On-device cmake build. Incremental by default."""
    build_flags = list(cfg.build_flags)
    if extra_flags:
        build_flags.extend(extra_flags)
    flags = " ".join(shlex.quote(f) for f in build_flags)

    steps = []
    if clean:
        steps.append(f"rm -rf {cfg.remote_dir}/build")
    if reconfigure or clean or not _build_dir_exists(cfg):
        steps.append(
            f"cmake -S {cfg.remote_dir} -B {cfg.remote_dir}/build {flags}"
        )

    jobs = parallel if parallel is not None else getattr(cfg, "build_jobs", None)
    jobs_flag = f"-j{jobs}" if jobs is not None else "-j"
    steps.append(f"{_TASKSET_ALL_CORES} cmake --build {cfg.remote_dir}/build {jobs_flag}")

    cmd = " && ".join(steps)
    res = _stash_and_run(cfg, cmd, "build", cfg.timeout("build"))
    parsed = parse_build(res["stdout"], res["exit_code"])
    parsed["timed_out"] = res["timed_out"]
    parsed["duration_s"] = res["duration_s"]
    parsed["log_path"] = res.get("log_path")
    if not res["ok"] and not parsed["errors"]:
        # surface a short tail when we couldn't pattern-match errors
        tail = res["stdout"][-800:] if res["stdout"] else res["stderr"][-800:]
        parsed["output_tail"] = tail
    return parsed


def _build_dir_exists(cfg: Config) -> bool:
    res = _run_remote(cfg, f"test -d {cfg.remote_dir}/build", timeout=10)
    return res["ok"]


def tool_test(cfg: Config, tests: list[str] | None, parallel: int) -> dict[str, Any]:
    """Run ctest on device. Optional regex filter of test names."""
    # --test-dir's value is built from cfg.remote_dir and interpolated
    # unquoted (not through the shlex.quote() below) so a `~`-relative
    # remote_dir still tilde-expands -- see _stash_and_run's docstring.
    args = ["ctest", f"--test-dir={cfg.remote_dir}/build",
            "--output-on-failure", f"-j{parallel}"]
    if tests:
        args += ["-R", "^(" + "|".join(re.escape(t) for t in tests) + ")$"]
    cmd = _TASKSET_ALL_CORES + " " + " ".join(
        a if a.startswith("--test-dir=") else shlex.quote(a) for a in args)
    # NOTE: was cfg.timeout() (the 120s "default" bucket) -- the full,
    # unfiltered suite (bench_armrx alone ~300s, test_pool_protocol ~65s,
    # plus everything else) routinely takes 350-400s+, so this always timed
    # out. Give it its own bucket instead of borrowing "default"/"bench".
    res = _stash_and_run(cfg, cmd, "test", cfg.timeout("test"))
    parsed = parse_ctest(res["stdout"])
    parsed["ok"] = res["ok"]
    parsed["timed_out"] = res["timed_out"]
    parsed["log_path"] = res.get("log_path")
    if not res["ok"] and parsed["failed"] is None:
        parsed["output_tail"] = res["stdout"][-800:] or res["stderr"][-800:]
    return parsed


def tool_bench(cfg: Config) -> dict[str, Any]:
    """Run bench_armrx on device, parse results."""
    cmd = f"{cfg.remote_dir}/build/bench_armrx"
    res = _stash_and_run(cfg, cmd, "bench", cfg.timeout("bench"))
    if not res["ok"]:
        return {
            "ok": False,
            "timed_out": res["timed_out"],
            "error": res["stderr"][-800:] or res["stdout"][-800:],
            "log_path": res.get("log_path"),
        }
    parsed = parse_bench(res["stdout"])
    parsed["ok"] = True
    parsed["duration_s"] = res["duration_s"]
    parsed["log_path"] = res.get("log_path")
    return parsed


def tool_perf_stat(cfg: Config, extra_args: list[str] | None = None) -> dict[str, Any]:
    """Run bench_armrx under perf stat on device, parse results."""
    bench_bin = f"{cfg.remote_dir}/build/bench_armrx"
    args = []
    if extra_args:
        args.extend(extra_args)
    args_str = " ".join(shlex.quote(a) for a in args)
    
    perf_cmd = f"perf stat -e instructions,cycles,branches,branch-misses {bench_bin} {args_str}"
    res = _stash_and_run(cfg, perf_cmd, "bench", cfg.timeout("bench"))
    
    output = (res["stdout"] or "") + "\n" + (res["stderr"] or "")
    parsed = parse_perf_stat(output)
    parsed["ok"] = res["ok"]
    parsed["timed_out"] = res["timed_out"]
    parsed["duration_s"] = res["duration_s"]
    parsed["log_path"] = res.get("log_path")
    if not res["ok"] and not parsed.get("instructions"):
        parsed["error"] = res["stderr"][-800:] or res["stdout"][-800:]
    return parsed


def tool_full(cfg: Config, mode: str, skip_test: bool, skip_bench: bool) -> dict[str, Any]:
    """Orchestrate sync -> (build) -> test -> bench. mode: 'full' or 'run-only'."""
    out: dict[str, Any] = {"mode": mode, "steps": {}}
    sync = tool_sync(cfg)
    out["steps"]["sync"] = sync
    if not sync["ok"]:
        out["ok"] = False
        out["error"] = "sync failed"
        return out
    if mode == "full":
        build = tool_build(cfg, reconfigure=False, clean=False)
        out["steps"]["build"] = build
        if not build["ok"]:
            out["ok"] = False
            out["error"] = "build failed"
            return out
    if not skip_test:
        out["steps"]["test"] = tool_test(cfg, tests=None, parallel=2)
    if not skip_bench:
        out["steps"]["bench"] = tool_bench(cfg)
    out["ok"] = True
    return out


def tool_pgo_build(cfg: Config, train_seconds: int, parallel: int | None) -> dict[str, Any]:
    """
    Two-stage PGO release build: GENERATE -> train -> USE, in cfg.remote_dir/build.

    ARMRX_PGO=GENERATE and ARMRX_PGO=USE must share the SAME build directory:
    GCC's .gcda profile-count files land inside the build tree next to each
    target's .o files, and the USE compile reads them from there (CMakeLists.txt's
    own comment: "using profile data from build directory"). So stage 1 uses
    clean=True (a fresh build, discarding any stale profile data from a prior
    run so counts aren't merged with old code), but stage 3 must NOT clean --
    doing so would delete the .gcda files stage 2 just produced, before stage 3
    ever reads them.

    Training workload: docs/performance-next-agent-handoff.md SS10.3 explicitly
    says to train on sustained light-mode JIT mining, not cache init/interpreted
    mode/CLI startup -- `armrx --mine --seconds=N` matches that guidance (the
    default binary is light mode; this is also the workload the +19.3%
    single-thread figure in NEXT_STEPS.md's telemetry table was validated against).

    LTO is auto-disabled by CMakeLists.txt whenever ARMRX_PGO is GENERATE or USE
    (see the ipo_supported guard there) -- no separate flag needed here.
    """
    out: dict[str, Any] = {"steps": {}, "train_seconds": train_seconds}

    generate = tool_build(cfg, reconfigure=True, clean=True,
                          extra_flags=["-DARMRX_PGO=GENERATE"], parallel=parallel)
    out["steps"]["generate_build"] = generate
    if not generate.get("ok"):
        out["ok"] = False
        out["error"] = "PGO GENERATE build failed"
        return out

    train_cmd = f"{cfg.remote_dir}/build/armrx --mine --seconds={int(train_seconds)}"
    train = _stash_and_run(cfg, train_cmd, "pgo_train", cfg.timeout("bench"))
    out["steps"]["train"] = {
        "ok": train["ok"],
        "timed_out": train["timed_out"],
        "duration_s": train["duration_s"],
        "log_path": train.get("log_path"),
    }
    if not train["ok"]:
        out["ok"] = False
        out["error"] = "PGO training run failed"
        return out

    use = tool_build(cfg, reconfigure=True, clean=False,
                     extra_flags=["-DARMRX_PGO=USE"], parallel=parallel)
    out["steps"]["use_build"] = use
    if not use.get("ok"):
        out["ok"] = False
        out["error"] = "PGO USE build failed"
        return out

    out["ok"] = True
    out["note"] = ("build/ now holds a PGO-optimized release binary (LTO disabled). "
                   "Run devbox_test / devbox_bench to validate before relying on it; "
                   "a subsequent plain devbox_build with default flags will reconfigure "
                   "away from PGO back to the normal dev build.")
    return out


def tool_shell(cfg: Config, command: str, timeout: int | None) -> dict[str, Any]:
    """Arbitrary shell on device. Audit-logged + destructive guard."""
    blocked = _guard(command, cfg)
    if blocked:
        _audit(command, blocked=blocked)
        return {"ok": False, "blocked": True, "reason": blocked}
    _audit(command)
    res = _run_remote(cfg, command, timeout=timeout or cfg.timeout())
    return {
        "ok": res["ok"],
        "exit_code": res["exit_code"],
        "stdout": res["stdout"],
        "stderr": res["stderr"],
        "timed_out": res["timed_out"],
        "duration_s": res["duration_s"],
    }


def tool_logs(cfg: Config, kind: str, lines: int) -> dict[str, Any]:
    """Read the durable on-device log for a prior build/test/bench run."""
    logpath = _remote_log_path(cfg, kind)
    # Unquoted so a `~`-relative remote_dir tilde-expands, matching how
    # _stash_and_run writes this same path -- see its docstring.
    res = _run_remote(cfg, f"tail -n {int(lines)} {logpath}",
                      timeout=15)
    return {
        "ok": res["ok"],
        "kind": kind,
        "log_path": logpath,
        "content": res["stdout"] if res["ok"] else res["stderr"],
    }


def tool_get(cfg: Config, path: str, max_bytes: int) -> dict[str, Any]:
    """Fetch a file's contents from device (read-oriented)."""
    if max_bytes <= 0 or max_bytes > 256 * 1024:
        max_bytes = 32 * 1024
    # head -c to cap size, avoid pulling giant files/core dumps in full.
    res = _run_remote(cfg, f"head -c {int(max_bytes)} {shlex.quote(path)}",
                      timeout=15)
    return {
        "ok": res["ok"],
        "path": path,
        "content": res["stdout"] if res["ok"] else res["stderr"],
        "truncated_to_bytes": max_bytes,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Tool schemas (for tools/list)
# ─────────────────────────────────────────────────────────────────────────────

TOOL_DEFS: list[dict[str, Any]] = [
    {
        "name": "devbox_status",
        "description": (
            "Check connectivity to the AArch64 device and report device info "
            "(uname, RAM, CPU model, core count, max frequencies), the deployed "
            "revision vs the local git HEAD, and whether the destructive guard is "
            "on. No side effects — the right first call every session."
        ),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "devbox_sync",
        "description": (
            "rsync the local source tree to the device (excludes build/ and .git/), "
            "then write a .devbox-revision marker. Required before devbox_build."
        ),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "devbox_build",
        "description": (
            "On-device cmake build (native AArch64 compile — slow). Incremental by "
            "default. Returns parsed {ok, errors[], warning_count}. Logs are stashed "
            "on-device for devbox_logs."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "reconfigure": {"type": "boolean", "default": False,
                                "description": "re-run cmake configure step"},
                "clean": {"type": "boolean", "default": False,
                          "description": "rm -rf build/ first (full rebuild)"},
                "extra_flags": {"type": "array", "items": {"type": "string"},
                                "description": "extra cmake build flags to append"},
                "parallel": {"type": "integer", "description": "number of parallel build jobs"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "devbox_test",
        "description": (
            "Run ctest --output-on-failure on device. Returns parsed "
            "{passed, failed, total, failures[]} for the KAT / unit suite."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "tests": {"type": "array", "items": {"type": "string"},
                          "description": "optional regex name filter"},
                "parallel": {"type": "integer", "default": 2},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "devbox_bench",
        "description": (
            "Run bench_armrx on device and parse results — per-benchmark μs/op and "
            "throughput, JIT vs interpreted hashes/s, and the XMRig gap percent."
        ),
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "devbox_full",
        "description": (
            "Orchestrate the full validation flow in one call: sync -> build (mode "
            "'full') or skip build (mode 'run-only') -> test -> bench. Returns "
            "aggregated JSON. One round-trip for complete real-hardware validation."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "mode": {"type": "string", "enum": ["full", "run-only"], "default": "full"},
                "skip_test": {"type": "boolean", "default": False},
                "skip_bench": {"type": "boolean", "default": False},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "devbox_pgo_build",
        "description": (
            "Two-stage Profile-Guided Optimization release build: GENERATE (clean "
            "rebuild, instrumented) -> train (runs `armrx --mine --seconds=N`, "
            "sustained light-mode JIT mining per docs/performance-next-agent-"
            "handoff.md SS10.3 -- not CLI startup or cache init) -> USE (reconfigure "
            "+ rebuild consuming the just-collected profile data, same build "
            "directory throughout so gcov finds it). LTO is auto-disabled for both "
            "PGO stages by CMakeLists.txt. This is the ~+19.3% single-thread / "
            "+14.9% at 8 threads lever (NEXT_STEPS.md SS5a) that the default "
            "devbox_build flags don't produce. Slow -- two full native rebuilds "
            "plus a real mining run. Run devbox_test/devbox_bench afterward to "
            "validate before relying on the result; a later plain devbox_build "
            "reconfigures back to the normal (non-PGO) dev build."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "train_seconds": {"type": "integer", "default": 30,
                                  "description": "how long to run the training mining workload"},
                "parallel": {"type": "integer", "description": "number of parallel build jobs"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "devbox_shell",
        "description": (
            "Run an arbitrary shell command on the device. Full shell access. Every "
            "call is appended to the audit log. A default-on destructive guard blocks "
            "a tiny catastrophic set (reboot/dd-to-device/mkfs/rm -rf /) — override "
            "with destructive_guard:false in devbox.json if truly needed."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {"type": "string"},
                "timeout": {"type": "integer",
                            "description": "override timeout in seconds"},
            },
            "required": ["command"],
            "additionalProperties": False,
        },
    },
    {
        "name": "devbox_logs",
        "description": (
            "Read the durable on-device log of a prior build/test/bench run "
            "(survives connection drops). kind is 'build', 'test', or 'bench'."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "kind": {"type": "string", "enum": ["build", "test", "bench"]},
                "lines": {"type": "integer", "default": 200},
            },
            "required": ["kind"],
            "additionalProperties": False,
        },
    },
    {
        "name": "devbox_get",
        "description": (
            "Fetch a file's contents from the device (read-oriented), capped to "
            "max_bytes (default 32 KiB). Use for /proc/cpuinfo, the head of a build "
            "log, a config, etc."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "absolute path on device"},
                "max_bytes": {"type": "integer", "default": 32768},
            },
            "required": ["path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "devbox_perf_stat",
        "description": (
            "Run bench_armrx under perf stat on device. Returns parsed performance metrics "
            "like instructions, cycles, branches, branch-misses, IPC, and branch-miss rate."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "extra_args": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "extra command line arguments for bench_armrx"
                }
            },
            "additionalProperties": False,
        },
    },
]


DISPATCH = {
    "devbox_status": lambda cfg, a: tool_status(cfg),
    "devbox_sync": lambda cfg, a: tool_sync(cfg),
    "devbox_build": lambda cfg, a: tool_build(cfg, a.get("reconfigure", False),
                                              a.get("clean", False),
                                              a.get("extra_flags"),
                                              a.get("parallel")),
    "devbox_test": lambda cfg, a: tool_test(cfg, a.get("tests"), a.get("parallel", 2)),
    "devbox_bench": lambda cfg, a: tool_bench(cfg),
    "devbox_perf_stat": lambda cfg, a: tool_perf_stat(cfg, a.get("extra_args")),
    "devbox_full": lambda cfg, a: tool_full(cfg, a.get("mode", "full"),
                                             a.get("skip_test", False),
                                             a.get("skip_bench", False)),
    "devbox_pgo_build": lambda cfg, a: tool_pgo_build(cfg, a.get("train_seconds", 30),
                                                       a.get("parallel")),
    "devbox_shell": lambda cfg, a: tool_shell(cfg, a["command"], a.get("timeout")),
    "devbox_logs": lambda cfg, a: tool_logs(cfg, a["kind"], a.get("lines", 200)),
    "devbox_get": lambda cfg, a: tool_get(cfg, a["path"], a.get("max_bytes", 32768)),
}


# ─────────────────────────────────────────────────────────────────────────────
# MCP JSON-RPC over stdio
# ─────────────────────────────────────────────────────────────────────────────

_stdout_lock = threading.Lock()


def _send(msg: dict[str, Any]) -> None:
    # tools/call responses can now arrive from worker threads (see
    # handle_request) concurrently with the main thread's own replies
    # (ping, tools/list, ...) -- serialize so two JSON lines never interleave
    # into one garbled line on stdout.
    line = json.dumps(msg) + "\n"
    with _stdout_lock:
        sys.stdout.write(line)
        sys.stdout.flush()


def _result(req_id: Any, result: Any) -> None:
    _send({"jsonrpc": "2.0", "id": req_id, "result": result})


def _error(req_id: Any, code: int, message: str, data: Any = None) -> None:
    err: dict[str, Any] = {"code": code, "message": message}
    if data is not None:
        err["data"] = data
    _send({"jsonrpc": "2.0", "id": req_id, "error": err})


# JSON-RPC error codes
ERR_PARSE = -32700
ERR_METHOD = -32601
ERR_PARAMS = -32602
ERR_INTERNAL = -32603


def handle_request(req: dict[str, Any], cfg: Config | None) -> None:
    req_id = req.get("id")
    method = req.get("method")
    params = req.get("params") or {}

    if method == "initialize":
        _result(req_id, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
        })
        return
    if method == "notifications/initialized":
        return  # notification — no response
    if method == "tools/list":
        _result(req_id, {"tools": TOOL_DEFS})
        return
    if method == "tools/call":
        if cfg is None:
            _error(req_id, ERR_INTERNAL, "server not configured: " + str(_CFG_ERR))
            return
        name = params.get("name")
        args = params.get("arguments") or {}
        handler = DISPATCH.get(name)
        if handler is None:
            _error(req_id, ERR_METHOD, f"unknown tool: {name}")
            return

        def _run(handler=handler, name=name, args=args, req_id=req_id) -> None:
            try:
                result = handler(cfg, args)
                text = json.dumps(result, indent=2, default=str)
                _send({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {
                        "content": [{"type": "text", "text": text}],
                        "isError": bool(result.get("ok") is False),
                    },
                })
            except KeyError as e:
                _error(req_id, ERR_PARAMS, f"missing required argument: {e}")
            except Exception as e:
                log(f"tool {name} raised: {type(e).__name__}: {e}")
                _error(req_id, ERR_INTERNAL, f"{type(e).__name__}: {e}")

        # Run off the stdin-read thread. Tool handlers shell out to ssh/rsync
        # and can legitimately take minutes (test/bench/build) -- previously
        # this ran inline, so the server couldn't read or answer *anything*
        # else on stdin (including `ping`) for the whole duration, and the
        # MCP host would conclude the server had hung and reconnect mid-call.
        # `_device_lock` (see _run_remote/_rsync) still serializes the actual
        # remote commands, so this doesn't let two builds/tests race on the
        # device -- it only frees the JSON-RPC loop to keep responding.
        threading.Thread(target=_run, name=f"tool-{name}-{req_id}", daemon=True).start()
        return
    if method == "ping":
        _result(req_id, {})
        return
    # Unknown method — error for requests, silent for notifications.
    if req_id is None:
        return
    _error(req_id, ERR_METHOD, f"unknown method: {method}")


_CFG: Config | None = None
_CFG_ERR: Exception | None = None


def main() -> int:
    global _CFG, _CFG_ERR
    try:
        _CFG = Config()
        log(f"configured for {_CFG.ssh_target()}:{_CFG.port} -> {_CFG.remote_dir}")
    except Exception as e:
        _CFG_ERR = e
        log(f"WARNING: config not loaded: {e}")
        log("tools/call will fail until devbox.json is set up; tools/list & initialize still work.")

    # Read newline-delimited JSON-RPC from stdin.
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            req = json.loads(raw)
        except json.JSONDecodeError:
            _send({"jsonrpc": "2.0", "id": None,
                   "error": {"code": ERR_PARSE, "message": "Parse error"}})
            continue
        try:
            handle_request(req, _CFG)
        except Exception as e:
            log(f"handler crash: {type(e).__name__}: {e}")
            try:
                _error(req.get("id"), ERR_INTERNAL, f"handler crash: {e}")
            except Exception:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
