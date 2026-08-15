// The server (ponger) is started ONCE, on a separate std::thread, BEFORE entering
// `for (auto _ : state)` — recreating the channel on every single ping-pong would be
// absurdly expensive and would distort the exact thing we're trying to measure. Each
// gbench iteration is exactly one round trip; timing is std::chrono::steady_clock,
// read before/after on the client thread only — portable across architectures,
// no CPU-specific cycle counters involved.
//
// gbench's own built-in "mean per iteration" (--benchmark_repetitions=K gives you
// stddev/CV across repeated FULL runs) is not the same thing as per-message percentile
// latency. So this benchmark additionally collects a vector of raw RTT samples and
// computes p50/p90/p99/p99.9/p99.99 by hand, stashing them into counters after the loop.

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <common/pin.hpp>
#include <common/spin.hpp>
#include <common/stats.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ipc/spsc_shm_channel.hpp>
#include <ipc/transport.hpp>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace ferrolink;
using clock_type = std::chrono::steady_clock;

struct alignas(64) PingMsg {
    std::uint64_t             seq;
    std::array<std::byte, 56> pad;
};
static_assert(sizeof(PingMsg) == 64);

constexpr std::size_t   kCapacity    = 1024; // ping-pong queue depth is almost always 0/1
constexpr std::uint64_t kWarmupCount = 50'000;

using Channel = ipc::shm_channel::SPSCShmChannel<PingMsg, kCapacity>;
using Trans   = ipc::transport::Transport<Channel, PingMsg>;

std::string unique_shm_name(const char* tag) {
    static std::atomic<std::uint64_t> counter{0};
    return std::string{"ferrolink_gbench_"} + tag + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

void BM_Latency(benchmark::State& state) {
    const int client_core{static_cast<int>(state.range(0))};
    const int server_core{static_cast<int>(state.range(1))};

    const std::string req_name{unique_shm_name("lat_req")};
    const std::string resp_name{unique_shm_name("lat_resp")};

    bench::pin_to_core(client_core);

    // Synchronous create BEFORE spawning the server thread — program order guarantees
    // the req channel exists by the time the server tries to attach.
    auto req_producer{Trans::create(req_name)};

    std::atomic<bool> resp_ready{false};
    std::atomic<bool> stop{false};

    std::thread server_thread{[&] {
        bench::pin_to_core(server_core);
        auto req_consumer{Trans::attach(req_name)};
        auto resp_producer{Trans::create(resp_name)};
        resp_ready.store(true, std::memory_order_release);

        PingMsg msg{};
        while (!stop.load(std::memory_order_acquire)) {
            if (req_consumer.try_recv(msg)) {
                while (!resp_producer.try_send(msg))
                    bench::cpu_relax();
            } else {
                bench::cpu_relax();
            }
        }
    }};

    while (!resp_ready.load(std::memory_order_acquire))
        bench::cpu_relax();
    auto resp_consumer{Trans::attach(resp_name)};

    PingMsg       out{};
    std::uint64_t seq{0};

    // Warmup — not counted, outside both the timer and the sample vector.
    for (std::uint64_t i{0}; i < kWarmupCount; ++i) {
        PingMsg msg{.seq = seq++, .pad = {}};
        while (!req_producer.try_send(msg))
            bench::cpu_relax();
        while (!resp_consumer.try_recv(out))
            bench::cpu_relax();
    }

    std::vector<double> rtt_ns;
    rtt_ns.reserve(static_cast<std::size_t>(state.max_iterations));

    for (auto _ : state) {
        PingMsg msg{.seq = seq++, .pad = {}};

        const auto t0{clock_type::now()};
        while (!req_producer.try_send(msg))
            bench::cpu_relax();
        while (!resp_consumer.try_recv(out))
            bench::cpu_relax();
        const auto t1{clock_type::now()};

        const double ns{std::chrono::duration<double, std::nano>(t1 - t0).count()};
        rtt_ns.push_back(ns);
        state.SetIterationTime(ns * 1e-9);
    }

    stop.store(true, std::memory_order_release);
    server_thread.join();

    bench::LatencyStats stats{bench::compute_latency_stats(rtt_ns)};

    state.counters["rtt_min_ns"]   = stats.min;
    state.counters["rtt_mean_ns"]  = stats.mean;
    state.counters["rtt_p50_ns"]   = stats.p50;
    state.counters["rtt_p90_ns"]   = stats.p90;
    state.counters["rtt_p99_ns"]   = stats.p99;
    state.counters["rtt_p999_ns"]  = stats.p999;
    state.counters["rtt_p9999_ns"] = stats.p9999;
    state.counters["rtt_max_ns"]   = stats.max;
    // One-way estimate = RTT / 2, assuming a symmetric channel in both directions.
    state.counters["one_way_p50_ns"] = stats.p50 * 0.5;
    state.counters["one_way_p99_ns"] = stats.p99 * 0.5;
}

} // namespace

// Args: {client_core, server_core}. Iterations is fixed explicitly — latency is
// interesting precisely over a large number of individual round trips, not over
// gbench's auto-picked count.
BENCHMARK(BM_Latency)->Args({2, 3})->Iterations(1'000'000)->UseManualTime()->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
