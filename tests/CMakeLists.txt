# Copyright (c) 2026-present Rimefire. All rights reserved.
# Proprietary and confidential — unauthorized copying, distribution, or use
# of this file, via any medium, is strictly prohibited. See LICENSE at the
# repository root.

# GoogleTest via FetchContent
# Pinned to a specific tag rather than a floating branch — a low-latency
# project should not have its test infrastructure silently change under it.
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

# Force GTest to match our project's runtime library / settings rather than
# picking its own defaults (relevant mainly on non-Linux, kept for hygiene).
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Helper: define a test executable + register it with ctest in one call
function(ferrolink_add_test test_name source_file)
    add_executable(${test_name} ${source_file})

    target_link_libraries(${test_name} PRIVATE
        ferrolink::ipc
        ferrolink::options
        GTest::gtest_main
    )

    # Death tests (ASSERT_DEATH on abort()) require the test binary itself
    # to be single-threaded at the point of the death-inducing call, and
    # gtest recommends the "threadsafe" death test style under threaded
    # programs. Kept explicit rather than relying on the default.
    target_compile_definitions(${test_name} PRIVATE
        GTEST_DEATH_TEST_STYLE="threadsafe"
    )

    gtest_discover_tests(${test_name}
        PROPERTIES
            TIMEOUT 60
            # IPC/fork-based tests must not run concurrently with each other
            # if they touch the same shm object names — force serial ctest
            # execution for this suite to avoid EEXIST races between tests.
            RUN_SERIAL TRUE
    )
endfunction()

# Test binaries
ferrolink_add_test(test_shm_channel  test_shm_channel.cpp)
ferrolink_add_test(test_spsc        test_spsc.cpp)
ferrolink_add_test(test_transport   test_transport.cpp)
ferrolink_add_test(test_shm_registry  test_shm_registry.cpp)
