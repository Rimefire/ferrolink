// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#pragma once
#include <array>
#include <bit>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

/// @file shm_registry.hpp
/// @brief Process-global registry of live POSIX shared-memory object names, used to
///        `shm_unlink()` them on abnormal termination (crash-safety net).
///
/// Without this, a process that creates a `shm_open`'d segment and then crashes
/// (SIGSEGV/SIGABRT) or is killed (SIGTERM/SIGINT) would leak the shared-memory
/// object in `/dev/shm`, since POSIX shm objects outlive the process by default.
/// This registry lets a signal handler unlink any still-registered names before
/// the process dies.
namespace ferrolink::detail::shm_registry {
namespace {

/// @brief A single registry entry: a shared-memory object name plus a liveness flag.
struct Slot {
    char shm_name[256]{}; ///< Null-terminated shm object name (including leading '/').
    bool in_use{false};   ///< Whether this slot currently holds a live registration.
};

/// @brief Maximum number of shared-memory channels that can be registered at once.
inline constexpr std::size_t kMaxChannels{32};

/// @brief Whether ensure_signal_handlers_registered() has already installed the handlers.
inline bool handlers_reg{false};

/// @brief Fixed-size table of registered shm object names.
inline std::array<Slot, kMaxChannels> registry{};

/// @brief Async-signal-safe handler that unlinks every still-registered shm object.
///
/// Installed with `SA_RESETHAND`, so after firing once the default disposition for
/// that signal is restored and the process terminates normally (e.g. by re-raising
/// SIGSEGV/SIGABRT), rather than looping or leaving the signal permanently handled.
///
/// @param sig Signal number (unused; the same handler is shared by all registered
///            signals).
inline void sig_handler([[maybe_unused]] int sig) noexcept {
    for (auto& slot : registry) {
        if (slot.in_use) {
            ::shm_unlink(slot.shm_name);
        }
    }
}
} // namespace

/// @brief Installs the crash-safety signal handlers (SIGABRT, SIGTERM, SIGSEGV, SIGINT)
///        exactly once per process.
///
/// Idempotent — subsequent calls after the first successful registration are no-ops.
/// Must be called before register_name() is relied upon to protect against a crash.
///
/// @note Aborts the process (via `std::abort()`) if signal mask initialization or
///       `sigaction` registration fails, since a partially-installed crash handler
///       is worse than none (silent leak vs. loud failure).
inline void ensure_signal_handlers_registered() noexcept {
    if (handlers_reg)
        return;

    struct sigaction sig_act {};
    sig_act.sa_handler = sig_handler;

    if (::sigemptyset(&sig_act.sa_mask) != 0) {
        std::print(stderr, "[FATAL] Failed to initialize signal mask. ERRNO: {}\n", std::strerror(errno));
        std::abort();
    }

    sig_act.sa_flags = static_cast<int>(SA_RESETHAND);

    static constexpr int signals[]{SIGABRT, SIGTERM, SIGSEGV, SIGINT};
    for (int signum : signals) {
        if (::sigaction(signum, &sig_act, nullptr) != 0) {
            std::print(stderr,
                       "[FATAL] Failed to register signal handler for signal {}. ERRNO: {}",
                       ::strsignal(signum),
                       std::strerror(errno));
            std::abort();
        }
    }

    handlers_reg = true;
}

/// @brief Registers a shared-memory object name so it will be `shm_unlink()`'d by the
///        crash signal handler if the process dies unexpectedly.
///
/// @param name Shared-memory object name to register, including the leading '/'
///             (as passed to `shm_open`/`shm_unlink`). Truncated to 255 characters
///             plus a null terminator if longer.
/// @return The registry slot index, to be passed to unregister() on clean shutdown.
///
/// @note Aborts the process if no free slot is available (registry is full, i.e.
///       kMaxChannels channels are already registered).
inline std::size_t register_name(const char* name) noexcept {
    for (std::size_t slot{}; slot < registry.size(); ++slot) {
        if (!registry[slot].in_use) {
            std::strncpy(registry[slot].shm_name, name, sizeof(registry[slot].shm_name) - 1);
            registry[slot].shm_name[sizeof(registry[slot].shm_name) - 1] = '\0';
            registry[slot].in_use                                        = true;

            return slot;
        }
    }

    std::print(stderr, "[FATAL] No slots are available.\n");
    std::abort();
}

/// @brief Removes a previously-registered slot on clean (non-crash) shutdown.
///
/// Call this after the owning channel has already `shm_unlink()`'d its own object,
/// so the crash handler doesn't attempt to unlink a name that's already gone (or,
/// worse, been reused by an unrelated channel that happened to land in the same slot).
///
/// @param slot Slot index previously returned by register_name().
///
/// @note Non-fatal: prints a warning to stderr and returns without effect if
///       @p slot is out of range or not currently in use.
inline void unregister(std::size_t slot) noexcept {
    if (slot >= registry.size()) {
        std::print(stderr, "[WARN] Slot number is greater than registry size");
        return;
    }

    if (!registry[slot].in_use) {
        std::print(stderr, "[WARN] Slot not in use\n");
        return;
    }

    std::memset(registry[slot].shm_name, 0, sizeof(registry[slot].shm_name));
    registry[slot].in_use = false;
}
} // namespace ferrolink::detail::shm_registry
