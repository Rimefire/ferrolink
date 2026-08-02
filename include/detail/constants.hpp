#pragma once
#include "../ipc/spsc_buffer.hpp"

#include <cstddef>

namespace ferrolink::detail::constants {
static constexpr std::size_t SPSC_BUFFER_SIZE{sizeof(ipc::spsc::SPSCBuffer)};
} // namespace ferrolink::detail::constants
