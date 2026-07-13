cmake_minimum_required(VERSION 3.20)

project(armrx VERSION 0.1.0 LANGUAGES CXX)

option(ARMRX_ENABLE_NATIVE "Tune for the compiler host CPU" OFF)
option(ARMRX_BUILD_TESTS "Build unit tests" ON)

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
)

target_include_directories(armrx_core PUBLIC include)
target_compile_features(armrx_core PUBLIC cxx_std_20)
target_compile_options(armrx_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -frounding-math)

if(ARMRX_ENABLE_NATIVE)
    target_compile_options(armrx_core PRIVATE -mcpu=native)
endif()

add_executable(armrx src/main.cpp)
target_link_libraries(armrx PRIVATE armrx_core)

if(ARMRX_BUILD_TESTS)
    enable_testing()
    add_executable(armrx_tests tests/test_blake2b.cpp)
    target_link_libraries(armrx_tests PRIVATE armrx_core)
    add_test(NAME armrx_tests COMMAND armrx_tests)
endif()
