cmake_minimum_required(VERSION 3.20)

project(armrx VERSION 0.2.0 LANGUAGES C CXX ASM)

# Embed git SHA and build date for --version flag
# .git_sha is written by devbox_sync or the build host
file(READ .git_sha ARMRX_GIT_SHA)
string(STRIP "${ARMRX_GIT_SHA}" ARMRX_GIT_SHA)
if(ARMRX_GIT_SHA STREQUAL "")
    set(ARMRX_GIT_SHA "unknown")
endif()
string(TIMESTAMP ARMRX_BUILD_DATE "%Y-%m-%d" UTC)
add_compile_definitions(ARMRX_VERSION="${PROJECT_VERSION}"
    ARMRX_GIT_SHA="${ARMRX_GIT_SHA}"
    ARMRX_BUILD_DATE="${ARMRX_BUILD_DATE}")

option(ARMRX_ENABLE_NATIVE "Tune for the compiler host CPU" OFF)
option(ARMRX_BUILD_TESTS "Build unit tests" ON)
option(ARMRX_JIT_PROFILE "Enable per-hash JIT profiling (adds 3 clock_gettime syscalls per hash)" OFF)
option(ARMRX_ENABLE_JIT_FAST_DIV_SQRT "Enable fast Newton-Raphson JIT FDIV and FSQRT" OFF)
option(ARMRX_FAST_MATH "Enable -Ofast / -ffast-math for floating-point speed" OFF)
option(ARMRX_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ARMRX_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ARMRX_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
set(ARMRX_PGO "OFF" CACHE STRING "Profile-Guided Optimization: OFF, GENERATE, or USE")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

add_library(armrx_core
    src/aes.cpp
    src/aes_generator.cpp
    src/argon2.cpp
    src/blake2_generator.cpp
    src/blake2b.cpp
    src/cpu_features.cpp
    src/dataset.cpp
    src/memory.cpp
    src/superscalar.cpp
    src/vm.cpp
    src/aes_hash.cpp
    src/mining_engine.cpp
    src/soft_aes.cpp
    src/stratum_client.cpp
    src/config.cpp
    src/json.cpp
    src/pool_manager.cpp
    src/tui.cpp
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm64|aarch64_be")
    target_sources(armrx_core PRIVATE
        src/jit_compiler_a64.cpp
        src/jit_compiler_a64_static.S
        src/virtual_memory.c
    )
    target_compile_definitions(armrx_core PUBLIC ARMRX_HAVE_JIT=1)
    # Enable hardware AES and NEON crypto extensions on AArch64
    target_compile_options(armrx_core PRIVATE -march=armv8-a+crypto)
else()
    target_sources(armrx_core PRIVATE
        src/virtual_memory.c
    )
endif()

find_package(Threads REQUIRED)

# Optional OpenSSL for TLS pool connections
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    target_sources(armrx_core PRIVATE src/tls_client.cpp)
    target_link_libraries(armrx_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(armrx_core PUBLIC ARMRX_HAVE_TLS=1)
    message(STATUS "OpenSSL found — TLS pool connections enabled")
else()
    message(STATUS "OpenSSL not found — TLS pool connections disabled")
endif()

# Optional hwloc for topology-aware CPU pinning
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(HWLOC hwloc QUIET)
endif()
if(HWLOC_FOUND OR NOT PkgConfig_FOUND)
    if(NOT HWLOC_FOUND)
        find_path(HWLOC_INCLUDE_DIRS hwloc.h)
        find_library(HWLOC_LIBRARIES hwloc)
    endif()
    if(HWLOC_INCLUDE_DIRS AND HWLOC_LIBRARIES)
        target_include_directories(armrx_core PRIVATE ${HWLOC_INCLUDE_DIRS})
        target_link_libraries(armrx_core PRIVATE ${HWLOC_LIBRARIES})
        target_compile_definitions(armrx_core PUBLIC ARMRX_HAVE_HWLOC=1)
        message(STATUS "hwloc found — topology-aware CPU pinning enabled")
    else()
        message(STATUS "hwloc not found — using sysfs-based CPU pinning")
    endif()
else()
    message(STATUS "hwloc not found — using sysfs-based CPU pinning")
endif()

target_include_directories(armrx_core PUBLIC include)
target_link_libraries(armrx_core PUBLIC Threads::Threads)
target_compile_features(armrx_core PUBLIC cxx_std_20)
target_compile_options(armrx_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=fast -funroll-loops)

if(ARMRX_ENABLE_NATIVE)
    target_compile_options(armrx_core PRIVATE -mcpu=native)
endif()

if(ARMRX_FAST_MATH)
    message(STATUS "Fast math enabled (-Ofast)")
    target_compile_options(armrx_core PRIVATE -Ofast)
endif()

# Profile-Guided Optimization (PGO) — GCC only
# Note: LTO must be disabled when using PGO (LTO+gcov conflict on GCC 15)
if(ARMRX_PGO STREQUAL "GENERATE")
    message(STATUS "PGO: generating profile data (LTO disabled)")
    target_compile_options(armrx_core PRIVATE -fprofile-generate -fno-lto)
    target_link_options(armrx_core PRIVATE -fprofile-generate -fno-lto)
elseif(ARMRX_PGO STREQUAL "USE")
    message(STATUS "PGO: using profile data from build directory (LTO disabled)")
    target_compile_options(armrx_core PRIVATE -fprofile-use -fprofile-correction -fno-lto)
    target_link_options(armrx_core PRIVATE -fprofile-use -fno-lto)
endif()

if(ARMRX_JIT_PROFILE)
    target_compile_definitions(armrx_core PUBLIC ARMRX_JIT_PROFILE)
    message(STATUS "JIT profiling enabled — adds 3 clock_gettime syscalls per hash")
endif()

if(ARMRX_ENABLE_JIT_FAST_DIV_SQRT)
    target_compile_definitions(armrx_core PUBLIC ARMRX_JIT_FAST_DIV_SQRT)
    message(STATUS "Fast JIT FDIV and FSQRT (Newton-Raphson) enabled")
endif()

if(ARMRX_ENABLE_ASAN)
    target_compile_options(armrx_core PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(armrx_core PRIVATE -fsanitize=address)
    message(STATUS "AddressSanitizer enabled")
endif()

if(ARMRX_ENABLE_UBSAN)
    target_compile_options(armrx_core PRIVATE -fsanitize=undefined)
    target_link_options(armrx_core PRIVATE -fsanitize=undefined)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
endif()

if(ARMRX_ENABLE_TSAN)
    target_compile_options(armrx_core PRIVATE -fsanitize=thread)
    target_link_options(armrx_core PRIVATE -fsanitize=thread)
    message(STATUS "ThreadSanitizer enabled")
endif()

add_executable(armrx src/main.cpp)
target_link_libraries(armrx PRIVATE armrx_core)

if(ARMRX_BUILD_TESTS)
    enable_testing()
    add_executable(armrx_tests tests/test_blake2b.cpp)
    target_link_libraries(armrx_tests PRIVATE armrx_core)
    add_test(NAME armrx_tests COMMAND armrx_tests)

    add_executable(test_mining tests/test_mining.cpp)
    target_link_libraries(test_mining PRIVATE armrx_core)
    add_test(NAME test_mining COMMAND test_mining)

    add_executable(debug_hash tests/debug_hash.cpp)
    target_link_libraries(debug_hash PRIVATE armrx_core)

    add_executable(bench_armrx tests/bench_armrx.cpp)
    target_link_libraries(bench_armrx PRIVATE armrx_core)
    add_test(NAME bench_armrx COMMAND bench_armrx)

    # Opcode frequency benchmark (not part of regular CTest; invoke manually)
    add_executable(bench_opcodes tests/bench_opcodes.cpp)
    target_link_libraries(bench_opcodes PRIVATE armrx_core)
    add_test(NAME bench_opcodes COMMAND bench_opcodes)

    # JIT encoding and determinism tests
    add_executable(test_jit_encodings tests/test_jit_encodings.cpp)
    target_link_libraries(test_jit_encodings PRIVATE armrx_core)
    add_test(NAME test_jit_encodings COMMAND test_jit_encodings)

    add_executable(test_jit_determinism tests/test_jit_determinism.cpp)
    target_link_libraries(test_jit_determinism PRIVATE armrx_core)
    add_test(NAME test_jit_determinism COMMAND test_jit_determinism)
endif()

# Enable Interprocedural Optimization (LTO) if supported
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported AND NOT ARMRX_PGO STREQUAL "GENERATE" AND NOT ARMRX_PGO STREQUAL "USE")
    message(STATUS "LTO/IPO is supported and enabled")
    set_property(TARGET armrx_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET armrx PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    if(ARMRX_BUILD_TESTS)
        set_property(TARGET armrx_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET test_mining PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET bench_armrx PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET bench_opcodes PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET test_jit_encodings PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET test_jit_determinism PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
elseif(NOT ipo_supported)
    message(STATUS "LTO/IPO is not supported: ${ipo_error}")
endif()
