#pragma once

namespace ferrolink::bench {

// Spin-wait hint so a busy loop doesn't hammer the memory bus pointlessly and gives
// a sibling hyperthread/core a chance to run. Portable: x86 uses the PAUSE instruction,
// ARM uses YIELD, anything else falls back to a plain no-op spin.
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
inline void cpu_relax() noexcept {}
#endif

} // namespace ferrolink::bench
