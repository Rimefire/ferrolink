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
