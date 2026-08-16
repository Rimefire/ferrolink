// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#pragma once
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ferrolink/detail/shm_registry.hpp>
#include <fcntl.h>
#include <format>
#include <ferrolink/ipc/spsc_buffer.hpp>
#include <print>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

/// @file spsc_shm_channel.hpp
/// @brief POSIX shared-memory-backed transport for an spsc::SPSCBuffer, allowing two
///        separate processes to communicate as producer and consumer.
namespace ferrolink::ipc::shm_channel {

/// @brief Owns a POSIX shared-memory segment holding a single spsc::SPSCBuffer,
///        either as the segment's creator or as an attaching peer.
///
/// One process calls create() to allocate and initialize a named shared-memory
/// object containing the buffer; a second process calls attach() with the same
/// name to map the same segment and obtain a pointer to the same buffer. Only the
/// creator unlinks the shm object on destruction — the attaching side just unmaps.
///
/// Move-only: exactly one live SPSCShmChannel instance should own a given mapping
/// at a time (move transfers ownership; copying would create two independent
/// unmap/unlink attempts on the same memory).
///
/// @tparam T        Element type stored in the underlying buffer. Must satisfy
///                   spsc::TriviallyCopyable.
/// @tparam Capacity Ring buffer capacity, forwarded to spsc::SPSCBuffer. Must be a
///                   power of two.
template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
class SPSCShmChannel {
  public:
    /// @brief The underlying ring buffer type this channel maps into shared memory.
    using Buffer = spsc::SPSCBuffer<T, Capacity>;

    /// @brief Creates and initializes a new shared-memory segment holding a fresh
    ///        Buffer, and registers it with the crash-safety shm_registry.
    ///
    /// Must be called before any peer calls attach() with the same @p obj_name —
    /// attach() expects the segment to already exist (`O_RDWR` without `O_CREAT`).
    ///
    /// @param obj_name POSIX shared-memory object name, without the leading '/'
    ///                  (the '/' is added internally). Must be unique per channel
    ///                  instance to avoid `EEXIST` from the underlying `O_EXCL` open.
    /// @return A valid, owning SPSCShmChannel whose role() is `true` (creator).
    ///
    /// @note Aborts the process on any unrecoverable OS-level failure (shm_open,
    ///       ftruncate, mmap, mlock all abort on failure — see the .inl for details).
    [[nodiscard, gnu::cold]] static SPSCShmChannel create(std::string obj_name) noexcept;

    /// @brief Attaches to an existing shared-memory segment previously created by
    ///        create() with the same @p obj_name.
    ///
    /// @param obj_name POSIX shared-memory object name matching the one passed to
    ///                  the corresponding create() call.
    /// @return A valid, non-owning SPSCShmChannel whose role() is `false` (attacher).
    ///
    /// @note Aborts the process on any unrecoverable OS-level failure. Does not
    ///       retry if the segment does not yet exist — the caller is responsible
    ///       for ensuring create() has already run (e.g. via program order, as in
    ///       the benchmarks where the producer is created before the consumer
    ///       thread is spawned).
    [[nodiscard, gnu::cold]] static SPSCShmChannel attach(std::string obj_name) noexcept;

    /// @name Non-copyable
    ///@{
    SPSCShmChannel(const SPSCShmChannel&)            = delete;
    SPSCShmChannel& operator=(const SPSCShmChannel&) = delete;
    ///@}

    /// @brief Member-wise swap, used to implement move construction/assignment.
    friend void swap(SPSCShmChannel<T, Capacity>& src, SPSCShmChannel<T, Capacity>& dest) noexcept {
        std::swap(src.obj_name_, dest.obj_name_);
        std::swap(src.registry_slot_, dest.registry_slot_);
        std::swap(src.shm_start_, dest.shm_start_);
        std::swap(src.buffer_, dest.buffer_);
        std::swap(src.is_creator_, dest.is_creator_);
    }

    /// @brief Move-constructs from @p other, leaving @p other in a default
    ///        (invalid, `is_valid() == false`) state.
    SPSCShmChannel(SPSCShmChannel&& other) noexcept : SPSCShmChannel() { swap(*this, other); }

    /// @brief Move-assigns from @p other via swap; the previous contents of `*this`
    ///        are destroyed (unmapped/unlinked as appropriate) when @p temp goes
    ///        out of scope.
    SPSCShmChannel& operator=(SPSCShmChannel&& other) noexcept {
        SPSCShmChannel temp{std::move(other)};
        swap(*this, temp);
        return *this;
    }

    /// @brief Unlocks and unmaps the shared-memory mapping; if this instance is the
    ///        creator, also unlinks the shm object and deregisters it from the
    ///        crash-safety registry.
    ///
    /// A default-constructed or moved-from instance (`shm_start_ == nullptr`) is a
    /// safe no-op.
    ~SPSCShmChannel() noexcept {
        if (shm_start_) {
            if (::munlock(shm_start_, shm_size_) != 0)
                std::print(stderr, "[WARN] munlock failed. ERRNO: {}\n", std::strerror(errno));
            if (::munmap(shm_start_, shm_size_) != 0)
                std::print(stderr, "[WARN] munmap failed. ERRNO: {}\n", std::strerror(errno));
            if (is_creator_) {
                if (::shm_unlink(("/" + obj_name_).c_str()) != 0)
                    std::print(stderr, "[WARN] shm_unlink failed. ERRNO: {}\n", std::strerror(errno));
                detail::shm_registry::unregister(registry_slot_);
            }
        }
    }

    /// @brief Reports whether this instance created (`true`) or attached to (`false`)
    ///        the underlying shared-memory segment.
    ///
    /// Only the creator is responsible for `shm_unlink`-ing the segment on
    /// destruction, so this also indicates ownership of the segment's lifetime.
    [[nodiscard]] bool role() const noexcept { return is_creator_; }

    /// @brief Read-only access to the mapped buffer.
    /// @return Pointer to the Buffer inside the shared-memory segment, or `nullptr`
    ///         if this instance is default-constructed/moved-from.
    [[nodiscard]] const Buffer* buffer() const noexcept { return buffer_; }

    /// @brief Mutable access to the mapped buffer.
    /// @return Pointer to the Buffer inside the shared-memory segment, or `nullptr`
    ///         if this instance is default-constructed/moved-from.
    [[nodiscard]] Buffer* buffer() noexcept { return buffer_; }

    /// @brief Checks whether this channel holds a live shared-memory mapping.
    /// @return `true` if both the mapping and the buffer pointer are non-null.
    [[nodiscard]] bool is_valid() const noexcept { return shm_start_ != nullptr && buffer_ != nullptr; };

    /// @brief The POSIX shared-memory object name this channel was created/attached with.
    [[nodiscard]] const std::string& name() const noexcept { return obj_name_; }

    /// @brief Size in bytes of the mapped shared-memory segment (i.e. `sizeof(Buffer)`).
    [[nodiscard]] std::size_t shm_size() const noexcept { return shm_size_; }

  private:
    /// @brief Private constructor used internally by create()/attach() and the
    ///        default/moved-from state.
    explicit SPSCShmChannel(std::byte*  shm_start     = nullptr,
                            Buffer*     buffer        = nullptr,
                            std::string obj_name      = "",
                            std::size_t registry_slot = 0,
                            bool        is_creator    = 0)
        : obj_name_{std::move(obj_name)}, registry_slot_{registry_slot}, shm_start_{shm_start}, buffer_{buffer},
          is_creator_{is_creator} {}

    /// @brief Applies memory-management advice/locking to the mapped segment
    ///        (`madvise` hints plus `mlock` to pin it resident, preventing paging
    ///        latency spikes). Called by both create() and attach().
    [[gnu::cold]] static void setup_memory(std::byte* shm_start) noexcept;

    /// @brief Wraps `shm_open` with fatal (process-aborting) error handling.
    /// @param name  Object name without the leading '/'.
    /// @param flags Flags forwarded to `shm_open` (e.g. `O_CREAT | O_EXCL | O_RDWR`
    ///              for create(), `O_RDWR` for attach()).
    /// @return A valid file descriptor; never returns on failure (aborts instead).
    [[nodiscard, gnu::cold]] static int open_shm(const std::string& name, int flags) noexcept;

    /// @brief Runs @p fn, treating a non-zero return as failure: logs @p what
    ///        prefixed with @p status and returns `false`, or returns `true` on
    ///        success. Does not itself abort — callers decide whether a `false`
    ///        result is fatal.
    template <typename Fn>
    static bool checked_syscall(Fn&& fn, const std::string& what, const std::string& status) noexcept;

    std::string                  obj_name_{""};             ///< Shared-memory object name (sans leading '/').
    static constexpr std::size_t shm_size_{sizeof(Buffer)}; ///< Size of the mapped segment.
    std::size_t                  registry_slot_{0};         ///< Slot index in shm_registry, valid only if is_creator_.
    std::byte*                   shm_start_{nullptr};       ///< Base address of the mmap'd segment.
    Buffer*                      buffer_{nullptr};          ///< Placement-new'd/reinterpreted Buffer at shm_start_.
    bool                         is_creator_{false};        ///< Whether this instance owns the segment's lifetime.
};
} // namespace ferrolink::ipc::shm_channel

#include <ferrolink/ipc/spsc_shm_channel.inl>
