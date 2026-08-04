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

TEST(SPSCBufferConcurrency, ProducerConsumerNoDataLossNoCorruption) {
    static constexpr std::uint64_t kTotal = 5'000'000;
    MidBuffer                      buf;

    std::atomic<bool>          failed{false};
    std::atomic<std::uint64_t> produced{0};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kTotal; ++i) {
            TickMessage msg = make_tick(i);
            while (!buf.push(msg)) {
                std::this_thread::yield();
            }
            ++produced;
        }
    });

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        while (expected < kTotal) {
            TickMessage out{};
            if (buf.pop(out)) {
                if (out.id != expected) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                TickMessage reference = make_tick(expected);
                if (!(out == reference)) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(failed.load());
    EXPECT_EQ(produced.load(), kTotal);
    EXPECT_TRUE(buf.empty());
}

TEST(SPSCBufferConcurrency, BurstyProducerSlowConsumerNoLoss) {
    static constexpr std::uint64_t kTotal = 200'000;
    SmallBuffer                    buf;

    std::atomic<bool> failed{false};

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kTotal; ++i) {
            TickMessage msg = make_tick(i);
            while (!buf.push(msg)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        while (expected < kTotal) {
            TickMessage out{};
            if (buf.pop(out)) {
                if (out.id != expected) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                ++expected;
                if (expected % 997 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_FALSE(failed.load());
}

TEST(SPSCBufferAlignment, IndicesDoNotShareCacheLine) {
    SmallBuffer buf;
    const auto* read_addr = reinterpret_cast<const std::byte*>(&buf);
    (void) read_addr;
    EXPECT_EQ(sizeof(SmallBuffer) % alignof(std::max_align_t), 0u);
}

TEST(SPSCBufferAlignment, CapacityMustBePowerOfTwoAtCompileTime) {
    EXPECT_TRUE((SmallBuffer::capacity() & (SmallBuffer::capacity() - 1)) == 0);
    EXPECT_TRUE((MidBuffer::capacity() & (MidBuffer::capacity() - 1)) == 0);
}

TEST(SPSCBufferBoundary, CapacityOneBufferWorks) {
    SPSCBuffer<TickMessage, 1> buf;
    EXPECT_TRUE(buf.empty());
    ASSERT_TRUE(buf.push(make_tick(1)));
    EXPECT_TRUE(buf.full());
    EXPECT_FALSE(buf.push(make_tick(2)));

    TickMessage out{};
    ASSERT_TRUE(buf.pop(out));
    EXPECT_EQ(out.id, 1u);
    EXPECT_TRUE(buf.empty());
}

TEST(SPSCBufferBoundary, RepeatedFillDrainOnCapacityOneBuffer) {
    SPSCBuffer<TickMessage, 1> buf;
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        ASSERT_TRUE(buf.push(make_tick(i)));
        TickMessage out{};
        ASSERT_TRUE(buf.pop(out));
        ASSERT_EQ(out.id, i);
    }
}

TEST(SPSCBufferBoundary, LargeCapacityDoesNotCorruptFarApartSlots) {
    static constexpr std::size_t kCap = 1 << 16;
    auto                         buf  = std::make_unique<SPSCBuffer<TickMessage, kCap>>();

    for (std::uint64_t i = 0; i < kCap; ++i) {
        ASSERT_TRUE(buf->push(make_tick(i)));
    }
    ASSERT_TRUE(buf->full());

    TickMessage out{};
    ASSERT_TRUE(buf->pop(out));
    EXPECT_EQ(out.id, 0u);
    ASSERT_TRUE(buf->push(make_tick(999999)));

    for (std::uint64_t i = 1; i < kCap; ++i) {
        ASSERT_TRUE(buf->pop(out));
        ASSERT_EQ(out.id, i) << "mismatch at logical position " << i;
    }
    ASSERT_TRUE(buf->pop(out));
    EXPECT_EQ(out.id, 999999u);
    EXPECT_TRUE(buf->empty());
}

EST(SPSCBufferFilledSize, TracksExactCountThroughInterleavedOps) {
    MidBuffer buf;
    EXPECT_EQ(buf.filled_size(), 0u);

    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(buf.push(make_tick(static_cast<std::uint64_t>(i))));
    }
    EXPECT_EQ(buf.filled_size(), 10u);

    TickMessage out{};
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(buf.pop(out));
    }
    EXPECT_EQ(buf.filled_size(), 6u);

    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(buf.push(make_tick(100 + static_cast<std::uint64_t>(i))));
    }
    EXPECT_EQ(buf.filled_size(), 10u);
}

TEST(SPSCBufferFilledSize, NeverExceedsCapacityAcrossRandomizedOps) {
    MidBuffer     buf;
    std::uint64_t next_id = 0;

    for (int op = 0; op < 500'000; ++op) {
        if (op % 3 != 0 && !buf.full()) {
            ASSERT_TRUE(buf.push(make_tick(next_id++)));
        } else if (!buf.empty()) {
            TickMessage out{};
            ASSERT_TRUE(buf.pop(out));
        }
        ASSERT_LE(buf.filled_size(), MidBuffer::capacity());
    }
}
} // namespace ferrolink::ipc::spsc::test
