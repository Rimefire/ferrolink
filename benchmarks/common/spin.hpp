// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#pragma once

/// @file spin.hpp
/// @brief Portable CPU spin-wait hint for busy-wait loops.
namespace ferrolink::bench {

/// @brief Spin-wait hint so a busy loop doesn't hammer the memory bus pointlessly and gives
///        a sibling hyperthread/core a chance to run.
///
/// Portable across architectures: x86 uses the `PAUSE` instruction, ARM uses `YIELD`,
/// anything else falls back to a plain no-op spin. Intended to be called on every
/// iteration of a busy-wait loop (e.g. `while (!try_recv(out)) cpu_relax();`).
///
/// @note This is a hint, not a blocking call — it does not yield the core to the OS
///       scheduler, it only reduces power/bus pressure while spinning.
#if defined(__x86_64__) || defined(__i386__)
#    include <immintrin.h>
inline void cpu_relax() noexcept {
    _mm_pause();
}
#elif defined(__aarch64__) || defined(__arm__)
inline void cpu_relax() noexcept {
    asm volatile("yield" ::: "memory");
}
#else
/// @brief No-op fallback for architectures without a dedicated spin-wait instruction.
inline void cpu_relax() noexcept {}
#endif

} // namespace ferrolink::bench
