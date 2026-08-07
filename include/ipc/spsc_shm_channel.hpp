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
    static SPSCShmChannel create(std::string obj_name) noexcept;
    static SPSCShmChannel attach(std::string obj_name) noexcept;

    SPSCShmChannel(const SPSCShmChannel&)            = delete;
    SPSCShmChannel& operator=(const SPSCShmChannel&) = delete;

    friend void swap(SPSCShmChannel<T, Capacity>& src, SPSCShmChannel<T, Capacity>& dest) noexcept {
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

    static void setup_memory(std::byte* shm_start) noexcept;
    static int  open_shm(const std::string& name, int flags) noexcept;

    static void sig_handler([[maybe_unused]] int sig) noexcept;
    static void register_signal_handlers(struct sigaction& sa) noexcept;

    template <typename Fn>
    static bool checked_syscall(Fn&& fn, const std::string& what, const std::string& status) noexcept;

    static char                  shm_name_[256];
    std::string                  obj_name_{""};
    static constexpr std::size_t shm_size_{sizeof(Buffer)};
    std::byte*                   shm_start_{nullptr};
    Buffer*                      buffer_{nullptr};
    bool                         is_creator_{false};
};
} // namespace ferrolink::ipc::shm_channel

#include <ipc/spsc_shm_channel.inl>
