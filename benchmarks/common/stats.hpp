// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <print>
#include <vector>

/// @file stats.hpp
/// @brief Latency-sample statistics computation and reporting for benchmarks.
namespace ferrolink::bench {

/// @brief Summary statistics computed over a set of latency samples (in nanoseconds).
///
/// All fields default to zero, which is also the result returned by
/// compute_latency_stats() for an empty input.
struct LatencyStats {
    std::size_t count{0};  ///< Number of samples the statistics were computed over.
    double      min{0};    ///< Minimum sample value, in nanoseconds.
    double      mean{0};   ///< Arithmetic mean of all samples, in nanoseconds.
    double      stddev{0}; ///< Population standard deviation of the samples, in nanoseconds.
    double      p50{0};    ///< 50th percentile (median), in nanoseconds.
    double      p90{0};    ///< 90th percentile, in nanoseconds.
    double      p99{0};    ///< 99th percentile, in nanoseconds.
    double      p999{0};   ///< 99.9th percentile, in nanoseconds.
    double      p9999{0};  ///< 99.99th percentile, in nanoseconds.
    double      max{0};    ///< Maximum sample value, in nanoseconds.
};

/// @brief Computes min/mean/stddev/percentiles/max over a set of latency samples.
///
/// Percentiles are computed using the nearest-rank method:
/// `index = clamp(floor(p * n), 0, n - 1)` on the sorted sample array.
///
/// @param[in,out] samples_ns Latency samples in nanoseconds. Sorted in place as a
///                            side effect — this is cheaper than making a defensive
///                            copy for large sample sets, but means the caller's
///                            vector will come out sorted (and its order should not
///                            be relied upon afterwards).
/// @return A LatencyStats populated from @p samples_ns, or a default-constructed
///         (all-zero) LatencyStats if @p samples_ns is empty.
inline LatencyStats compute_latency_stats(std::vector<double>& samples_ns) noexcept {
    LatencyStats stats{};
    if (samples_ns.empty())
        return stats;

    std::sort(samples_ns.begin(), samples_ns.end());

    const std::size_t n{samples_ns.size()};
    stats.count = n;
    stats.min   = samples_ns.front();
    stats.max   = samples_ns.back();

    auto percentile{[&](double p) noexcept -> double {
        // Nearest-rank method: index = ceil(p * n) - 1, clamped to bounds.
        std::size_t idx{static_cast<std::size_t>(p * static_cast<double>(n))};
        if (idx >= n)
            idx = n - 1;
        return samples_ns[idx];
    }};

    stats.p50   = percentile(0.50);
    stats.p90   = percentile(0.90);
    stats.p99   = percentile(0.99);
    stats.p999  = percentile(0.999);
    stats.p9999 = percentile(0.9999);

    double sum{0};
    for (double v : samples_ns)
        sum += v;
    stats.mean = sum / static_cast<double>(n);

    double sq_sum{0};
    for (double v : samples_ns) {
        const double d{v - stats.mean};
        sq_sum += d * d;
    }
    stats.stddev = std::sqrt(sq_sum / static_cast<double>(n));

    return stats;
}

/// @brief Pretty-prints a LatencyStats report to stdout.
///
/// @param s     Statistics to print, typically produced by compute_latency_stats().
/// @param title Title line printed above the report (e.g. the benchmark name).
inline void print_latency_report(const LatencyStats& s, const char* title) noexcept {
    std::print("\n=== {} ===\n", title);
    std::print("samples : {}\n", s.count);
    std::print("min     : {:>10.1f} ns\n", s.min);
    std::print("mean    : {:>10.1f} ns   (stddev {:.1f} ns)\n", s.mean, s.stddev);
    std::print("p50     : {:>10.1f} ns\n", s.p50);
    std::print("p90     : {:>10.1f} ns\n", s.p90);
    std::print("p99     : {:>10.1f} ns\n", s.p99);
    std::print("p99.9   : {:>10.1f} ns\n", s.p999);
    std::print("p99.99  : {:>10.1f} ns\n", s.p9999);
    std::print("max     : {:>10.1f} ns\n", s.max);
}

} // namespace ferrolink::bench
