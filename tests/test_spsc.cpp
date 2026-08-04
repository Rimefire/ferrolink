#include "ipc/spsc_buffer.hpp"

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>
#include <type_traits>
#include <vector>

namespace ferrolink::ipc::spsc::test {
struct TickMessage {
    std::uint64_t id{};
    double        price{};
    std::uint64_t volume{};
    std::uint64_t timestamp{};

    bool operator==(const TickMessage& other) const noexcept {
        return id == other.id && price == other.price && volume == other.volume && timestamp == other.timestamp;
    }
};
static_assert(std::is_trivially_copyable_v<TickMessage>);
static_assert(!std::is_copy_constructible_v<SPSCBuffer<TickMessage, 8>>);
static_assert(!std::is_copy_assignable_v<SPSCBuffer<TickMessage, 8>>);
static_assert(!std::is_move_constructible_v<SPSCBuffer<TickMessage, 8>>);
static_assert(!std::is_move_assignable_v<SPSCBuffer<TickMessage, 8>>);
static_assert(std::atomic<std::size_t>::is_always_lock_free);

TickMessage make_tick(std::uint64_t id) {
    return TickMessage{id, static_cast<double>(id) * 1.5, id * 10, id * 100};
}

using SmallBuffer = SPSCBuffer<TickMessage, 4>;
using MidBuffer   = SPSCBuffer<TickMessage, 64>;
} // namespace ferrolink::ipc::spsc::test
