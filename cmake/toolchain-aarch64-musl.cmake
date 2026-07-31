# Cross-compile toolchain for the armrx device target.
#
# Device: MSM8929 (Cortex-A53), postmarketOS edge — Alpine musl 1.2.x, GCC 15.2.0.
# Host: x86_64 Linux (CachyOS). Toolchain: musl.cc aarch64-linux-musl-cross
# (GCC 11.2.1 + musl; musl.cc stable snapshot) under $HOME/toolchains/aarch64-linux-musl-cross.
#
# Usage:
#   cmake -S . -B build-cross -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-musl.cmake
#   cmake --build build-cross -j
# Do NOT pass -DARMRX_ENABLE_NATIVE=ON (maps to -mcpu=native; invalid on this
# cross GCC — forced OFF below; -mtune=cortex-a53 covers the intent).
#
# Notes:
# - CMAKE_SYSTEM_PROCESSOR=aarch64 makes CMakeLists.txt take the JIT branch
#   (ARMRX_HAVE_JIT=1, -march=armv8-a+crypto, .S assembled by cross gas).
# - Musl target matches the device libc → binaries run directly after scp.
# - OpenSSL for aarch64 is NOT present → TLS pool connections disabled
#   (find_package(OpenSSL QUIET) fails silently; same as device-native without openssl-dev).
# - GCC 11 LTO probe fails (no linker plugin) → ipo_supported=FALSE → LTO auto-skips.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_ROOT $ENV{HOME}/toolchains/aarch64-linux-musl-cross)
set(CMAKE_C_COMPILER   ${CROSS_ROOT}/bin/aarch64-linux-musl-gcc)
set(CMAKE_CXX_COMPILER ${CROSS_ROOT}/bin/aarch64-linux-musl-g++)
set(CMAKE_ASM_COMPILER ${CROSS_ROOT}/bin/aarch64-linux-musl-gcc)

set(ARMRX_ENABLE_NATIVE OFF CACHE BOOL "Tune for the compiler host CPU" FORCE)

# CRITICAL: disable host pkg-config. CMakeLists.txt:88-91 runs
# pkg_check_modules(HWLOC hwloc) which finds the HOST's hwloc.pc and adds
# -I/usr/include (host glibc headers) to every cross compile — 500+ errors.
# The musl sysroot ships no .pc files, so hwloc correctly falls back to
# sysfs-based CPU pinning (no ARMRX_HAVE_HWLOC).
set(PKG_CONFIG_EXECUTABLE FALSE CACHE FILEPATH "pkg-config disabled for cross builds")

set(CMAKE_C_FLAGS_INIT   "-mtune=cortex-a53")
set(CMAKE_CXX_FLAGS_INIT "-mtune=cortex-a53")

set(CMAKE_FIND_ROOT_PATH ${CROSS_ROOT}/aarch64-linux-musl)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
