// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <common/pin.hpp>
#include <common/spin.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ipc/spsc_shm_channel.hpp>
#include <ipc/transport.hpp>
#include <print>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using namespace ferrolink;

struct alignas(64) Msg {
    std::uint64_t             seq;
    std::array<std::byte, 56> payload;
};
static_assert(sizeof(Msg) == 64);

constexpr std::size_t   kCapacity    = 1 << 16;
constexpr std::uint64_t kWarmupCount = 100'000;

using Channel = ipc::shm_channel::SPSCShmChannel<Msg, kCapacity>;
using Trans   = ipc::transport::Transport<Channel, Msg>;

// A fresh shm object name on every benchmark function call (in case gbench decides to
// call it more than once — e.g. with --benchmark_repetitions > 1) — otherwise a second
// call would hit O_CREAT|O_EXCL with EEXIST if the first run somehow failed to shm_unlink.
std::string unique_shm_name(const char* tag) {
    static std::atomic<std::uint64_t> counter{0};
    return std::string{"ferrolink_gbench_"} + tag + "_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

void BM_Throughput(benchmark::State& state) {
    const std::uint64_t measure_count{static_cast<std::uint64_t>(state.range(0))};
    const int           producer_core{static_cast<int>(state.range(1))};
    const int           consumer_core{static_cast<int>(state.range(2))};

    for (auto _ : state) {
        const std::string name{unique_shm_name("throughput")};

        bench::pin_to_core(producer_core);

        // Producer is created synchronously, BEFORE the consumer thread is spawned —
        // program order guarantees the shm object already exists by the time the
        // consumer calls attach.
        auto producer{Trans::create(name)};

        std::atomic<double> consumer_elapsed_ns{0};

        std::thread consumer_thread{[&] {
            bench::pin_to_core(consumer_core);
            auto consumer{Trans::attach(name)};

            Msg out{};
            for (std::uint64_t i{0}; i < kWarmupCount; ++i) {
                while (!consumer.try_recv(out))
                    bench::cpu_relax();
            }

            const auto t_start{std::chrono::steady_clock::now()};
            for (std::uint64_t i{0}; i < measure_count; ++i) {
                while (!consumer.try_recv(out))
                    bench::cpu_relax();
            }
            const auto t_end{std::chrono::steady_clock::now()};

            consumer_elapsed_ns.store(std::chrono::duration<double, std::nano>(t_end - t_start).count(),
                                      std::memory_order_relaxed);
        }};

        Msg msg{};
        for (std::uint64_t i{0}; i < kWarmupCount; ++i) {
            msg.seq = i;
            while (!producer.try_send(msg))
                bench::cpu_relax();
        }

        const auto t0{std::chrono::steady_clock::now()};
        for (std::uint64_t i{0}; i < measure_count; ++i) {
            msg.seq = i;
            while (!producer.try_send(msg))
                bench::cpu_relax();
        }

        consumer_thread
            .join(); // wait until the consumer has actually drained everything — that's the end of the measurement
        const auto t1{std::chrono::steady_clock::now()};

        const double wall_seconds{std::chrono::duration<double>(t1 - t0).count()};
        state.SetIterationTime(wall_seconds);

        // Besides the total wall-time pipeline throughput, also stash the "pure"
        // consumer-side time in counters — if it's noticeably smaller than
        // wall_seconds, the bottleneck is on the producer side (and vice versa).
        state.counters["consumer_only_Mmsg_s"] =
            static_cast<double>(measure_count) / (consumer_elapsed_ns.load(std::memory_order_relaxed) / 1e3);
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(measure_count));
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(measure_count) *
                            static_cast<std::int64_t>(sizeof(Msg)));
    state.counters["msg_size_bytes"] = sizeof(Msg);
    state.counters["ring_capacity"]  = kCapacity;
}

} // namespace

// Args: {message_count, producer_core, consumer_core}.
// ->Iterations(1) — this is NOT a micro-benchmark with thousands of tiny runs, it's one
// big run over N messages; for repeat-run statistics use --benchmark_repetitions=K from
// the outside instead of gbench's auto iteration-count probing.
BENCHMARK(BM_Throughput)->Args({20'000'000, 2, 3})->Iterations(1)->UseManualTime()->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
