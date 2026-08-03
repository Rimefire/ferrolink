#pragma once
#include <cstddef>
#include <new>

namespace ferrolink::detail::cacheline {
#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t DestructiveSize{std::hardware_destructive_interference_size};
inline constexpr std::size_t ConstructiveSize{std::hardware_constructive_interference_size};
#else
inline constexpr std::size_t DestructiveSize{64};
inline constexpr std::size_t ConstructiveSize{64};
#endif

} // namespace ferrolink::detail::cacheline
