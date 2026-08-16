// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <detail/shm_registry.hpp>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <semaphore.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace ferrolink::detail::shm_registry::test {

// Helpers
inline std::string unique_shm_name(const char* test_tag) {
    static std::atomic<std::uint64_t> counter{0};
    auto                              n = counter.fetch_add(1, std::memory_order_relaxed);
    return std::string{"ferrolink_registry_"} + test_tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(n);
}

// RAII guard so a failed assertion doesn't leave a stale /dev/shm entry
// behind for later tests. Registry tests generally rely on the signal
// handler to unlink, but this is a safety net, not the primary mechanism
// under test.
struct ShmNameGuard {
    std::string name;
    explicit ShmNameGuard(std::string n) : name{std::move(n)} {}
    ~ShmNameGuard() { ::shm_unlink(("/" + name).c_str()); }
    ShmNameGuard(const ShmNameGuard&)            = delete;
    ShmNameGuard& operator=(const ShmNameGuard&) = delete;
};

inline bool shm_segment_exists(const std::string& name) {
    int fd = ::shm_open(("/" + name).c_str(), O_RDONLY, 0660);
    if (fd >= 0) {
        ::close(fd);
        return true;
    }
    return false;
}

// Actually creates a POSIX shm object on disk (register_name() only
// tracks a name string for later unlink — it never creates the object
// itself), so signal-cleanup tests have something real for shm_unlink()
// to remove.
inline void create_real_shm_object(const std::string& name) {
    int fd = ::shm_open(("/" + name).c_str(), O_CREAT | O_EXCL | O_RDWR, 0660);
    ASSERT_GE(fd, 0) << "failed to create real shm object for test: " << std::strerror(errno);
    ASSERT_EQ(::ftruncate(fd, 4096), 0);
    ::close(fd);
}

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

inline bool wait_child_success(pid_t pid) {
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid)
        return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// register_name()/unregister() slot lifecycle — run in forked
// children so each test starts from a pristine copy of the global
// registry array rather than sharing accumulated state with other tests
// in the same process.
TEST(ShmRegistryLifecycleTest, RegisterThenUnregisterFreesSlotForReuse) {
    pid_t pid = ::fork();
    ASSERT_NE(pid, -1) << "fork failed: " << std::strerror(errno);

    if (pid == 0) {
        std::size_t slot_a = register_name("/ferrolink_registry_reuse_a");
        unregister(slot_a);
        std::size_t slot_b = register_name("/ferrolink_registry_reuse_b");

        // Freeing a slot and re-registering must reuse it (the
        // implementation scans for the first free slot linearly), not
        // grow into a fresh one — this is a direct, observable
        // consequence of the documented "first free slot" allocation
        // strategy, not an assumption.
        if (slot_b != slot_a)
            _exit(1);
        _exit(0);
    }

    EXPECT_TRUE(wait_child_success(pid));
}

TEST(ShmRegistryLifecycleTest, UnregisterOnAlreadyFreeSlotIsNoOpNotCrash) {
    pid_t pid = ::fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        std::size_t slot = register_name("/ferrolink_registry_double_free");
        unregister(slot);
        unregister(slot); // double-unregister: must be a safe no-op
        _exit(0);
    }

    EXPECT_TRUE(wait_child_success(pid));
}

TEST(ShmRegistryLifecycleTest, UnregisterOnOutOfRangeSlotIsNoOpNotCrash) {
    pid_t pid = ::fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        unregister(kMaxChannels);       // exactly one past the end
        unregister(kMaxChannels + 100); // wildly out of range
        _exit(0);
    }

    EXPECT_TRUE(wait_child_success(pid));
}

TEST(ShmRegistryLifecycleTest, LongNameIsTruncatedNotOverflowed) {
    pid_t pid = ::fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        std::string long_name(1000, 'x'); // far longer than the 256-byte buffer
        std::size_t slot = register_name(long_name.c_str());

        // register_name must not have overrun its fixed-size buffer or
        // corrupted adjacent registry state; a subsequent register/
        // unregister on a fresh slot must still work normally.
        std::size_t other_slot = register_name("/short_name_after_long_one");
        if (other_slot == slot)
            _exit(1); // must be a distinct slot

        unregister(slot);
        unregister(other_slot);
        _exit(0);
    }

    EXPECT_TRUE(wait_child_success(pid));
}

// Capacity exhaustion — filling every slot and registering one more
// must abort(). Run via EXPECT_DEATH in its own forked death-test
// child (gtest already forks internally for death tests, but we still
// want this isolated from any other test's registry state, so the
// whole scenario lives in a single self-contained test).
TEST(ShmRegistryDeathTest, ExceedingMaxChannelsAborts) {
    // EXPECT_DEATH forks internally; the registry-filling loop and the
    // final over-capacity call both run inside that forked child, so the
    // main test process's registry state is never touched.
    EXPECT_DEATH(
        {
            for (std::size_t i = 0; i < kMaxChannels; ++i) {
                std::string           name = "/ferrolink_registry_fill_" + std::to_string(i);
                [[maybe_unused]] auto res{register_name(name.c_str())};
            }
            // One more than capacity — must abort() per shm_registry's
            // documented fatal-on-exhaustion behavior.
            [[maybe_unused]] auto res{register_name("/ferrolink_registry_overflow")};
        },
        "");
}

// Signal-safety, tested directly against shm_registry's public API.
//
// IMPORTANT — actual observed contract, not an assumption:
// The handler is installed with SA_RESETHAND and its body (sig_handler)
// only iterates the registry and calls shm_unlink() on each in-use slot;
// it never re-raises the signal and never calls _exit()/abort() itself.
// Combined with SA_RESETHAND (which resets the signal's disposition to
// SIG_DFL as soon as the handler is entered), the practical behavior is:
//   1st SIGTERM/SIGINT -> registered shm objects get unlinked, handler
//                          returns, and the process CONTINUES RUNNING
//                          (the signal does not terminate it, because
//                          the handler doesn't ask for that).
//   2nd SIGTERM/SIGINT -> disposition is back to SIG_DFL by now, so this
//                          one terminates the process the normal way.
// This was verified empirically: a single SIGTERM against a child
// blocked in a sleep loop cleans up /dev/shm but the child survives past
// the signal. The tests below assert this real, two-signal behavior
// rather than "signal kills and cleans up in one shot", which is not
// what the current implementation does. If shm_registry is later changed
// to re-raise/exit after cleanup, these tests should be revisited to
// drop the second signal.
TEST(ShmRegistrySignalSafetyTest, SIGTERMUnlinksRegisteredSegmentButDoesNotTerminateProcess) {
    const auto     name = unique_shm_name("sigterm_only_registered");
    ShmNameGuard   guard{name};
    const auto     sem_name = unique_shm_name("sigterm_sem1");
    NamedSemaphore ready_sem{sem_name, 0};

    pid_t pid = ::fork();
    ASSERT_NE(pid, -1) << "fork failed: " << std::strerror(errno);

    if (pid == 0) {
        ensure_signal_handlers_registered();
        create_real_shm_object(name);
        [[maybe_unused]] auto res{register_name(("/" + name).c_str())};

        sem_t* child_sem = ::sem_open(("/" + sem_name).c_str(), 0);
        if (child_sem == SEM_FAILED)
            _exit(1);
        ::sem_post(child_sem);
        ::sem_close(child_sem);

        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        _exit(0); // reached only if the parent doesn't kill us first
    }

    ready_sem.wait();

    const auto pre_signal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shm_segment_exists(name)) {
        ASSERT_LT(std::chrono::steady_clock::now(), pre_signal_deadline)
            << "shm segment never became visible before signal could be sent";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_EQ(::kill(pid, SIGTERM), 0) << "failed to send SIGTERM: " << std::strerror(errno);

    const auto post_signal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (shm_segment_exists(name)) {
        ASSERT_LT(std::chrono::steady_clock::now(), post_signal_deadline)
            << "registered shm segment was not unlinked after SIGTERM";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // The process must still be alive: SA_RESETHAND + a handler with no
    // re-raise/_exit means a single SIGTERM cleans up but does not kill.
    ASSERT_EQ(::kill(pid, 0), 0) << "child unexpectedly died from a single SIGTERM; "
                                    "if shm_registry's handler now terminates the process, "
                                    "update this test to match the new contract";

    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

TEST(ShmRegistrySignalSafetyTest, SecondSIGTERMTerminatesProcessViaResetDisposition) {
    const auto     name = unique_shm_name("sigterm_second_kill");
    ShmNameGuard   guard{name};
    const auto     sem_name = unique_shm_name("sigterm_sem2");
    NamedSemaphore ready_sem{sem_name, 0};

    pid_t pid = ::fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        ensure_signal_handlers_registered();
        create_real_shm_object(name);
        [[maybe_unused]] auto res{register_name(("/" + name).c_str())};

        sem_t* child_sem = ::sem_open(("/" + sem_name).c_str(), 0);
        if (child_sem == SEM_FAILED)
            _exit(1);
        ::sem_post(child_sem);
        ::sem_close(child_sem);

        for (int i = 0; i < 300; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        _exit(42); // should not be reached under normal test operation
    }

    ready_sem.wait();

    const auto pre_signal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shm_segment_exists(name)) {
        ASSERT_LT(std::chrono::steady_clock::now(), pre_signal_deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_EQ(::kill(pid, SIGTERM), 0);

    const auto post_first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (shm_segment_exists(name)) {
        ASSERT_LT(std::chrono::steady_clock::now(), post_first_deadline)
            << "shm segment was not unlinked after the first SIGTERM";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(::kill(pid, 0), 0) << "child died after only one SIGTERM — contract assumption violated, "
                                    "see comment above ShmRegistrySignalSafetyTest";

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    ASSERT_EQ(::kill(pid, SIGTERM), 0);

    int   status = 0;
    pid_t waited = ::waitpid(pid, &status, 0);
    ASSERT_EQ(waited, pid);
    ASSERT_TRUE(WIFSIGNALED(status)) << "second SIGTERM should terminate the process via default "
                                        "disposition after SA_RESETHAND reset it";
    EXPECT_EQ(WTERMSIG(status), SIGTERM);
}

TEST(ShmRegistrySignalSafetyTest, SIGINTUnlinksRegisteredSegmentSameAsSigterm) {
    // shm_registry registers SIGTERM, SIGINT, SIGABRT and SIGSEGV
    // independently — a bug wiring up only one of them correctly would
    // slip past a SIGTERM-only test.
    const auto     name = unique_shm_name("sigint_only_registered");
    ShmNameGuard   guard{name};
    const auto     sem_name = unique_shm_name("sigint_sem");
    NamedSemaphore ready_sem{sem_name, 0};

    pid_t pid = ::fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        ensure_signal_handlers_registered();
        create_real_shm_object(name);
        [[maybe_unused]] auto res{register_name(("/" + name).c_str())};

        sem_t* child_sem = ::sem_open(("/" + sem_name).c_str(), 0);
        if (child_sem == SEM_FAILED)
            _exit(1);
        ::sem_post(child_sem);
        ::sem_close(child_sem);

        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        _exit(0);
    }

    ready_sem.wait();

    const auto pre_signal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shm_segment_exists(name)) {
        ASSERT_LT(std::chrono::steady_clock::now(), pre_signal_deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_EQ(::kill(pid, SIGINT), 0) << "failed to send SIGINT: " << std::strerror(errno);

    const auto post_signal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (shm_segment_exists(name)) {
        ASSERT_LT(std::chrono::steady_clock::now(), post_signal_deadline)
            << "registered shm segment was not unlinked after SIGINT";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

TEST(ShmRegistrySignalSafetyTest, UnregisteredSegmentSurvivesSignalUntouched) {
    // Negative case: a shm object that was created but NEVER passed to
    // register_name() must be left alone by the signal handler — the
    // handler only knows about slots it was explicitly told to track. If
    // this ever fails, the handler is iterating something other than (or
    // in addition to) its own registry array.
    const auto     tracked_name   = unique_shm_name("sigterm_tracked");
    const auto     untracked_name = unique_shm_name("sigterm_untracked");
    ShmNameGuard   tracked_guard{tracked_name};
    ShmNameGuard   untracked_guard{untracked_name};
    const auto     sem_name = unique_shm_name("sigterm_sem3");
    NamedSemaphore ready_sem{sem_name, 0};

    pid_t pid = ::fork();
    ASSERT_NE(pid, -1);

    if (pid == 0) {
        ensure_signal_handlers_registered();

        create_real_shm_object(tracked_name);
        [[maybe_unused]] auto res{register_name(("/" + tracked_name).c_str())};

        // Deliberately create but do NOT register this one.
        create_real_shm_object(untracked_name);

        sem_t* child_sem = ::sem_open(("/" + sem_name).c_str(), 0);
        if (child_sem == SEM_FAILED)
            _exit(1);
        ::sem_post(child_sem);
        ::sem_close(child_sem);

        for (int i = 0; i < 100; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        _exit(0);
    }

    ready_sem.wait();

    const auto pre_signal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shm_segment_exists(tracked_name) || !shm_segment_exists(untracked_name)) {
        ASSERT_LT(std::chrono::steady_clock::now(), pre_signal_deadline)
            << "one or both shm segments never became visible before signal could be sent";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_EQ(::kill(pid, SIGTERM), 0);

    const auto post_signal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (shm_segment_exists(tracked_name)) {
        ASSERT_LT(std::chrono::steady_clock::now(), post_signal_deadline)
            << "tracked (registered) shm segment was not unlinked after SIGTERM";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // The untracked segment must still be there — give it the same grace
    // window before asserting, in case cleanup is somehow still settling.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(shm_segment_exists(untracked_name))
        << "an shm segment that was never registered got unlinked by the signal handler — "
           "it must only touch names it was explicitly told to track";

    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

} // namespace ferrolink::detail::shm_registry::test
