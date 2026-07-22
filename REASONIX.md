# armrx — Reasonix project card

## Stack

- **Language** — C++20, AArch64 Linux primary target
- **Build** — CMake ≥3.20 (`cmake -S . -B build && cmake --build build -j`)
- **Test** — CTest via `ctest --test-dir build --output-on-failure`
- **Key deps** — pthreads (via `find_package(Threads)`); hardware AES/NEON on AArch64 (`-march=armv8-a+crypto`)
- **Lint/format** — none configured (no `.clang-format`, `.clang-tidy`, or lint scripts)

## Layout

| Path | Content |
|------|---------|
| `include/armrx/` | Public headers (`.hpp`) |
| `src/` | Implementation files (`.cpp`, `.c`, `.S` for AArch64 JIT) |
| `tests/` | Test executables: `test_blake2b.cpp`, `test_mining.cpp` |
| `scratch_vm_study/` | Standalone RandomX reference study area (own CMakeLists.txt, NOT part of main build) |
| `build/` | Build output (gitignored) |

## Commands

| Action | Command |
|--------|---------|
| Configure | `cmake -S . -B build -DARMRX_ENABLE_NATIVE=ON` |
| Build | `cmake --build build -j` |
| Test | `ctest --test-dir build --output-on-failure` |
| Cross-build (x86_64) | Same configure/build; JIT files excluded automatically |

## Conventions

- **Include style** — project headers with `""`, stdlib headers with `<>`, grouped separately
- **Header guard** — `#pragma once` everywhere
- **Namespace** — everything under `armrx`
- **Naming** — `PascalCase` classes, `snake_case` functions/members, `SCREAMING_SNAKE_CASE` constants
- **File extensions** — `.hpp` for C++ headers, `.h` for C headers, `.cpp` for C++ sources, `.c` for C sources, `.S` for AArch64 assembly
- **Test files** — `test_` prefix in filename, linked against `armrx_core` library

## Watch out for

- **`scratch_vm_study/` is standalone** — it has its own `CMakeLists.txt` and an embedded `upstream_rx/` subtree (third-party RandomX reference). Edits there don't affect the main build.
- **JIT is AArch64-only** — `jit_compiler_a64.cpp`, `jit_compiler_a64_static.S`, and `virtual_memory.c` are only compiled when targeting AArch64. On x86_64 they're silently excluded; the VM falls back to interpreted mode.
- **`build/` is gitignored** — all build output lives there. `cmake -S . -B build` is required before any build commands.
- **`-frounding-math` enabled** — affects floating-point behavior; relevant for the RandomX VM's FPU opcodes.
