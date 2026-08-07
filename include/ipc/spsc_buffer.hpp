#pragma once
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <detail/cache_line.hpp>
#include <type_traits>
#include <utility>

namespace ferrolink::ipc::spsc {
template <typename T>
concept TriviallyCopyable = std::is_trivially_copyable_v<T>;

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
class SPSCBuffer {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be greater than zero and must be a power of two.");

  public:
    SPSCBuffer() noexcept  = default;
    ~SPSCBuffer() noexcept = default;

    SPSCBuffer(const SPSCBuffer&)            = delete;
    SPSCBuffer& operator=(const SPSCBuffer&) = delete;

    SPSCBuffer(SPSCBuffer&& other)            = delete;
    SPSCBuffer& operator=(SPSCBuffer&& other) = delete;

    bool push(const T& value) noexcept;

    bool pop(T& out) noexcept;

    bool empty() const noexcept;

    bool full() const noexcept;

    std::size_t filled_size() const noexcept;

    static constexpr std::size_t capacity() noexcept;

  private:
    alignas(detail::cacheline::DestructiveSize) std::atomic<std::size_t> read_index_{0};
    alignas(detail::cacheline::DestructiveSize) std::atomic<std::size_t> write_index_{0};
    alignas(detail::cacheline::DestructiveSize) std::array<T, Capacity> slots_{};

    static constexpr std::size_t mask_{Capacity - 1};
};

} // namespace ferrolink::ipc::spsc

#include "spsc_buffer.inl"
