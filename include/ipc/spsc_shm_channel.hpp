#pragma once
#include "spsc_buffer.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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
        int fd{::shm_open(("/" + obj_name).c_str(), O_CREAT | O_EXCL | O_RDWR, 0660)};

        if (fd == -1) {
            std::print(stderr,
                       "[FATAL] Cannot get file descriptor for shared memory block. "
                       "ERRNO: {}\n",
                       std::strerror(errno));
            std::abort();
        }

        auto cleanup{[&fd]() { ::close(fd); }};

        if (::ftruncate(fd, shm_size_) != 0) {
            std::print(stderr, "[FATAL] Cannot truncate size of the object. ERRNO: {}\n", std::strerror(errno));
            cleanup();
            std::abort();
        }

        void* mmap_result{::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, fd, 0)};
        int   saved_errno{errno};
        cleanup();

        if (mmap_result == MAP_FAILED) {
            std::print(stderr, "[FATAL] Cannot map shared object to memory. ERRNO: {}\n", std::strerror(saved_errno));
            std::abort();
        }

        std::byte* shm_start{static_cast<std::byte*>(mmap_result)};

        if (::madvise(shm_start, shm_size_, MADV_SEQUENTIAL) != 0)
            std::print(stderr,
                       "[WARN] madvise with MADV_SEQUENTIAL in "
                       "SPSCShmChannel::create function failed. ERRNO: {}\n",
                       std::strerror(errno));
        if (::madvise(shm_start, shm_size_, MADV_WILLNEED) != 0)
            std::print(stderr,
                       "[WARN] madvise with MADV_WILLNEED in SPSCShmChannel::create "
                       "function failed. ERRNO: {}\n",
                       std::strerror(errno));
        if (::madvise(shm_start, shm_size_, MADV_HUGEPAGE) != 0)
            std::print(stderr,
                       "[WARN] madvise with MADV_HUGEPAGE in SPSCShmChannel::create "
                       "function failed. ERRNO: {}\n",
                       std::strerror(errno));

        if (::mlock(shm_start, shm_size_) != 0) {
            std::print(stderr, "[FATAL] Cannot lock virtual address space into RAM. ERRNO: {}\n", std::strerror(errno));
            std::abort();
        }

        Buffer* buffer{new (shm_start) Buffer{}};

        return SPSCShmChannel(shm_start, buffer, std::move(obj_name), 1);
    }

    static SPSCShmChannel attach(std::string obj_name) noexcept {
        int fd{::shm_open(("/" + obj_name).c_str(), O_RDWR, 0660)};

        if (fd == -1) {
            std::print(stderr,
                       "[FATAL] Cannot get file descriptor for shared memory block. "
                       "ERRNO: {}\n",
                       std::strerror(errno));
            std::abort();
        }

        auto cleanup{[&fd]() { ::close(fd); }};

        void* mmap_result{::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)};

        int saved_errno{errno};
        cleanup();

        if (mmap_result == MAP_FAILED) {
            std::print(stderr, "[FATAL] Cannot map shared object to memory. ERRNO: {}\n", std::strerror(saved_errno));
            std::abort();
        }

        std::byte* shm_start{static_cast<std::byte*>(mmap_result)};

        if (::madvise(shm_start, shm_size_, MADV_SEQUENTIAL) != 0)
            std::print(stderr,
                       "[WARN] madvise with MADV_SEQUENTIAL in "
                       "SPSCShmChannel::create function failed. ERRNO: {}\n",
                       std::strerror(errno));
        if (::madvise(shm_start, shm_size_, MADV_WILLNEED) != 0)
            std::print(stderr,
                       "[WARN] madvise with MADV_WILLNEED in SPSCShmChannel::create "
                       "function failed. ERRNO: {}\n",
                       std::strerror(errno));
        if (::madvise(shm_start, shm_size_, MADV_HUGEPAGE) != 0)
            std::print(stderr,
                       "[WARN] madvise with MADV_HUGEPAGE in SPSCShmChannel::create "
                       "function failed. ERRNO: {}\n",
                       std::strerror(errno));

        if (::mlock(shm_start, shm_size_) != 0) {
            std::print(stderr, "[FATAL] Cannot lock virtual address space into RAM. ERRNO: {}\n", std::strerror(errno));
            std::abort();
        }

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

    bool is_valid() const noexcept { return shm_start_ != nullptr && buffer_ != nullptr; }

    const std::string& name() const noexcept { return obj_name_; }

  private:
    explicit SPSCShmChannel(std::byte*  shm_start  = nullptr,
                            Buffer*     buffer     = nullptr,
                            std::string obj_name   = "",
                            bool        is_creator = 0)
        : obj_name_{std::move(obj_name)}, shm_start_{shm_start}, buffer_{buffer}, is_creator_{is_creator} {}

    std::string                  obj_name_{""};
    static constexpr std::size_t shm_size_{sizeof(Buffer)};
    std::byte*                   shm_start_{nullptr};
    Buffer*                      buffer_{nullptr};
    bool                         is_creator_{false};
};
} // namespace ferrolink::ipc::shm_channel
