#pragma once
#include <array>
#include <bit>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <print>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace ferrolink::detail::shm_registry {
namespace {
struct Slot {
    char shm_name[256]{};
    bool in_use{false};
};

inline constexpr std::size_t          kMaxChannels{32};
inline bool                           handlers_reg{false};
inline std::array<Slot, kMaxChannels> registry{};

inline void sig_handler([[maybe_unused]] int sig) noexcept {
    for (auto& slot : registry) {
        if (slot.in_use) {
            ::shm_unlink(slot.shm_name);
        }
    }
}
} // namespace

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
}

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
