#pragma once
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <format>
#include <ipc/spsc_buffer.hpp>
#include <print>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace ferrolink::ipc::shm_channel {
template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
class SPSCShmChannel {
  public:
    using Buffer = spsc::SPSCBuffer<T, Capacity>;
    static SPSCShmChannel create(std::string obj_name) noexcept {
        std::strncpy(shm_name_, ("/" + obj_name).c_str(), sizeof(shm_name_) - 1);

        struct sigaction sig_act {};
        register_signal_handlers(sig_act);

        int fd{open_shm(obj_name, O_CREAT | O_EXCL | O_RDWR)};

        auto cleanup{[&fd]() { ::close(fd); }};

        if (::ftruncate(fd, shm_size_) != 0) {
            std::print(stderr, "[FATAL] Cannot truncate size of the object. ERRNO: {}\n", std::strerror(errno));
            cleanup();
            std::abort();
        }

        void* mmap_result{::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)};
        int   saved_errno{errno};
        cleanup();

        if (mmap_result == MAP_FAILED) {
            std::print(stderr, "[FATAL] Cannot map shared object to memory. ERRNO: {}\n", std::strerror(saved_errno));
            std::abort();
        }

        std::byte* shm_start{static_cast<std::byte*>(mmap_result)};

        setup_memory(shm_start);

        Buffer* buffer{new (shm_start) Buffer{}};

        return SPSCShmChannel(shm_start, buffer, std::move(obj_name), 1);
    }

    static SPSCShmChannel attach(std::string obj_name) noexcept {
        int fd{open_shm(obj_name, O_RDWR)};

        auto cleanup{[&fd]() { ::close(fd); }};

        void* mmap_result{::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)};

        int saved_errno{errno};
        cleanup();

        if (mmap_result == MAP_FAILED) {
            std::print(stderr, "[FATAL] Cannot map shared object to memory. ERRNO: {}\n", std::strerror(saved_errno));
            std::abort();
        }

        std::byte* shm_start{static_cast<std::byte*>(mmap_result)};

        setup_memory(shm_start);

        Buffer* buffer{reinterpret_cast<Buffer*>(shm_start)};

        return SPSCShmChannel(shm_start, buffer, std::move(obj_name), 0);
    }

    SPSCShmChannel(const SPSCShmChannel&)            = delete;
    SPSCShmChannel& operator=(const SPSCShmChannel&) = delete;

    friend void swap(SPSCShmChannel& src, SPSCShmChannel& dest) noexcept {
        std::swap(src.obj_name_, dest.obj_name_);
        std::swap(src.shm_start_, dest.shm_start_);
        std::swap(src.buffer_, dest.buffer_);
        std::swap(src.is_creator_, dest.is_creator_);
    }

    SPSCShmChannel(SPSCShmChannel&& other) noexcept : SPSCShmChannel() { swap(*this, other); }

    SPSCShmChannel& operator=(SPSCShmChannel&& other) noexcept {
        SPSCShmChannel temp{std::move(other)};
        swap(*this, temp);
        return *this;
    }

    ~SPSCShmChannel() noexcept {
        if (shm_start_) {
            if (::munlock(shm_start_, shm_size_) != 0)
                std::print(stderr, "[WARN] munlock failed. ERRNO: {}\n", std::strerror(errno));
            if (::munmap(shm_start_, shm_size_) != 0)
                std::print(stderr, "[WARN] munmap failed. ERRNO: {}\n", std::strerror(errno));
            if (is_creator_) {
                if (::shm_unlink(("/" + obj_name_).c_str()) != 0)
                    std::print(stderr, "[WARN] shm_unlink failed. ERRNO: {}\n", std::strerror(errno));
            }
        }
    }

    bool role() const noexcept { return is_creator_; }

    const Buffer* buffer() const noexcept { return buffer_; }

    Buffer* buffer() noexcept { return buffer_; }

    bool               is_valid() const noexcept { return shm_start_ != nullptr && buffer_ != nullptr; };
    const std::string& name() const noexcept { return obj_name_; }

    std::size_t shm_size() const noexcept { return shm_size_; }

  private:
    explicit SPSCShmChannel(std::byte*  shm_start  = nullptr,
                            Buffer*     buffer     = nullptr,
                            std::string obj_name   = "",
                            bool        is_creator = 0)
        : obj_name_{std::move(obj_name)}, shm_start_{shm_start}, buffer_{buffer}, is_creator_{is_creator} {}

    static void setup_memory(std::byte* shm_start) noexcept {
        static constexpr std::pair<int, const char*> flags[]{
            {MADV_SEQUENTIAL, "MADV_SEQUENTIAL"},
            {MADV_WILLNEED, "MADV_WILLNEED"},
            {MADV_HUGEPAGE, "MADV_HUGEPAGE"},
        };

        for (const auto& [flagnum, name] : flags) {
            checked_syscall([&] { return ::madvise(shm_start, shm_size_, flagnum); },
                            std::format("madvise with {} in SPSCShmChannel::setup_memory function failed", name),
                            "WARNING");
        }

        if (!checked_syscall(
                [&] { return ::mlock(shm_start, shm_size_); }, "Cannot lock virtual address space into RAM", "FATAL")) {
            std::abort();
        }
    }

    static int open_shm(const std::string& name, int flags) noexcept {
        int fd{::shm_open(("/" + name).c_str(), flags, 0660)};

        if (fd == -1) {
            std::print(stderr,
                       "[FATAL] Cannot get file descriptor for shared memory block. ERRNO: {}\n",
                       std::strerror(errno));
            std::abort();
        }

        return fd;
    }

    static void sig_handler([[maybe_unused]] int sig) noexcept {
        if (shm_name_[0] != '\0') {
            ::shm_unlink(shm_name_);
        }
    }

    static void register_signal_handlers(struct sigaction& sa) noexcept {
        sa.sa_handler = sig_handler;
        sa.sa_flags   = SA_RESETHAND;

        if (!checked_syscall([&] { return ::sigemptyset(&sa.sa_mask); }, "Failed to initialize signal mask", "FATAL")) {
            std::abort();
        }

        static constexpr int signals[] = {SIGABRT, SIGTERM, SIGSEGV, SIGINT};
        for (int signum : signals) {
            if (!checked_syscall([&] { return ::sigaction(signum, &sa, nullptr); },
                                 std::format("Failed to register signal handler for signal {}", ::strsignal(signum)),
                                 "FATAL")) {
                std::abort();
            }
        }
    }

    template <typename Fn>
    static bool checked_syscall(Fn&& fn, const std::string& what, const std::string& status) noexcept {
        if (fn() != 0) {
            std::print(stderr, "[{}] {}. ERRNO: {}", status, what, std::strerror(errno));
            return false;
        }
        return true;
    }

    static char                  shm_name_[256];
    std::string                  obj_name_{""};
    static constexpr std::size_t shm_size_{sizeof(Buffer)};
    std::byte*                   shm_start_{nullptr};
    Buffer*                      buffer_{nullptr};
    bool                         is_creator_{false};
};
} // namespace ferrolink::ipc::shm_channel
