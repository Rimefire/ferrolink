#pragma once
#include "../ipc/spsc_buffer.hpp"

#include <cstddef>

namespace ferrolink::detail::constants {
inline constexpr std::size_t SPSCBufferSize{sizeof(ipc::spsc::SPSCBuffer)};
} // namespace ferrolink::detail::constants
