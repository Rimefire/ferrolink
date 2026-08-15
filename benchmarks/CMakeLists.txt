# Google Benchmark via FetchContent
include(FetchContent)

FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.1
    GIT_SHALLOW    TRUE
)

# Google Benchmark bundles its own copy of googletest for its own test suite —
# we don't need that, and we already have our own GTest fetched above.
set(BENCHMARK_ENABLE_TESTING       OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL       OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS   OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR        OFF CACHE BOOL "" FORCE)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googlebenchmark)

# The benchmarks spawn producer/consumer via std::thread and pin them via
# sched_setaffinity — need Threads explicitly (don't rely on it leaking in
# transitively through benchmark::benchmark).
find_package(Threads REQUIRED)

# Benchmark binaries
#
# NOTE: benchmarks are deliberately NOT run under ASan/UBSan — sanitizer
# instrumentation changes timing characteristics and would produce
# meaningless latency numbers. FERROLINK_ENABLE_SANITIZERS only applies to
# Debug builds anyway; benchmarks should always be built Release/RelWithDebInfo.

function(ferrolink_add_benchmark bench_name source_file)
    add_executable(${bench_name} ${source_file})

    target_link_libraries(${bench_name} PRIVATE
        ferrolink::ipc
        ferrolink::options
        benchmark::benchmark
        benchmark::benchmark_main
        Threads::Threads
    )

    # Puts benchmarks/ itself on the include search path, so bench_latency.cpp
    # and bench_throughput.cpp can pull in the shared helpers as
    # `#include <common/pin.hpp>` instead of a quoted relative path — <> skips
    # the including file's own directory and goes straight to -I search paths,
    # which is exactly what needs adding here.
    target_include_directories(${bench_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # glibc < 2.34 needs -lrt for shm_open/shm_unlink; newer glibc folds it into
    # libc, linking it explicitly is harmless either way.
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(${bench_name} PRIVATE rt)
    endif()

    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" AND
       NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        message(WARNING
            "${bench_name}: building benchmarks in '${CMAKE_BUILD_TYPE}' "
            "mode produces meaningless latency numbers. "
            "Use -DCMAKE_BUILD_TYPE=Release or RelWithDebInfo.")
    endif()
endfunction()

ferrolink_add_benchmark(bench_latency     bench_latency.cpp)
ferrolink_add_benchmark(bench_throughput  bench_throughput.cpp)
