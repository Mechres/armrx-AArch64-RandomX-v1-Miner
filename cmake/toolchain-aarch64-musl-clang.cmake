# Clang-based cross-compile toolchain for the armrx device target.
#
# Device: MSM8929 (Cortex-A53), postmarketOS edge — Alpine musl 1.2.x.
# Host: x86_64 Linux. Toolchain: host clang 22.x + musl sysroot at
# /usr/aarch64-linux-musl/ (from the AUR aarch64-linux-musl-cross package).
#
# Purpose: A/B the GCC-16 cross build (the shipping baseline) against Clang's
# AArch64 codegen to see whether Clang emits fewer instructions per hash
# (Era II Phase 1.2 / GLM Tier 1-B). Zero source change — compiler only.
#
# Mirrors cmake/toolchain-aarch64-musl.cmake (GCC) except for the compiler
# driver and the --target/--sysroot flags clang requires for cross builds.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_ROOT /usr/aarch64-linux-musl)

# Clang as the driver for C/C++/ASM. clang cross-builds via --target + --sysroot.
set(CMAKE_C_COMPILER   /usr/bin/clang)
set(CMAKE_CXX_COMPILER /usr/bin/clang++)
set(CMAKE_ASM_COMPILER /usr/bin/clang)

# AUR cross-ar (binutils 2.44) HANGS on archive creation (same as GCC variant).
# GNU archive format is arch-independent, so host binutils ar/ranlib/nm handle
# aarch64 objects fine. FORCE is required (compiler detection shadows cache).
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
set(ARMRX_DISABLE_LTO ON CACHE BOOL "Disable LTO (musl + LTO crash workaround)" FORCE)

# pkg-config disabled for cross builds (same rationale as GCC toolchain).
set(PKG_CONFIG_EXECUTABLE FALSE CACHE FILEPATH "pkg-config disabled for cross builds")

# Clang cross flags: target triple + musl sysroot. -mcpu matches the GCC
# -mtune=cortex-a53 intent (mtune is a no-op on AArch64 perf per prior
# measurement, so this is a fair comparison). -fuse-ld=lld uses clang's
# built-in linker (no separate cross ld needed for static musl).
set(CLANG_CROSS_FLAGS "--target=aarch64-linux-musl --sysroot=${CROSS_ROOT} -mcpu=cortex-a53 -fuse-ld=lld")
set(CMAKE_C_FLAGS_INIT   "${CLANG_CROSS_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${CLANG_CROSS_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${CLANG_CROSS_FLAGS}")

set(CMAKE_FIND_ROOT_PATH ${CROSS_ROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
