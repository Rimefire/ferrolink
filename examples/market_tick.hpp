#pragma once

#include <cstdint>
#include <type_traits>

/// @file market_tick.hpp
/// @brief Shared trivially-copyable message type for the producer/consumer
///        example pair.
///
/// Deliberately kept in its own header, included identically by both
/// producer.cpp and consumer.cpp, rather than defined separately in each
/// .cpp: since the two processes only ever agree on what's in shared memory
/// via this struct's layout, having a single source of truth for it avoids
/// the classic IPC bug of the two sides silently drifting out of sync.
namespace ferrolink::examples {

/// @brief A single simulated market data tick — one instrument's trade
///        print at one instant.
///
/// Trivially copyable (a `static_assert` below enforces this), as required
/// by ferrolink::ipc::spsc::TriviallyCopyable: the type is placed directly
/// into shared memory and copied in/out via memcpy-like semantics, with no
/// constructor/destructor run on the shared copy itself.
struct MarketTick {
    /// @brief Monotonically increasing sequence number assigned by the
    ///        producer, starting at 0. Lets the consumer detect gaps
    ///        (buffer overrun on the producer side) or, if it ever regresses,
    ///        a stale/corrupt read.
    std::uint64_t seq{};

    /// @brief Trade price in fixed-point ticks (i.e. price_ticks / 100 =
    ///        dollars), avoiding floating point in the wire format.
    std::uint64_t price_ticks{};

    /// @brief Trade quantity (number of shares/contracts).
    std::uint32_t qty{};

    /// @brief Opaque instrument identifier. This example doesn't map it to
    ///        a real symbol table — it only needs to look plausibly
    ///        "market data shaped" and be reproducible for verification.
    std::uint32_t symbol_id{};
};

static_assert(std::is_trivially_copyable_v<MarketTick>,
              "MarketTick must stay trivially copyable to be usable with SPSCBuffer/SPSCShmChannel");

/// @brief Name of the shared-memory object the two example processes agree
///        on out of band (i.e. by both hardcoding it, same as a config file
///        or command-line flag would in a real deployment).
inline constexpr const char* kChannelName = "ferrolink_example_ticks";

/// @brief Ring buffer capacity for the example channel. Must be a power of
///        two (enforced by SPSCBuffer's static_assert). 4096 is generously
///        larger than the example's slow, human-watchable send rate needs,
///        so try_send() never has a practical reason to report "full" here
///        — that failure path is still handled by the producer, just not
///        expected to trigger.
inline constexpr std::size_t kChannelCapacity = 4096;

} // namespace ferrolink::examples
