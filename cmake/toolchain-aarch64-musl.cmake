# Cross-compile toolchain for the armrx device target.
#
# Device: MSM8929 (Cortex-A53), postmarketOS edge — Alpine musl 1.2.x, GCC 15.2.0.
# Host: x86_64 Linux (CachyOS). Toolchain: AUR aarch64-linux-musl-cross
# (GCC 16.1.0 + binutils 2.44 + musl 1.2.5, musl-cross-make; static target libs).
# Install: paru -S aarch64-linux-musl-cross  →  drivers in /usr/bin/, sysroot in
# /usr/aarch64-linux-musl/.
#
# Usage:
#   cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake
#   cmake --build build-cross -j
# Do NOT pass -DARMRX_ENABLE_NATIVE=ON (maps to -mcpu=native; invalid on cross
# GCC — forced OFF below; -mtune=cortex-a53 covers the intent).
#
# Notes:
# - CMAKE_SYSTEM_PROCESSOR=aarch64 makes CMakeLists.txt take the JIT branch
#   (ARMRX_HAVE_JIT=1, -march=armv8-a+crypto, .S assembled by cross gas).
# - Musl target matches the device libc → binaries run directly after scp.
# - OpenSSL for aarch64 is NOT present → TLS pool connections disabled
#   (find_package(OpenSSL QUIET) fails silently; same as device-native without openssl-dev).
# - LTO forced OFF (ARMRX_DISABLE_LTO): the cross GCC 16.1.0 (AUR
#   aarch64-linux-musl-cross) ships NO liblto_plugin.so for the aarch64 target,
#   so real LTO (cross-module inlining) is impossible on this toolchain. CMake's
#   check_ipo_supported() probe uses -fno-fat-lto-objects (requires the plugin)
#   and fails -> LTO silently disabled. Fat-LTO objects compile/link but the
#   linker just uses the embedded non-LTO code (no optimization). See 2026-08-07
#   Lever-4 attempt: cross-LTO blocked by missing plugin; not adopted.
# - GCC 16.1.0 (vs device GCC 15.2.0) fixed the GCC 11.2.1 seed-dependent JIT
#   codegen hang: test_jit_equivalence + test_jit_scheduler_stress now pass on
#   device with cross binaries (verified 2026-08-01).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_ROOT /usr/aarch64-linux-musl)
set(CMAKE_C_COMPILER   /usr/bin/aarch64-linux-musl-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-musl-g++)
set(CMAKE_ASM_COMPILER /usr/bin/aarch64-linux-musl-gcc)

# AUR cross-ar (binutils 2.44) HANGS spinning at 100% CPU while archiving the
# project's objects (State R, wchan 0, archive stuck at 8 bytes — verified
# 2026-07-31 on libarmrx_core.a). GNU archive format is arch-independent, so
# host binutils ar/ranlib/nm handle aarch64 objects fine.
# CACHE...FORCE is REQUIRED: plain set() in a toolchain file is clobbered by
# CMake's compiler-detection stage (which finds <prefix>-ar from the compiler).
# Detection ALSO sets a directory-scoped CMAKE_AR that shadows the cache during
# rule generation (cache says /usr/bin/ar, link.txt still musl-ar) — so the
# per-language ARCHIVE_* rules below hard-pin the host tools by absolute path;
# they are generated verbatim into link.txt and cannot be shadowed.
set(CMAKE_AR     /usr/bin/ar     CACHE FILEPATH "host ar (AUR cross-ar hangs)" FORCE)
set(CMAKE_RANLIB /usr/bin/ranlib CACHE FILEPATH "host ranlib (AUR cross-ranlib hangs)" FORCE)
set(CMAKE_NM     /usr/bin/nm     CACHE FILEPATH "host nm (AUR cross-nm hangs)" FORCE)

set(CMAKE_C_ARCHIVE_CREATE   "/usr/bin/ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_APPEND   "/usr/bin/ar q  <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH   "/usr/bin/ranlib <TARGET>")
set(CMAKE_CXX_ARCHIVE_CREATE "/usr/bin/ar qc <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_APPEND "/usr/bin/ar q  <TARGET> <LINK_FLAGS> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_FINISH "/usr/bin/ranlib <TARGET>")

set(ARMRX_ENABLE_NATIVE OFF CACHE BOOL "Tune for the compiler host CPU" FORCE)
set(ARMRX_DISABLE_LTO ON CACHE BOOL "Disable LTO/IPO even if supported (cross GCC 16 has no aarch64 liblto_plugin -> real LTO impossible)" FORCE)

# CRITICAL: disable host pkg-config. CMakeLists.txt runs
# pkg_check_modules(HWLOC hwloc) which finds the HOST's hwloc.pc and adds
# -I/usr/include (host glibc headers) to every cross compile — 500+ errors.
# The musl sysroot ships no .pc files, so hwloc correctly falls back to
# sysfs-based CPU pinning (no ARMRX_HAVE_HWLOC).
set(PKG_CONFIG_EXECUTABLE FALSE CACHE FILEPATH "pkg-config disabled for cross builds")

set(CMAKE_C_FLAGS_INIT   "-mtune=cortex-a53")
set(CMAKE_CXX_FLAGS_INIT "-mtune=cortex-a53")

set(CMAKE_FIND_ROOT_PATH ${CROSS_ROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
