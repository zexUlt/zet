cmake_minimum_required(VERSION 3.24)

# A single integer, bumped as a whole. Semantic versioning would promise
# compatibility per component, and there is only one thing to be compatible
# with: the protocol, which negotiates its own range (design.md §7).
project(zet
    VERSION 1
    DESCRIPTION "A terminal session that survives the network"
    LANGUAGES CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a preset: cmake --preset dev")
endif()

# Clang only, and deliberately so. One compiler means one set of diagnostics to
# satisfy, and the fuzzing and sanitizer story is built around it: libFuzzer is
# a Clang runtime, and -fsanitize=fuzzer has no GCC equivalent. Supporting a
# second compiler is a cost we take on when someone actually needs it.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "zet builds with Clang; found ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION}. Configure through a preset "
        "(cmake --preset dev), which pins the compiler.")
endif()

# 20 is what this is developed and tested against. Clang 18 paired with the
# libstdc++ that ships on Ubuntu 24.04 has no usable <expected>, which is not
# a corner of the standard library we can do without, so the floor is set to a
# version observed to work rather than to the oldest that might.
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
    message(FATAL_ERROR
        "Clang 20 or newer is required: the C++23 subset zet uses needs a "
        "working <expected>, which Clang 18 with Ubuntu's libstdc++ does not "
        "provide. Found ${CMAKE_CXX_COMPILER_VERSION}.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ZET_BUILD_TESTS "Build the test suite" ON)
option(ZET_WERROR "Treat warnings as errors" ON)

# Off by default because the targets only link under a preset that compiles
# with -fsanitize=fuzzer-no-link; switching it on anywhere else fails at the
# link step rather than producing something runnable.
option(ZET_BUILD_FUZZERS "Build the libFuzzer targets" OFF)

include(cmake/CompileOptions.cmake)

add_subdirectory(src)

if(ZET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(ZET_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
