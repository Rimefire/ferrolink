// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#pragma once
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <print>
#include <sched.h>

/// @file pin.hpp
/// @brief Thread/process CPU affinity and scheduling helpers for benchmarking.
namespace ferrolink::bench {

/// @brief Pins the current process/thread to a specific logical core.
///
/// Non-fatal on failure (e.g. if that core number simply doesn't exist on this
/// machine) — prints a warning and keeps running on whatever core the scheduler
/// assigns.
///
/// @param core Logical CPU index to pin the calling thread to.
/// @return `true` on success, `false` if `sched_setaffinity` failed (a warning
///         is printed to stderr in that case).
inline bool pin_to_core(int core) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(core), &set);

    if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::print(stderr, "[WARN] sched_setaffinity(core={}) failed: {}\n", core, std::strerror(errno));
        return false;
    }
    return true;
}

/// @brief Attempts to switch the calling thread to the `SCHED_FIFO` real-time
///        scheduling policy at the given priority.
///
/// @warning `SCHED_FIFO` without a periodic `sched_yield()`/blocking call inside a
/// busy-spin loop can stall the entire CPU on a single-core or virtualized machine —
/// FIFO tasks are not voluntarily preempted by the scheduler. Only enable this when
/// you are certain that the producer and consumer are pinned to **different physical
/// cores** (ideally with `isolcpus`/`nohz_full` on them too), otherwise you risk a
/// full machine lockup that may require a hard reset. This function is not called by
/// default in any benchmark here.
///
/// @note Requires `CAP_SYS_NICE` / root.
///
/// @param priority SCHED_FIFO priority to request (default 80).
/// @return `true` on success, `false` if `sched_setscheduler` failed (a warning is
///         printed to stderr in that case, typically due to missing privileges).
inline bool try_realtime_priority(int priority = 80) noexcept {
    sched_param sp{};
    sp.sched_priority = priority;

    if (::sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        std::print(stderr,
                   "[WARN] sched_setscheduler(SCHED_FIFO) failed: {} (needs root/CAP_SYS_NICE for a fair run)\n",
                   std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace ferrolink::bench
