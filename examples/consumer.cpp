// consumer.cpp
//
// Manual, hands-on example: run `producer` in one terminal first, then run
// this in another. This process attaches to the shared-memory channel the
// producer created and prints each tick as it arrives, verifying sequence
// numbers stay contiguous along the way.
//
// This is NOT part of the automated test suite (see tests/test_transport.cpp
// for the fork()-based automated equivalent) — it exists so you can watch
// two real, separate OS processes talk to each other over
// ferrolink::ipc::transport with your own eyes, and use it as a starting
// point for your own consumer code.
//
// Usage:
//   ./consumer                  # attach and print every tick forever
//   ./consumer --quiet          # only print periodic stats, not every tick
//   ./consumer --count 100000   # stop automatically after N ticks received
//
// IMPORTANT ordering requirement: start `producer` BEFORE this process.
// Transport::attach() (called here) expects the underlying POSIX
// shared-memory object to already exist — it aborts the process if
// producer hasn't created it yet. If you see an abort on startup, that's
// almost certainly why; start producer first and try again.

#include "market_tick.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ipc/spsc_shm_channel.hpp>
#include <ipc/transport.hpp>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>

namespace {

using ferrolink::examples::kChannelCapacity;
using ferrolink::examples::kChannelName;
using ferrolink::examples::MarketTick;

using Channel = ferrolink::ipc::shm_channel::SPSCShmChannel<MarketTick, kChannelCapacity>;
using Xport   = ferrolink::ipc::transport::Transport<Channel, MarketTick>;

/// @brief Set by the SIGINT/SIGTERM handler; polled by the receive loop so
///        we exit and print final stats instead of just dying. See the
///        matching comment in producer.cpp for why a relaxed atomic<bool>
///        is the right (async-signal-safe) tool here.
std::atomic<bool> g_stop_requested{false};

extern "C" void on_stop_signal(int /*signum*/) noexcept {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

void install_stop_handler() noexcept {
    struct sigaction act {};
    act.sa_handler = on_stop_signal;
    ::sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    // Must be called after Transport::attach() — see the ordering comment
    // in main() (and the matching one in producer.cpp) for why: shm_registry
    // installs its own SIGINT/SIGTERM handlers on first channel use, which
    // would otherwise silently overwrite ours if we went first.
    ::sigaction(SIGINT, &act, nullptr);
    ::sigaction(SIGTERM, &act, nullptr);
}

struct Options {
    bool                         quiet = false;
    std::optional<std::uint64_t> count; // unset = run forever until Ctrl+C
};

[[noreturn]] void usage_error(const char* prog, const std::string& message) {
    std::print(stderr, "error: {}\n\nusage: {} [--quiet] [--count N]\n", message, prog);
    std::exit(2);
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--quiet") {
            opts.quiet = true;
        } else if (arg == "--count") {
            if (i + 1 >= argc)
                usage_error(argv[0], "missing value for --count");
            std::string val = argv[++i];
            try {
                opts.count = static_cast<std::uint64_t>(std::stoull(val));
            } catch (const std::exception&) {
                usage_error(argv[0], "--count must be a non-negative integer");
            }
        } else if (arg == "--help" || arg == "-h") {
            std::print("usage: {} [--quiet] [--count N]\n"
                       "  --quiet     only print periodic stats, not every tick\n"
                       "  --count N   stop after N ticks instead of running forever\n",
                       argv[0]);
            std::exit(0);
        } else {
            usage_error(argv[0], "unrecognized argument '" + std::string{arg} + "'");
        }
    }
    return opts;
}

} // namespace

int main(int argc, char** argv) {
    Options opts = parse_args(argc, argv);

    std::print("[consumer] attaching to channel '{}'...\n", kChannelName);
    std::print("[consumer] (if this aborts immediately, start producer first — see file header)\n");

    // Transport::attach() aborts the process if the shared-memory object
    // doesn't exist yet (i.e. producer hasn't run Transport::create() for
    // this name) — see SPSCShmChannel::attach()'s docs. There's no retry
    // loop here on purpose: for this example, "producer isn't up yet" is a
    // usage error the person running it should fix by starting producer
    // first, not a transient condition worth papering over silently.
    //
    // IMPORTANT: install_stop_handler() is called AFTER attach(), not
    // before — for the same reason as in producer.cpp. attach() installs
    // ferrolink::detail::shm_registry's own SIGINT/SIGTERM handlers
    // internally on first use, unconditionally overwriting any handler
    // already registered for those signals. Installing ours after attach()
    // ensures ours is the one left active.
    auto consumer = Xport::attach(kChannelName);
    install_stop_handler();
    std::print("[consumer] attached. waiting for ticks. Press Ctrl+C to stop.\n");

    std::uint64_t                received      = 0;
    std::uint64_t                gaps_detected = 0;
    std::optional<std::uint64_t> last_seq;
    auto                         last_report = std::chrono::steady_clock::now();

    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        if (opts.count && received >= *opts.count)
            break;

        MarketTick tick{};
        if (consumer.try_recv(tick)) {
            ++received;

            // Sequence numbers are assigned monotonically by the producer
            // (see market_tick.hpp / producer.cpp's make_tick()), so any
            // jump larger than 1 means the buffer overran between our
            // pop() calls and the producer's try_send() started returning
            // false — i.e. we're not keeping up. This is exactly the kind
            // of thing a real consumer should monitor for in production.
            if (last_seq && tick.seq != *last_seq + 1) {
                ++gaps_detected;
                std::print("[consumer] WARNING: sequence gap detected: {} -> {} (missed {} tick(s))\n",
                           *last_seq,
                           tick.seq,
                           tick.seq - *last_seq - 1);
            }
            last_seq = tick.seq;

            if (!opts.quiet) {
                std::print("[consumer] seq={:>8} symbol_id={} price={}.{:02} qty={}\n",
                           tick.seq,
                           tick.symbol_id,
                           tick.price_ticks / 100,
                           tick.price_ticks % 100,
                           tick.qty);
            }
        } else {
            // Buffer empty: nothing to do right now. A tight spin-loop
            // would be appropriate for a true low-latency consumer (see
            // the benchmarks for that style), but this is a human-watched
            // example printing to a terminal, so a short sleep keeps CPU
            // usage sane without meaningfully affecting what you observe.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        auto now = std::chrono::steady_clock::now();
        if (now - last_report >= std::chrono::seconds(2)) {
            std::print("[consumer] received={} gaps={}\n", received, gaps_detected);
            last_report = now;
        }
    }

    std::print("[consumer] stopping. total received={} gaps_detected={}\n", received, gaps_detected);
    return 0;
}
