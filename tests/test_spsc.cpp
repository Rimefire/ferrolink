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

TEST(SPSCBufferBasic, StartsEmpty) {
    SmallBuffer buf;
    EXPECT_TRUE(buf.empty());
    EXPECT_FALSE(buf.full());
    EXPECT_EQ(buf.filled_size(), 0u);
}

TEST(SPSCBufferBasic, CapacityMatchesTemplateParameter) {
    EXPECT_EQ(SmallBuffer::capacity(), 4u);
    EXPECT_EQ(MidBuffer::capacity(), 64u);
}

TEST(SPSCBufferBasic, SinglePushPopRoundTrip) {
    SmallBuffer buf;
    TickMessage in = make_tick(42);
    ASSERT_TRUE(buf.push(in));

    TickMessage out{};
    ASSERT_TRUE(buf.pop(out));
    EXPECT_EQ(in, out);
    EXPECT_TRUE(buf.empty());
}

TEST(SPSCBufferBasic, PopOnEmptyFails) {
    SmallBuffer buf;
    TickMessage out{};
    EXPECT_FALSE(buf.pop(out));
}

TEST(SPSCBufferBasic, PopOnEmptyDoesNotTouchOutput) {
    SmallBuffer buf;
    TickMessage sentinel = make_tick(9999);
    TickMessage out      = sentinel;
    EXPECT_FALSE(buf.pop(out));
    EXPECT_EQ(out, sentinel);
}

TEST(SPSCBufferBasic, FillToCapacityThenRejectsNextPush) {
    SmallBuffer buf;
    for (std::uint64_t i = 0; i < SmallBuffer::capacity(); ++i) {
        ASSERT_TRUE(buf.push(make_tick(i)));
    }
    EXPECT_TRUE(buf.full());
    EXPECT_EQ(buf.filled_size(), SmallBuffer::capacity());
    EXPECT_FALSE(buf.push(make_tick(999999)));
}

TEST(SPSCBufferBasic, DrainToEmptyThenRejectsNextPop) {
    SmallBuffer buf;
    for (std::uint64_t i = 0; i < SmallBuffer::capacity(); ++i) {
        ASSERT_TRUE(buf.push(make_tick(i)));
    }
    TickMessage out{};
    for (std::uint64_t i = 0; i < SmallBuffer::capacity(); ++i) {
        ASSERT_TRUE(buf.pop(out));
        EXPECT_EQ(out.id, i);
    }
    EXPECT_TRUE(buf.empty());
    EXPECT_FALSE(buf.pop(out));
}

TEST(SPSCBufferBasic, FifoOrderPreservedUnderPartialDrain) {
    SmallBuffer buf;
    ASSERT_TRUE(buf.push(make_tick(1)));
    ASSERT_TRUE(buf.push(make_tick(2)));

    TickMessage out{};
    ASSERT_TRUE(buf.pop(out));
    EXPECT_EQ(out.id, 1u);

    ASSERT_TRUE(buf.push(make_tick(3)));

    ASSERT_TRUE(buf.pop(out));
    EXPECT_EQ(out.id, 2u);
    ASSERT_TRUE(buf.pop(out));
    EXPECT_EQ(out.id, 3u);
    EXPECT_TRUE(buf.empty());
}

TEST(SPSCBufferWraparound, SurvivesManyFullCyclesWithoutCorruption) {
    SmallBuffer   buf;
    constexpr int kCycles = 1'000'000;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        TickMessage in = make_tick(static_cast<std::uint64_t>(cycle));
        ASSERT_TRUE(buf.push(in));
        TickMessage out{};
        ASSERT_TRUE(buf.pop(out));
        ASSERT_EQ(in, out) << "corruption at cycle " << cycle;
    }
    EXPECT_TRUE(buf.empty());
}

TEST(SPSCBufferWraparound, IndicesGrowMonotonicallyAcrossManyWraps) {
    SmallBuffer   buf;
    constexpr int kPushes = 500'000;
    for (int i = 0; i < kPushes; ++i) {
        ASSERT_TRUE(buf.push(make_tick(static_cast<std::uint64_t>(i))));
        TickMessage out{};
        ASSERT_TRUE(buf.pop(out));
        ASSERT_EQ(out.id, static_cast<std::uint64_t>(i));
    }
}

TEST(SPSCBufferWraparound, PartialFillDrainCyclesAcrossWrap) {
    SmallBuffer   buf;
    std::uint64_t next_expected = 0;
    std::uint64_t next_id       = 0;

    for (int round = 0; round < 200'000; ++round) {
        int push_count = (round % 3) + 1;
        int pop_count  = (round % 2 == 0) ? push_count : push_count - 1;

        for (int i = 0; i < push_count && !buf.full(); ++i) {
            ASSERT_TRUE(buf.push(make_tick(next_id)));
            ++next_id;
        }
        for (int i = 0; i < pop_count && !buf.empty(); ++i) {
            TickMessage out{};
            ASSERT_TRUE(buf.pop(out));
            ASSERT_EQ(out.id, next_expected);
            ++next_expected;
        }
    }
}

} // namespace ferrolink::ipc::spsc::test
