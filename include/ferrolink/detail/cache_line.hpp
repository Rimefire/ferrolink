// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#pragma once
#include <cstddef>
/// @file cache_line.hpp
/// @brief Portable cache-line size constants used to align hot data and avoid false sharing.
///
/// libstdc++/libc++ do not yet ship `std::hardware_destructive_interference_size` /
/// `std::hardware_constructive_interference_size` (P0154) consistently across
/// toolchains, so we define our own fixed constants instead of relying on `<new>`.
namespace ferrolink::detail::cacheline {

/// @brief Minimum recommended offset between two objects to avoid false sharing
///        (i.e. the two objects should not end up on the same cache line).
///
/// Used to `alignas()` independently-written fields (e.g. producer/consumer
/// indices in an SPSC ring buffer) so that a write to one does not invalidate
/// the cache line backing the other.
inline constexpr std::size_t DestructiveSize{64};

/// @brief Maximum recommended size for two objects to share a cache line and
///        benefit from prefetching together (constructive interference).
///
/// Used when objects are read together and we *want* them on the same line.
inline constexpr std::size_t ConstructiveSize{64};

} // namespace ferrolink::detail::cacheline
