#pragma once
#include <cstddef>
#include <new>

namespace ferrolink::detail::cacheline {
inline constexpr std::size_t DestructiveSize{64};
inline constexpr std::size_t ConstructiveSize{64};
} // namespace ferrolink::detail::cacheline
