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
    )

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
