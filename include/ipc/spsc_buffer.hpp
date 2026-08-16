// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#pragma once
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <detail/cache_line.hpp>
#include <type_traits>
#include <utility>

/// @file spsc_buffer.hpp
/// @brief Lock-free single-producer/single-consumer ring buffer.
namespace ferrolink::ipc::spsc {

/// @brief Constrains @p T to types safe to store and copy in/out of the ring buffer
///        without invoking constructors/destructors (required since the buffer may
///        live in shared memory shared between processes with independent address
///        spaces / lifetimes).
template <typename T>
concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

/// @brief Fixed-capacity, lock-free ring buffer for exactly one producer thread and
///        one consumer thread.
///
/// Safe for concurrent use by **exactly one** producer calling push() and **exactly
/// one** consumer calling pop() concurrently — it is not safe for multiple producers
/// or multiple consumers. Correctness relies on the classic SPSC pattern: each side
/// owns one index (the producer owns `write_index_`, the consumer owns
/// `read_index_`), reads its own index with `relaxed` ordering, reads the other
/// side's index with `acquire`, and publishes its own index with `release`.
///
/// `read_index_`, `write_index_`, and `slots_` are each aligned to
/// detail::cacheline::DestructiveSize and placed in separate cache lines to avoid
/// false sharing between the producer and consumer.
///
/// Trivially relocatable in memory: contains no pointers to itself, so it can be
/// placed directly in shared memory (see shm_channel::SPSCShmChannel) and used
/// across process boundaries, not just threads within one process.
///
/// @tparam T        Element type. Must satisfy TriviallyCopyable.
/// @tparam Capacity Number of slots. Must be a power of two greater than zero.
template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
class SPSCBuffer {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be greater than zero and must be a power of two.");

  public:
    /// @brief Constructs an empty buffer with both indices at zero.
    [[gnu::cold]] SPSCBuffer() noexcept  = default;
    [[gnu::cold]] ~SPSCBuffer() noexcept = default;

    /// @name Non-copyable, non-movable
    /// @brief The buffer's address is meaningful (it may be placed in shared memory
    ///        and referenced by another process), so copying or moving it is disallowed.
    ///@{
    SPSCBuffer(const SPSCBuffer&)            = delete;
    SPSCBuffer& operator=(const SPSCBuffer&) = delete;

    SPSCBuffer(SPSCBuffer&& other)            = delete;
    SPSCBuffer& operator=(SPSCBuffer&& other) = delete;
    ///@}

    /// @brief Attempts to enqueue @p value. Must only be called by the single producer.
    ///
    /// @param value Element to copy into the buffer.
    /// @return `true` if the value was enqueued, `false` if the buffer is full().
    [[nodiscard, gnu::hot, gnu::always_inline]] bool push(const T& value) noexcept;

    /// @brief Attempts to dequeue the oldest element into @p out. Must only be called
    ///        by the single consumer.
    ///
    /// @param[out] out Destination for the dequeued element. Left unmodified if the
    ///                 buffer is empty().
    /// @return `true` if an element was dequeued, `false` if the buffer is empty().
    [[nodiscard, gnu::hot, gnu::always_inline]] bool pop(T& out) noexcept;

    /// @brief Checks whether the buffer currently holds no elements.
    ///
    /// @return `true` if empty. Safe to call from either producer or consumer, but
    ///         the result may be immediately stale due to concurrent activity on the
    ///         other side.
    [[nodiscard, gnu::cold]] bool empty() const noexcept;

    /// @brief Checks whether the buffer currently holds Capacity elements.
    ///
    /// @return `true` if full. Safe to call from either producer or consumer, but
    ///         the result may be immediately stale due to concurrent activity on the
    ///         other side.
    [[nodiscard, gnu::cold]] bool full() const noexcept;

    /// @brief Returns a snapshot of how many elements are currently in the buffer.
    ///
    /// @return Number of elements, in `[0, Capacity]`. May be immediately stale due
    ///         to concurrent push()/pop() calls.
    [[nodiscard, gnu::cold]] std::size_t filled_size() const noexcept;

    /// @brief Returns the compile-time capacity of the buffer.
    /// @return Capacity, as given by the class template parameter.
    [[nodiscard, gnu::cold, gnu::const]] static constexpr std::size_t capacity() noexcept;

  private:
    /// @brief Consumer-owned read cursor (monotonically increasing, wraps via mask_).
    alignas(detail::cacheline::DestructiveSize) std::atomic<std::size_t> read_index_{0};
    /// @brief Producer-owned write cursor (monotonically increasing, wraps via mask_).
    alignas(detail::cacheline::DestructiveSize) std::atomic<std::size_t> write_index_{0};
    /// @brief Backing storage for the ring buffer's elements.
    alignas(detail::cacheline::DestructiveSize) std::array<T, Capacity> slots_{};

    /// @brief Bitmask (`Capacity - 1`) used to wrap monotonic indices into `[0, Capacity)`.
    static constexpr std::size_t mask_{Capacity - 1};
};

} // namespace ferrolink::ipc::spsc

#include "spsc_buffer.inl"
