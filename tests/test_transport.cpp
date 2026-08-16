// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <ferrolink/ipc/spsc_shm_channel.hpp>
#include <ferrolink/ipc/transport.hpp>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <unordered_set>

namespace ferrolink::ipc::transport::test {

using ferrolink::ipc::shm_channel::SPSCShmChannel;
// Helper types

// A trivially copyable POD simulating a market message, with an embedded
// checksum so torn reads/writes (or cross-process corruption) are
// detectable rather than silently passing.
struct OrderMsg {
    std::uint64_t seq{};
    std::uint64_t price_ticks{};
    std::uint32_t qty{};
    std::uint32_t symbol_id{};
    std::uint64_t checksum{};

    static constexpr std::uint64_t compute_checksum(std::uint64_t seq,
                                                    std::uint64_t price_ticks,
                                                    std::uint32_t qty,
                                                    std::uint32_t symbol_id) noexcept {
        std::uint64_t h = seq * 1469598103934665603ull ^ price_ticks;
        h               = h * 1099511628211ull ^ (static_cast<std::uint64_t>(qty) << 32 | symbol_id);
        return h;
    }

    static OrderMsg make(std::uint64_t seq) noexcept {
        OrderMsg m;
        m.seq         = seq;
        m.price_ticks = seq * 7 + 100000;
        m.qty         = static_cast<std::uint32_t>((seq % 997) + 1);
        m.symbol_id   = static_cast<std::uint32_t>(seq % 32);
        m.checksum    = compute_checksum(m.seq, m.price_ticks, m.qty, m.symbol_id);
        return m;
    }

    bool is_internally_consistent() const noexcept {
        return checksum == compute_checksum(seq, price_ticks, qty, symbol_id);
    }
};
static_assert(std::is_trivially_copyable_v<OrderMsg>);

inline std::string unique_shm_name(const char* test_tag) {
    static std::atomic<std::uint64_t> counter{0};
    auto                              n = counter.fetch_add(1, std::memory_order_relaxed);
    return std::string{"ferrolink_xport_"} + test_tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(n);
}

// RAII guard so a failed assertion / early return doesn't leave a stale
// /dev/shm entry behind to break later tests via O_EXCL.
struct ShmNameGuard {
    std::string name;
    explicit ShmNameGuard(std::string n) : name{std::move(n)} {}
    ~ShmNameGuard() { ::shm_unlink(("/" + name).c_str()); }
    ShmNameGuard(const ShmNameGuard&)            = delete;
    ShmNameGuard& operator=(const ShmNameGuard&) = delete;
};

inline bool wait_child_success(pid_t pid) {
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

using Channel64 = SPSCShmChannel<OrderMsg, 1024>;
using Xport64   = Transport<Channel64, OrderMsg>;

// Static contract checks: role binding, non-copyable/non-movable.
//
// These are compile-time assertions. If any of them fail, the test binary
// fails to compile rather than fails at runtime — which is the point: the
// contract must be enforced at the type level, not discovered at runtime.
using ProducerType = decltype(Xport64::create(std::declval<std::string>()));
using ConsumerType = decltype(Xport64::attach(std::declval<std::string>()));

static_assert(std::is_same_v<ProducerType, TransportProducer<Channel64, OrderMsg>>,
              "Transport::create() must always yield a TransportProducer");
static_assert(std::is_same_v<ConsumerType, TransportConsumer<Channel64, OrderMsg>>,
              "Transport::attach() must always yield a TransportConsumer");

static_assert(!std::is_copy_constructible_v<TransportProducer<Channel64, OrderMsg>>);
static_assert(!std::is_copy_assignable_v<TransportProducer<Channel64, OrderMsg>>);
static_assert(!std::is_move_constructible_v<TransportProducer<Channel64, OrderMsg>>);
static_assert(!std::is_move_assignable_v<TransportProducer<Channel64, OrderMsg>>);

static_assert(!std::is_copy_constructible_v<TransportConsumer<Channel64, OrderMsg>>);
static_assert(!std::is_copy_assignable_v<TransportConsumer<Channel64, OrderMsg>>);
static_assert(!std::is_move_constructible_v<TransportConsumer<Channel64, OrderMsg>>);
static_assert(!std::is_move_assignable_v<TransportConsumer<Channel64, OrderMsg>>);

// Transport itself must be a pure factory: not constructible, not
// copyable, not movable — there is never a reason to hold a Transport
// instance.
static_assert(!std::is_default_constructible_v<Xport64>);
static_assert(!std::is_copy_constructible_v<Xport64>);
static_assert(!std::is_move_constructible_v<Xport64>);

TEST(TransportStaticContractTest, RoleBindingAndNonTransferabilityCompile) {
    // Nothing to run — the interesting assertions above are all
    // compile-time. This test exists so the static_asserts show up under
    // a named test in the ctest/gtest report rather than being silent
    // file-scope noise that nobody notices if it's ever weakened.
    SUCCEED();
}

// Channel/Buffer concept satisfaction — positive and negative cases.

// A deliberately non-conforming mock: create()/attach() return the wrong
// type (void instead of MockChannel), so it must NOT satisfy Channel<>.
struct NonConformingChannel {
    struct Buffer {
        bool push(const OrderMsg&) noexcept { return true; }
        bool pop(OrderMsg&) noexcept { return true; }
    };

    static void create(std::string) noexcept {} // wrong return type
    static void attach(std::string) noexcept {} // wrong return type

    Buffer* buffer() noexcept { return nullptr; }
    bool    is_valid() const noexcept { return true; }
    bool    role() const noexcept { return true; }
};

static_assert(!Channel<NonConformingChannel, OrderMsg>,
              "A channel whose create()/attach() don't return Self must not satisfy Channel<>");

// A conforming mock that lets us control is_valid() at runtime — this is
// how we exercise the abort() path inside TransportProducer/Consumer that
// the real SPSCShmChannel can never reach (since it already aborts before
// returning an invalid instance).
struct ControllableMockChannel {
    struct Buffer {
        bool push(const OrderMsg&) noexcept { return true; }
        bool pop(OrderMsg&) noexcept { return true; }
    };

    static ControllableMockChannel create(std::string) noexcept {
        ControllableMockChannel ch;
        ch.valid_ = !force_invalid_on_next_construct;
        return ch;
    }
    static ControllableMockChannel attach(std::string) noexcept {
        ControllableMockChannel ch;
        ch.valid_ = !force_invalid_on_next_construct;
        return ch;
    }

    Buffer* buffer() noexcept { return &buf_; }
    bool    is_valid() const noexcept { return valid_; }
    bool    role() const noexcept { return true; }

    // Test-only knob: the real API has no way to construct an "invalid"
    // channel deliberately (create/attach abort first), so we simulate
    // that scenario here to verify TransportProducer/Consumer's own
    // defensive abort() actually fires when a conforming Channel hands
    // back is_valid()==false.
    static inline bool force_invalid_on_next_construct = false;

  private:
    bool   valid_{true};
    Buffer buf_{};
};

static_assert(Channel<ControllableMockChannel, OrderMsg>,
              "ControllableMockChannel must satisfy the Channel concept for this test to be meaningful");

using MockXport = Transport<ControllableMockChannel, OrderMsg>;

TEST(TransportConceptTest, RealChannelSatisfiesConcept) {
    static_assert(Channel<Channel64, OrderMsg>, "SPSCShmChannel<OrderMsg,1024> must satisfy Channel<>");
    SUCCEED();
}

TEST(TransportConceptTest, NonConformingChannelRejectedAtCompileTime) {
    // Re-assert here (in addition to file scope) so the failure surfaces
    // under a named test rather than as an opaque compile error far from
    // any test log.
    static_assert(!Channel<NonConformingChannel, OrderMsg>);
    SUCCEED();
}

// Fatal-contract test: invalid channel passed into TransportProducer/
// TransportConsumer must abort(), not silently construct.

TEST(TransportDeathTest, ProducerAbortsOnInvalidChannel) {
    ControllableMockChannel::force_invalid_on_next_construct = true;
    EXPECT_DEATH(
        {
            auto producer = MockXport::create("whatever");
            (void) producer;
        },
        "");
    ControllableMockChannel::force_invalid_on_next_construct = false;
}

TEST(TransportDeathTest, ConsumerAbortsOnInvalidChannel) {
    ControllableMockChannel::force_invalid_on_next_construct = true;
    EXPECT_DEATH(
        {
            auto consumer = MockXport::attach("whatever");
            (void) consumer;
        },
        "");
    ControllableMockChannel::force_invalid_on_next_construct = false;
}

// Single-process end-to-end correctness through the Transport API
// (try_send/try_recv), not the lower-level buffer directly.

class TransportSingleProcessTest : public ::testing::Test {
  protected:
    using Channel = SPSCShmChannel<std::uint64_t, 8>;
    using Xport   = Transport<Channel, std::uint64_t>;
};

TEST_F(TransportSingleProcessTest, FIFOOrderPreservedThroughTrySendRecv) {
    const auto   name = unique_shm_name("single_fifo");
    ShmNameGuard guard{name};

    auto producer = Xport::create(name);
    auto consumer = Xport::attach(name);

    for (std::uint64_t i = 0; i < Channel::Buffer::capacity(); ++i) {
        ASSERT_TRUE(producer.try_send(i));
    }
    EXPECT_FALSE(producer.try_send(999)) << "try_send must reject once the underlying buffer is full";

    for (std::uint64_t i = 0; i < Channel::Buffer::capacity(); ++i) {
        std::uint64_t out = 0;
        ASSERT_TRUE(consumer.try_recv(out));
        EXPECT_EQ(out, i) << "FIFO order violated via Transport API at index " << i;
    }

    std::uint64_t dummy = 0;
    EXPECT_FALSE(consumer.try_recv(dummy)) << "try_recv must reject once the underlying buffer is empty";
}

TEST_F(TransportSingleProcessTest, CreatorIsAlwaysProducerAttacherIsAlwaysConsumer) {
    // This test exists purely to document and lock in the role contract:
    // the object returned by create() only ever exposes try_send(), and
    // the object returned by attach() only ever exposes try_recv(). There
    // is no API to reverse this, unlike the raw SPSCShmChannel. We verify
    // this at the type level (already done via static_assert above) and
    // functionally here: data pushed via the producer is observable via
    // the consumer, and there is no possible call that would let the
    // consumer push or the producer pop (this wouldn't even compile, so
    // the absence of such calls in this test file is itself the
    // enforcement — nothing to run beyond a functional round trip).
    const auto   name = unique_shm_name("role_lock");
    ShmNameGuard guard{name};

    auto producer = Xport::create(name);
    auto consumer = Xport::attach(name);

    ASSERT_TRUE(producer.try_send(42));
    std::uint64_t out = 0;
    ASSERT_TRUE(consumer.try_recv(out));
    EXPECT_EQ(out, 42u);
}

// Cross-process IPC via fork(): real producer process + real consumer
// process, communicating exclusively through Transport::create()/
// attach(). Full saturation, checksum-verified payload integrity.
TEST(TransportCrossProcessTest, ProducerChildConsumerParentFullIntegrity) {
    const auto   name = unique_shm_name("xproc_basic");
    ShmNameGuard guard{name};

    constexpr std::uint64_t kMessageCount = 200'000;

    // Parent creates the channel BEFORE fork(), guaranteeing placement-new
    // has already run in shared memory before the child attaches (see the
    // architectural-contract note at the top of this file).
    //
    // We need the PRODUCER (creator) side to exist before fork() so the
    // child can safely attach(). But the role contract fixes create() to
    // Producer — so the parent must be the creator/producer here, and the
    // child attaches as consumer.
    auto producer = Xport64::create(name);

    pid_t pid = ::fork();
    ASSERT_NE(pid, -1) << "fork failed: " << std::strerror(errno);

    if (pid == 0) {
        // --- Child process: consumer ---
        auto consumer = Xport64::attach(name);

        std::uint64_t expected_seq = 0;
        std::uint64_t received     = 0;
        const auto    deadline     = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        while (received < kMessageCount) {
            OrderMsg out{};
            if (consumer.try_recv(out)) {
                if (!out.is_internally_consistent())
                    _exit(2);
                if (out.seq != expected_seq)
                    _exit(3);
                ++expected_seq;
                ++received;
            } else {
                if (std::chrono::steady_clock::now() >= deadline)
                    _exit(4);
                std::this_thread::yield();
            }
        }
        _exit(0);
    }

    // --- Parent process: producer ---
    for (std::uint64_t seq = 0; seq < kMessageCount; ++seq) {
        OrderMsg msg = OrderMsg::make(seq);
        while (!producer.try_send(msg)) {
            std::this_thread::yield();
        }
    }

    EXPECT_TRUE(wait_child_success(pid));
}

TEST(TransportCrossProcessTest, NoDuplicationNoLossUnderBurstyBackpressure) {
    const auto   name = unique_shm_name("xproc_bursty");
    ShmNameGuard guard{name};

    constexpr std::uint64_t kMessageCount = 50'000;

    auto producer = Xport64::create(name);

    pid_t pid = ::fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        auto consumer = Xport64::attach(name);

        std::unordered_set<std::uint64_t> seen;
        seen.reserve(kMessageCount);
        std::uint64_t received = 0;
        const auto    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        while (received < kMessageCount) {
            OrderMsg out{};
            if (consumer.try_recv(out)) {
                if (!out.is_internally_consistent())
                    _exit(2);
                auto [it, inserted] = seen.insert(out.seq);
                if (!inserted)
                    _exit(3); // duplicate
                ++received;
            } else {
                if (std::chrono::steady_clock::now() >= deadline)
                    _exit(4);
                std::this_thread::yield();
            }
        }
        if (seen.size() != kMessageCount)
            _exit(5);
        for (std::uint64_t seq = 0; seq < kMessageCount; ++seq) {
            if (!seen.contains(seq))
                _exit(6); // missing seq -> loss
        }
        _exit(0);
    }

    for (std::uint64_t seq = 0; seq < kMessageCount; ++seq) {
        OrderMsg msg = OrderMsg::make(seq);
        while (!producer.try_send(msg))
            std::this_thread::yield();

        if (seq % 4096 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    EXPECT_TRUE(wait_child_success(pid));
}

} // namespace ferrolink::ipc::transport::test
