#include "spsc_shm_channel.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <numeric>
#include <optional>
#include <random>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace ferrolink::ipc::shm::test {

using ferrolink::ipc::shm_channel::SPSCShmChannel;
// Helper types

// A trivially copyable POD simulating a market message (order/tick).
// Carries a "checksum" field that lets us detect torn writes/reads: if the
// producer always writes seq with checksum = f(seq), and the consumer ever
// observes checksum != f(seq), the buffer handed out a partially-written
// element — that would be a fatal race against the SPSC contract.
struct alignas(64) OrderMsg {
    std::uint64_t seq{};
    std::uint64_t price_ticks{};
    std::uint32_t qty{};
    std::uint32_t symbol_id{};
    std::uint64_t checksum{};

    static constexpr std::uint64_t compute_checksum(std::uint64_t seq,
                                                    std::uint64_t price_ticks,
                                                    std::uint32_t qty,
                                                    std::uint32_t symbol_id) noexcept {
        // Simple mix, but sensitive to byte-level reordering/corruption.
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

// Produces a unique shm segment name per test so that parallel runs or
// repeated failures don't leave stale /dev/shm/xxx objects behind that
// would break neighboring tests via O_EXCL.
inline std::string unique_shm_name(const char* test_tag) {
    static std::atomic<std::uint64_t> counter{0};
    auto                              n = counter.fetch_add(1, std::memory_order_relaxed);
    return std::string{"ferrolink_test_"} + test_tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(n);
}

// RAII guarantee that we won't leave /dev/shm/<name> dangling if a test
// fails before the normal unlink path runs (e.g. EXPECT_DEATH forks a
// child process that never goes through the parent's regular destructor).
struct ShmNameGuard {
    std::string name;
    explicit ShmNameGuard(std::string n) : name{std::move(n)} {}
    ~ShmNameGuard() { ::shm_unlink(("/" + name).c_str()); }
    ShmNameGuard(const ShmNameGuard&)            = delete;
    ShmNameGuard& operator=(const ShmNameGuard&) = delete;
};

// A named semaphore for cross-process synchronization in fork-based tests,
// used when we need to strictly order "parent created the channel" ->
// "child attaches to it". This is not part of the contract under test,
// purely test scaffolding.
struct NamedSemaphore {
    std::string name;
    sem_t*      sem{nullptr};

    explicit NamedSemaphore(std::string n, unsigned init_value) : name{std::move(n)} {
        ::sem_unlink(("/" + name).c_str());
        sem = ::sem_open(("/" + name).c_str(), O_CREAT | O_EXCL, 0660, init_value);
        if (sem == SEM_FAILED) {
            std::perror("sem_open");
            std::abort();
        }
    }
    ~NamedSemaphore() {
        if (sem)
            ::sem_close(sem);
        ::sem_unlink(("/" + name).c_str());
    }
    void post() { ::sem_post(sem); }
    void wait() { ::sem_wait(sem); }

    NamedSemaphore(const NamedSemaphore&)            = delete;
    NamedSemaphore& operator=(const NamedSemaphore&) = delete;
};

// Waits for a child process to finish and returns true if it exited
// normally with code 0.
inline bool wait_child_success(pid_t pid) {
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Lifecycle & ownership & move semantics
class ShmChannelLifecycleTest : public ::testing::Test {
  protected:
    using Channel = SPSCShmChannel<int, 16>;
};

TEST_F(ShmChannelLifecycleTest, CreateProducesValidCreatorChannel) {
    const auto   name = unique_shm_name("create_valid");
    ShmNameGuard guard{name};

    Channel ch = Channel::create(name);

    EXPECT_TRUE(ch.is_valid());
    EXPECT_TRUE(ch.role()); // creator == true
    EXPECT_EQ(ch.name(), name);
    EXPECT_EQ(ch.shm_size(), sizeof(Channel::Buffer));
    ASSERT_NE(ch.buffer(), nullptr);
    EXPECT_TRUE(ch.buffer()->empty());
    EXPECT_FALSE(ch.buffer()->full());
    EXPECT_EQ(ch.buffer()->filled_size(), 0u);
}

TEST_F(ShmChannelLifecycleTest, AttachToExistingSegmentSucceeds) {
    const auto   name = unique_shm_name("attach_ok");
    ShmNameGuard guard{name};

    Channel creator = Channel::create(name);
    ASSERT_TRUE(creator.is_valid());

    Channel attacher = Channel::attach(name);

    EXPECT_TRUE(attacher.is_valid());
    EXPECT_FALSE(attacher.role()); // attach -> is_creator == false
    EXPECT_EQ(attacher.name(), name);
    // Both instances view the same logical buffer: a write through one
    // is visible through the other without any intermediate re-creation.
    ASSERT_TRUE(creator.buffer()->push(42));
    int out = 0;
    ASSERT_TRUE(attacher.buffer()->pop(out));
    EXPECT_EQ(out, 42);
}

TEST_F(ShmChannelLifecycleTest, OnlyCreatorUnlinksOnDestruction) {
    const auto name     = unique_shm_name("unlink_owner");
    const auto shm_path = "/dev/shm/" + name;

    {
        Channel creator = Channel::create(name);
        ASSERT_TRUE(creator.is_valid());

        {
            Channel attacher = Channel::attach(name);
            ASSERT_TRUE(attacher.is_valid());
            // Attacher is destroyed first — the segment must not disappear,
            // since unlink ownership belongs exclusively to the creator.
        }

        // The segment must still exist: attach() never performs an unlink.
        int fd = ::shm_open(("/" + name).c_str(), O_RDONLY, 0660);
        EXPECT_GE(fd, 0) << "shm segment must survive attacher destruction";
        if (fd >= 0)
            ::close(fd);
    }

    // After the creator is destroyed, the segment must be unlinked.
    int fd_after = ::shm_open(("/" + name).c_str(), O_RDONLY, 0660);
    EXPECT_LT(fd_after, 0) << "shm segment must be unlinked after creator destruction";
    if (fd_after >= 0)
        ::close(fd_after);
}

TEST_F(ShmChannelLifecycleTest, MoveConstructionTransfersOwnershipAndInvalidatesSource) {
    const auto   name = unique_shm_name("move_ctor");
    ShmNameGuard guard{name};

    Channel original = Channel::create(name);
    ASSERT_TRUE(original.is_valid());
    auto* buf_ptr = original.buffer();

    Channel moved{std::move(original)};

    EXPECT_TRUE(moved.is_valid());
    EXPECT_TRUE(moved.role());
    EXPECT_EQ(moved.buffer(), buf_ptr);
    EXPECT_EQ(moved.name(), name);

    // After the move, the source must be in a valid "empty" state
    // (swapped with a default-constructed temporary) and own no resource.
    EXPECT_FALSE(original.is_valid());
    EXPECT_EQ(original.buffer(), nullptr);
}

TEST_F(ShmChannelLifecycleTest, MoveAssignmentTransfersOwnershipAndUnlinksOldTarget) {
    const auto   name_a = unique_shm_name("move_assign_a");
    const auto   name_b = unique_shm_name("move_assign_b");
    ShmNameGuard guard_a{name_a};
    ShmNameGuard guard_b{name_b};

    Channel a = Channel::create(name_a);
    Channel b = Channel::create(name_b);
    ASSERT_TRUE(a.is_valid());
    ASSERT_TRUE(b.is_valid());

    a = std::move(b);

    // After the move-assign, `a` now owns whatever `b` used to own (name_b).
    EXPECT_EQ(a.name(), name_b);
    EXPECT_TRUE(a.role());

    // The old resource of `a` (name_a) was destroyed by the temporary
    // inside move-assign (see SPSCShmChannel::operator=: temp swap idiom),
    // so its shm segment must have been unlinked.
    int fd = ::shm_open(("/" + name_a).c_str(), O_RDONLY, 0660);
    EXPECT_LT(fd, 0) << "old resource of move-assigned-to object must be released";
    if (fd >= 0)
        ::close(fd);
}

TEST_F(ShmChannelLifecycleTest, DifferentInstancesHaveIndependentNamesAfterCreate) {
    const auto   name1 = unique_shm_name("indep_1");
    const auto   name2 = unique_shm_name("indep_2");
    ShmNameGuard g1{name1};
    ShmNameGuard g2{name2};

    Channel c1 = Channel::create(name1);
    Channel c2 = Channel::create(name2);

    EXPECT_NE(c1.name(), c2.name());
    EXPECT_NE(c1.buffer(), c2.buffer());

    ASSERT_TRUE(c1.buffer()->push(1));
    ASSERT_TRUE(c2.buffer()->push(2));

    int out1 = 0, out2 = 0;
    ASSERT_TRUE(c1.buffer()->pop(out1));
    ASSERT_TRUE(c2.buffer()->pop(out2));
    EXPECT_EQ(out1, 1);
    EXPECT_EQ(out2, 2);
}

// Contract violations — fatal by design (std::abort())
//
// These tests document that violating the contract (double-creating the
// same object, attaching to a non-existent one) is NOT a recoverable
// error, but a fatal abort. This matches the intended architecture ("by
// design"); the test merely documents and guards this behavior against an
// accidental regression to a "silent" failure mode.
class ShmChannelDeathTest : public ::testing::Test {
  protected:
    using Channel = SPSCShmChannel<int, 16>;
};

TEST_F(ShmChannelDeathTest, CreateWithExistingNameAborts) {
    const auto   name = unique_shm_name("double_create");
    ShmNameGuard guard{name};

    Channel first = Channel::create(name);
    ASSERT_TRUE(first.is_valid());

    EXPECT_DEATH(
        {
            Channel second = Channel::create(name); // O_CREAT|O_EXCL must fail -> abort
            (void) second;
        },
        "");
}

TEST_F(ShmChannelDeathTest, AttachToNonexistentNameAborts) {
    const auto name = unique_shm_name("attach_missing");
    // Explicitly guarantee that the segment does not exist.
    ::shm_unlink(("/" + name).c_str());

    EXPECT_DEATH(
        {
            Channel ch = Channel::attach(name);
            (void) ch;
        },
        "");
}

// FIFO / buffer-boundary correctness through a real shm channel, but
// within a single process (a fast sanity-check before the fork tests).
class ShmChannelBufferSemanticsTest : public ::testing::Test {
  protected:
    using Channel = SPSCShmChannel<std::uint64_t, 8>; // capacity=8, power of two

    void SetUp() override {
        name_ = unique_shm_name("buf_sem");
        guard_.emplace(name_);
        channel_.emplace(Channel::create(name_));
        ASSERT_TRUE(channel_->is_valid());
    }

    std::string                 name_;
    std::optional<ShmNameGuard> guard_;
    std::optional<Channel>      channel_;
};

TEST_F(ShmChannelBufferSemanticsTest, FIFOOrderPreserved) {
    auto* buf = channel_->buffer();
    for (std::uint64_t i = 0; i < Channel::Buffer::capacity(); ++i) {
        ASSERT_TRUE(buf->push(i));
    }
    EXPECT_TRUE(buf->full());
    EXPECT_FALSE(buf->push(999)); // must reject when full, no silent overwrite

    for (std::uint64_t i = 0; i < Channel::Buffer::capacity(); ++i) {
        std::uint64_t out = 0;
        ASSERT_TRUE(buf->pop(out));
        EXPECT_EQ(out, i) << "FIFO order violated at index " << i;
    }
    EXPECT_TRUE(buf->empty());

    std::uint64_t dummy = 0;
    EXPECT_FALSE(buf->pop(dummy)); // must reject when empty
}

TEST_F(ShmChannelBufferSemanticsTest, WrapAroundMaintainsOrderAndCounts) {
    auto*             buf = channel_->buffer();
    const std::size_t cap = Channel::Buffer::capacity();

    // Push values through the buffer totaling 10x its capacity, alternating
    // push/pop so the physical index (write_index & mask) wraps around
    // Capacity many times over. This catches any masking (mask_) errors at
    // the array boundary.
    std::uint64_t       produced = 0;
    std::uint64_t       consumed = 0;
    const std::uint64_t total    = cap * 10;

    while (consumed < total) {
        while (produced < total && buf->filled_size() < cap) {
            ASSERT_TRUE(buf->push(produced));
            ++produced;
        }
        std::uint64_t out = 0;
        if (buf->pop(out)) {
            EXPECT_EQ(out, consumed) << "order violated after wrap-around";
            ++consumed;
        }
    }
    EXPECT_EQ(produced, total);
    EXPECT_EQ(consumed, total);
    EXPECT_TRUE(buf->empty());
}

TEST_F(ShmChannelBufferSemanticsTest, FilledSizeTracksExactly) {
    auto*             buf = channel_->buffer();
    const std::size_t cap = Channel::Buffer::capacity();

    EXPECT_EQ(buf->filled_size(), 0u);
    for (std::size_t i = 0; i < cap; ++i) {
        ASSERT_TRUE(buf->push(i));
        EXPECT_EQ(buf->filled_size(), i + 1);
    }
    for (std::size_t i = 0; i < cap; ++i) {
        std::uint64_t out = 0;
        ASSERT_TRUE(buf->pop(out));
        EXPECT_EQ(buf->filled_size(), cap - i - 1);
    }
}
