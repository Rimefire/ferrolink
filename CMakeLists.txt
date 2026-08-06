cmake_minimum_required(VERSION 3.25)

project(ferrolink
    VERSION 0.3.0
    DESCRIPTION "Low-latency SPSC shared-memory IPC transport"
    LANGUAGES CXX
)

# Global settings
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always emit compile_commands.json (clang-tidy, IDE tooling, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Refuse in-source builds outright — an in-source build has burned every
# serious C++ project at least once.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed. Use: cmake -B build -S .")
endif()

# Only Linux is supported — this project depends on POSIX shm_open/mmap/mlock,
# not portable to Windows without a different backend.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ferrolink targets Linux only (POSIX shm/mmap/mlock).")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Release" "RelWithDebInfo" "MinSizeRel")

message(STATUS "ferrolink: build type       = ${CMAKE_BUILD_TYPE}")
message(STATUS "ferrolink: compiler         = ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# Options
option(FERROLINK_BUILD_TESTS      "Build unit/integration tests"        ON)
option(FERROLINK_BUILD_BENCHMARKS "Build latency/throughput benchmarks" ON)
option(FERROLINK_BUILD_EXAMPLES   "Build producer/consumer examples"    ON)
option(FERROLINK_ENABLE_SANITIZERS "Enable ASan+UBSan in Debug builds"  ON)
option(FERROLINK_NATIVE_ARCH      "Compile with -march=native"          ON)
option(FERROLINK_ENABLE_LTO       "Enable interprocedural optimization in Release" ON)
option(FERROLINK_WARNINGS_AS_ERRORS "Treat all warnings as errors"      ON)

# Compiler identity checks
set(FERROLINK_IS_GNU   FALSE)
set(FERROLINK_IS_CLANG FALSE)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(FERROLINK_IS_GNU TRUE)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(FERROLINK_IS_CLANG TRUE)
else()
    message(FATAL_ERROR
        "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}'. "
        "ferrolink supports GCC and Clang only.")
endif()

# Interface library: common compile options for every target in the project
# Everything (lib, tests, benchmarks, examples) links against this so the
# strict flags are enforced project-wide, not just in the library build.
add_library(ferrolink_options INTERFACE)
add_library(ferrolink::options ALIAS ferrolink_options)

target_compile_features(ferrolink_options INTERFACE cxx_std_23)

# Warnings: as strict as practically possible
set(FERROLINK_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wmisleading-indentation
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast
)

if(FERROLINK_WARNINGS_AS_ERRORS)
    list(APPEND FERROLINK_WARNING_FLAGS -Werror)
endif()

if(FERROLINK_IS_CLANG)
    # Clang doesn't know -Wduplicated-cond/-Wduplicated-branches/-Wlogical-op/
    # -Wuseless-cast — those are GCC-specific. Strip them for Clang builds.
    list(REMOVE_ITEM FERROLINK_WARNING_FLAGS
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wuseless-cast
    )
endif()

target_compile_options(ferrolink_options INTERFACE ${FERROLINK_WARNING_FLAGS})

# Architecture
if(FERROLINK_NATIVE_ARCH)
    target_compile_options(ferrolink_options INTERFACE -march=native -mtune=native)
endif()

# Frame pointers kept even in Release: on a latency-critical system you want
# perf/gdb to be able to unwind the stack in production without guesswork.
target_compile_options(ferrolink_options INTERFACE -fno-omit-frame-pointer)

# Per-build-type flags
target_compile_options(ferrolink_options INTERFACE
    $<$<CONFIG:Debug>:-Og -g3>
    $<$<CONFIG:Release>:-O3 -DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-O3 -g -DNDEBUG>
)

# Sanitizers (Debug only — never in Release, they change timing/ABI)
if(FERROLINK_ENABLE_SANITIZERS)
    target_compile_options(ferrolink_options INTERFACE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-sanitize-recover=all>
    )
    target_link_options(ferrolink_options INTERFACE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    )
endif()

# LTO in Release/RelWithDebInfo
if(FERROLINK_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT FERROLINK_IPO_SUPPORTED OUTPUT FERROLINK_IPO_ERROR)
    if(FERROLINK_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
        message(STATUS "ferrolink: LTO enabled for Release/RelWithDebInfo")
    else()
        message(WARNING "ferrolink: LTO requested but not supported: ${FERROLINK_IPO_ERROR}")
    endif()
endif()

# Core library — header-only IPC transport
add_library(ferrolink_ipc INTERFACE)
add_library(ferrolink::ipc ALIAS ferrolink_ipc)

target_include_directories(ferrolink_ipc INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ferrolink_ipc INTERFACE ferrolink::options)

# transport.cpp/.hpp are compiled (non-header-only) — build them as a
# separate static library and link it into the interface target so both
# header-only and compiled parts are pulled in by a single ferrolink::ipc.
add_library(ferrolink_ipc_impl STATIC
    src/transport.cpp
)
add_library(ferrolink::ipc_impl ALIAS ferrolink_ipc_impl)

target_include_directories(ferrolink_ipc_impl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(ferrolink_ipc_impl PUBLIC ferrolink::options)

target_link_libraries(ferrolink_ipc INTERFACE ferrolink::ipc_impl)

# rt is required on some glibc versions for shm_open/shm_unlink;
# pthread is required for std::atomic wait/notify and thread support.
find_library(FERROLINK_RT_LIBRARY rt)
if(FERROLINK_RT_LIBRARY)
    target_link_libraries(ferrolink_ipc_impl PUBLIC ${FERROLINK_RT_LIBRARY})
endif()

find_package(Threads REQUIRED)
target_link_libraries(ferrolink_ipc_impl PUBLIC Threads::Threads)

# Tests
if(FERROLINK_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarks
if(FERROLINK_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Examples
if(FERROLINK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Summary
message(STATUS "ferrolink: build tests      = ${FERROLINK_BUILD_TESTS}")
message(STATUS "ferrolink: build benchmarks = ${FERROLINK_BUILD_BENCHMARKS}")
message(STATUS "ferrolink: build examples   = ${FERROLINK_BUILD_EXAMPLES}")
message(STATUS "ferrolink: sanitizers       = ${FERROLINK_ENABLE_SANITIZERS} (Debug only)")
message(STATUS "ferrolink: native arch      = ${FERROLINK_NATIVE_ARCH}")
message(STATUS "ferrolink: warnings-as-errors = ${FERROLINK_WARNINGS_AS_ERRORS}")
