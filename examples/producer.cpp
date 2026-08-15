// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

// Manual, hands-on example: run this in one terminal, then run `consumer`
// in another. This process creates the shared-memory channel and streams
// simulated market ticks into it at a fixed rate, forever, until you stop
// it with Ctrl+C.
//
// This is NOT part of the automated test suite (see tests/test_transport.cpp
// for the fork()-based automated equivalent of this same producer/consumer
// relationship) — it exists so you can watch two real, separate OS
// processes talk to each other over ferrolink::ipc::transport with your
// own eyes, and use it as a starting point for your own producer code.
//
// Usage:
//   ./producer                 # send one tick roughly every 200ms, forever
//   ./producer --rate-hz 1000  # send ~1000 ticks/sec instead
//   ./producer --count 100000  # stop automatically after N ticks
//
// IMPORTANT ordering requirement: start this process BEFORE consumer.
// Transport::create() (called here) allocates the underlying POSIX
// shared-memory object; Transport::attach() (called by consumer) expects
// that object to already exist and aborts if it doesn't. This mirrors the
// real requirement documented on SPSCShmChannel::attach().

#include "market_tick.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ipc/spsc_shm_channel.hpp>
#include <ipc/transport.hpp>
#include <optional>
#include <print>
#include <string>
#include <thread>

namespace ferrolink::examples::producer {

using ferrolink::examples::kChannelCapacity;
using ferrolink::examples::kChannelName;
using ferrolink::examples::MarketTick;

using Channel = ferrolink::ipc::shm_channel::SPSCShmChannel<MarketTick, kChannelCapacity>;
using Xport   = ferrolink::ipc::transport::Transport<Channel, MarketTick>;

/// @brief Set by the SIGINT/SIGTERM handler; polled by the send loop so we
///        exit the loop and print final stats instead of just dying.
///
/// A plain `std::atomic<bool>` with relaxed ops inside an async-signal
/// handler is the standard, safe pattern here: `std::atomic<bool>::store`
/// on a lock-free bool is async-signal-safe on every platform this project
/// targets (POSIX/Linux), unlike e.g. allocating, locking a mutex, or
/// calling most libc functions from a handler.
std::atomic<bool> g_stop_requested{false};

extern "C" void on_stop_signal(int /*signum*/) noexcept {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

void install_stop_handler() noexcept {
    struct sigaction act {};
    act.sa_handler = on_stop_signal;
    ::sigemptyset(&act.sa_mask);
    act.sa_flags = 0; // deliberately no SA_RESTART: we want blocking calls (if any) to
                      // return EINTR promptly rather than transparently retry, so a
                      // stuck producer still notices the stop request quickly.

    // Must be called AFTER Transport::create()/ChannelType::create() — see the
    // ordering comment in main() for why. In short: shm_registry installs its
    // own SIGINT/SIGTERM handlers the first time a channel is created, and
    // that installation overwrites ours if we went first. Calling this after
    // create() means we install last and our handler is the one active for
    // the rest of the process's life; shm_registry's crash-safety handlers
    // remain the backstop for paths this graceful handler doesn't cover
    // (SIGSEGV, or a SIGKILL that can't be caught at all).
    ::sigaction(SIGINT, &act, nullptr);
    ::sigaction(SIGTERM, &act, nullptr);
}

struct Options {
    double                       rate_hz = 5.0; // ~1 tick every 200ms by default: slow enough to watch
    std::optional<std::uint64_t> count;         // unset = run forever until Ctrl+C
};

[[noreturn]] void usage_error(const char* prog, const std::string& message) {
    std::print(stderr, "error: {}\n\nusage: {} [--rate-hz N] [--count N]\n", message, prog);
    std::exit(2);
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg        = argv[i];
        auto             next_value = [&](const char* flag) -> std::string {
            if (i + 1 >= argc)
                usage_error(argv[0], std::string{"missing value for "} + flag);
            return argv[++i];
        };

        if (arg == "--rate-hz") {
            std::string val = next_value("--rate-hz");
            try {
                opts.rate_hz = std::stod(val);
            } catch (const std::exception&) {
                usage_error(argv[0], "--rate-hz must be a positive number");
            }
            if (!(opts.rate_hz > 0.0))
                usage_error(argv[0], "--rate-hz must be a positive number");
        } else if (arg == "--count") {
            std::string val = next_value("--count");
            try {
                opts.count = static_cast<std::uint64_t>(std::stoull(val));
            } catch (const std::exception&) {
                usage_error(argv[0], "--count must be a non-negative integer");
            }
        } else if (arg == "--help" || arg == "-h") {
            std::print("usage: {} [--rate-hz N] [--count N]\n"
                       "  --rate-hz N   ticks per second to send (default: 5)\n"
                       "  --count N     stop after N ticks instead of running forever\n",
                       argv[0]);
            std::exit(0);
        } else {
            usage_error(argv[0], "unrecognized argument '" + std::string{arg} + "'");
        }
    }
    return opts;
}

/// @brief Deterministically synthesizes a plausible-looking tick from a
///        sequence number, so both producer and (independently, if you
///        write your own verifying consumer) consumer can agree on what a
///        given seq's fields "should" be without any extra coordination.
MarketTick make_tick(std::uint64_t seq) noexcept {
    MarketTick tick;
    tick.seq         = seq;
    tick.symbol_id   = static_cast<std::uint32_t>(seq % 8); // rotate through 8 fake symbols
    tick.price_ticks = 10'000 + (seq % 500) * 3;            // wander in a small deterministic band
    tick.qty         = static_cast<std::uint32_t>(100 + (seq % 20) * 25);
    return tick;
}

} // namespace ferrolink::examples::producer

// main() must stay in the global namespace to remain a valid program entry
// point — a linker looks for a global `::main`, not a namespaced one. Pull
// in everything the producer namespace defined above instead of qualifying
// every single name here.
using namespace ferrolink::examples::producer;

int main(int argc, char** argv) {
    Options opts = parse_args(argc, argv);

    std::print("[producer] creating channel '{}' (capacity={})\n", kChannelName, kChannelCapacity);
    std::print("[producer] start consumer now, in another terminal, to see ticks arrive\n");

    // Transport::create() aborts the process on any unrecoverable OS-level
    // failure (see SPSCShmChannel::create's docs) rather than returning an
    // error we'd need to check here — a producer that can't even set up its
    // own channel has no reasonable way to continue, so a loud, immediate
    // failure is preferable to limping on.
    //
    // IMPORTANT: install_stop_handler() is called AFTER create(), not
    // before. ChannelType::create() installs ferrolink::detail::shm_registry's
    // own SIGINT/SIGTERM handlers internally (for crash-safety shm cleanup)
    // the first time any channel is created/attached in this process — and
    // that installation unconditionally overwrites whatever handler was
    // previously registered for those signals, with no way to chain to it.
    // Installing ours first and create() second silently discarded our
    // handler and left Ctrl+C doing shm cleanup only, never actually
    // stopping the send loop. Installing ours after create() ensures ours
    // is the one left in place.
    auto producer = Xport::create(kChannelName);
    install_stop_handler();

    std::print("[producer] channel ready, sending at ~{:.1f} ticks/sec"
               "{}. Press Ctrl+C to stop.\n",
               opts.rate_hz,
               opts.count ? std::format(", stopping after {} ticks", *opts.count) : "");

    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / opts.rate_hz));

    std::uint64_t sent           = 0;
    std::uint64_t dropped_full   = 0;
    auto          next_send_time = std::chrono::steady_clock::now();
    auto          last_report    = next_send_time;

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        if (opts.count && sent >= *opts.count)
            break;

        MarketTick tick = make_tick(sent);

        // try_send() is non-blocking; a `false` return means the ring
        // buffer is full (the consumer isn't keeping up). At this example's
        // deliberately slow default rate that should never happen, but a
        // real producer must always handle it — here we count it and move
        // on rather than spinning forever on a single tick, so one slow
        // consumer can't wedge the whole feed.
        if (producer.try_send(tick)) {
            ++sent;
        } else {
            ++dropped_full;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(2)) {
            std::print("[producer] sent={} dropped(buffer full)={}\n", sent, dropped_full);
            last_report = now;
        }

        next_send_time += interval;
        if (next_send_time > now) {
            std::this_thread::sleep_until(next_send_time);
        } else {
            // We've fallen behind schedule (e.g. terminal I/O above took a
            // while); resync rather than sleeping for a negative/near-zero
            // duration on every subsequent iteration.
            next_send_time = now;
        }
    }

    std::print("[producer] stopping. total sent={} dropped(buffer full)={}\n", sent, dropped_full);

    // No explicit cleanup call needed here: TransportProducer's destructor
    // (via its owned SPSCShmChannel) unlinks the shared-memory object and
    // deregisters it from shm_registry automatically when `producer` goes
    // out of scope at the end of main(). This graceful exit path (loop
    // notices g_stop_requested, breaks, falls through to here) is what
    // performs the cleanup for SIGINT/SIGTERM — since install_stop_handler()
    // installs our handler after create(), it's ours (not shm_registry's)
    // that's active for those two signals now. shm_registry's own handlers
    // remain the backstop only for signals we don't install a handler for
    // here, namely SIGABRT and SIGSEGV, plus SIGKILL (which cannot be
    // caught by anyone and always leaves cleanup to shm_registry on the
    // next process to attach/create, or to manual `rm /dev/shm/...`).
    return 0;
}
