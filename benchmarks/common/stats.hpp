#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <print>
#include <vector>

namespace ferrolink::bench {

struct LatencyStats {
    std::size_t count{0};
    double      min{0}, mean{0}, stddev{0};
    double      p50{0}, p90{0}, p99{0}, p999{0}, p9999{0}, max{0};
};

// samples_ns is modified in place (sorted) — cheaper than making a copy for huge sample sets.
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
