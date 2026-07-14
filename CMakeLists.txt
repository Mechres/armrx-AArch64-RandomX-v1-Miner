cmake_minimum_required(VERSION 3.20)

project(armrx VERSION 0.1.0 LANGUAGES C CXX ASM)

option(ARMRX_ENABLE_NATIVE "Tune for the compiler host CPU" OFF)
option(ARMRX_BUILD_TESTS "Build unit tests" ON)
option(ARMRX_JIT_PROFILE "Enable per-hash JIT profiling (adds 3 clock_gettime syscalls per hash)" OFF)

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

target_include_directories(armrx_core PUBLIC include)
target_link_libraries(armrx_core PUBLIC Threads::Threads)
target_compile_features(armrx_core PUBLIC cxx_std_20)
target_compile_options(armrx_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -ffp-contract=fast -funroll-loops)

if(ARMRX_ENABLE_NATIVE)
    target_compile_options(armrx_core PRIVATE -mcpu=native)
endif()

if(ARMRX_JIT_PROFILE)
    target_compile_definitions(armrx_core PUBLIC ARMRX_JIT_PROFILE)
    message(STATUS "JIT profiling enabled — adds 3 clock_gettime syscalls per hash")
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

    add_executable(bench_armrx tests/bench_armrx.cpp)
    target_link_libraries(bench_armrx PRIVATE armrx_core)
endif()

# Enable Interprocedural Optimization (LTO) if supported
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    message(STATUS "LTO/IPO is supported and enabled")
    set_property(TARGET armrx_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET armrx PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    if(ARMRX_BUILD_TESTS)
        set_property(TARGET armrx_tests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET test_mining PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET bench_armrx PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
else()
    message(STATUS "LTO/IPO is not supported: ${ipo_error}")
endif()
