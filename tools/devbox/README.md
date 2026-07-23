# armrx devbox — MCP bridge to the AArch64 device

A single-file, **zero-dependency** MCP (Model Context Protocol) server that lets
ZCode (or any MCP-aware agent) drive the real AArch64 development device over
SSH: sync code, compile natively, run the KAT/bench suite, and read back
**structured JSON results**.

Why it exists: the agent's local environment is sandboxed x86_64, but the perf
numbers and the JIT only run on the A53. This tool closes that gap so a change
can be validated against real hardware in one call.

## Requirements

- **Local** (where ZCode runs): Python 3 (any 3.8+; uses stdlib only), `ssh`,
  `rsync`, `scp` on PATH. No pip packages.
- **Device**: an SSH server, `rsync`, a C++20 toolchain + CMake ≥ 3.20. The
  miner builds there natively (no cross-compile needed).

## Setup (one time)

### 1. Configure the device connection

```sh
cp tools/devbox/devbox.example.json tools/devbox/devbox.json
$EDITOR tools/devbox/devbox.json   # set host + user
```

`devbox.json` is gitignored — it never leaves your machine.

### 2. Confirm non-interactive SSH works

The MCP server runs as a background process and **cannot prompt** for a
passphrase or host-key confirmation:

```sh
ssh user@host true      # should return immediately, no prompt
```

- If your key has a passphrase, add it to the agent first: `ssh-add ~/.ssh/id_ed25519`.
- If host-key checking prompts, the server uses `StrictHostKeyChecking=accept-new`
  so a first connection auto-accepts; thereafter it's pinned.
- Password auth is supported (`ssh_password` in config) via `sshpass -e`, but key
  auth is strongly preferred. The password is read from the process environment,
  never passed on the command line.

### 3. Register the server with ZCode

Add this block under `mcp.servers` in **`~/.zcode/cli/config.json`** (use
absolute paths — config files do not expand `~` or `$VAR`):

```jsonc
"armrx-devbox": {
  "type": "stdio",
  "command": "/usr/bin/python3",
  "args": ["/home/mechres/Projeler/aarch64-randomx/tools/devbox/devbox_mcp.py"],
  "cwd": "/home/mechres/Projeler/aarch64-randomx",
  "env": {
    "DEVBOX_CONFIG": "/home/mechres/Projeler/aarch64-randomx/tools/devbox/devbox.json"
  },
  "timeoutMs": 1850000,
  "enabled": true
}
```

Restart ZCode. In a new session the tools appear as `mcp__armrx-devbox__*` and
auto-connect. Verify under **Settings → MCP**.

> The high `timeoutMs` (≈31 min) accommodates a cold native A53 build + bench in
> a single `devbox_full` call. Individual tool calls still get their own
> server-side timeout from `timeout_s` in your config.

## Tools

| Tool | What it does | Notable return fields |
|------|---|---|
| `devbox_status` | Connectivity + device info (uname, RAM, CPU, cores, max freqs), deployed revision vs local HEAD. No side effects. The right first call every session. | `reachable`, `local_head`, `device_output` |
| `devbox_sync` | `rsync -az --delete` the source tree (excludes `build/`, `.git/`). Writes a `.devbox-revision` marker. | `ok`, `synced_revision` |
| `devbox_build` | On-device `cmake` build (native A53 — slow). Incremental by default; `reconfigure`/`clean` opts. | `ok`, `errors[]`, `warning_count` |
| `devbox_test` | `ctest --output-on-failure`, optionally filtered by name regex. | `passed`, `failed`, `total`, `failures[]` |
| `devbox_bench` | Runs `bench_armrx`, parses per-benchmark μs/op + throughput. | `benchmarks[]`, `jit_hashes_per_s`, `gap_pct` |
| `devbox_full` | One-shot **sync → build → test → bench** (`mode: full`) or **sync → test → bench** (`mode: run-only`). | `ok`, `steps.{sync,build,test,bench}` |
| `devbox_shell` | Arbitrary shell on device. Audit-logged; destructive guard on by default. | `ok`, `stdout`, `stderr`, `exit_code` |
| `devbox_logs` | Read the durable on-device log of a prior `build`/`test`/`bench` (survives drops). | `content` |
| `devbox_get` | Fetch a file's contents from the device (read-only, size-capped). | `content` |

### Typical session flow

1. `devbox_status` — confirm the bridge is up and see the device.
2. Make a code change locally.
3. `devbox_full` with `mode: full` — sync + native build + tests + bench, all parsed.
4. If something regresses, `devbox_logs` (kind: `build`/`test`/`bench`) for the full output, or `devbox_get` a `/proc/...` file for diagnostics.

## Safety

- **Audit log:** every `devbox_shell` call is appended to
  `tools/devbox/devbox-audit.log` (gitignored): timestamp, command, exit code.
- **Destructive guard** (`destructive_guard: true`, default): blocks a tiny set
  of catastrophic commands that can brick or lock the box and never appear in
  real dev work — `reboot`/`shutdown`/`poweroff`, `dd … of=/dev/(sd|nvme|mmcblk…)`,
  `mkfs`, fork bombs, `rm -rf /`. Everything else is unrestricted shell. Set
  `destructive_guard: false` only if you accept the risk.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `tools/call` returns "server not configured" | `DEVBOX_CONFIG` unset or file missing — check the `env` block in the MCP config points at your `devbox.json`. |
| `devbox_status` says `reachable: false` | Run `ssh user@host true` by hand; fix keys/passphrase/`known_hosts` first. The server uses `BatchMode=yes`. |
| Build times out | Raise `timeout_s.build` in `devbox.json` and `timeoutMs` in the MCP config. Native A53 compile of a cold tree is slow. |
| `devbox_bench` returns no `jit_hashes_per_s` | The bench binary didn't build or didn't print the `full hash (light, JIT)` line — check `devbox_logs` kind `bench`. |
| Tools don't appear after edit | User config (`~/.zcode/cli/config.json`) overrides workspace; restart ZCode; confirm under Settings → MCP. See the `diagnosing-mcp` skill for the full checklist. |

## Running the server by hand (debugging)

The server speaks newline-delimited JSON-RPC on stdio. You can drive it
directly to test without ZCode:

```sh
# handshake + list tools
printf '%s\n' \
  '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
| DEVBOX_CONFIG=tools/devbox/devbox.json /usr/bin/python3 tools/devbox/devbox_mcp.py
```

Diagnostics go to **stderr** (stdout is the protocol channel — a stray `print`
there would corrupt the protocol).

## Design notes

- **Zero-dependency by necessity.** The ZCode AppImage intercepts `python3`/`pip`,
  so the `mcp` SDK can't be installed in this environment. Implementing
  JSON-RPC-over-stdio by hand (~150 LOC) removes that dependency and makes the
  server portable to any host with a system Python 3.
- **No credentials are stored by this tool.** It reuses your existing
  `~/.ssh/id_*` key, `~/.ssh/known_hosts`, and ssh config. `ssh_password`, if
  set, lives only in the gitignored `devbox.json` and is passed via env.
- **Synchronous model.** MCP calls are sequential, so v1 is synchronous with
  per-command server-side timeouts and durable on-device logs for long builds.
  Async job IDs can layer on later without rework.
