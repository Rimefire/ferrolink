// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

namespace ferrolink::ipc::spsc {

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
constexpr std::size_t SPSCBuffer<T, Capacity>::capacity() noexcept {
    // Just echoes the template parameter back — kept as a method so callers don't
    // need to know the class is a template to query capacity.
    return Capacity;
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
bool SPSCBuffer<T, Capacity>::full() const noexcept {
    // write - read == Capacity means every slot is occupied.
    // write_index_ read with relaxed: we own this side (only meaningful if called
    // from the producer thread). read_index_ read with acquire: syncs with the
    // consumer's release store in pop(), so we see an up-to-date read cursor.
    return (write_index_.load(std::memory_order_relaxed) - read_index_.load(std::memory_order_acquire)) == Capacity;
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
bool SPSCBuffer<T, Capacity>::empty() const noexcept {
    // Mirror of full(): here we're on the consumer side, so read_index_ is "ours"
    // (relaxed) and write_index_ is the producer's, read with acquire.
    return write_index_.load(std::memory_order_acquire) == read_index_.load(std::memory_order_relaxed);
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
std::size_t SPSCBuffer<T, Capacity>::filled_size() const noexcept {
    // Callable from either side, so both indices are read with acquire here (no
    // "own index" to treat as relaxed) — this is just a snapshot query, not on the
    // hot push()/pop() path.
    return write_index_.load(std::memory_order_acquire) - read_index_.load(std::memory_order_acquire);
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
bool SPSCBuffer<T, Capacity>::push(const T& value) noexcept {
    // Producer-only. write_index_ is ours: relaxed load is fine, nobody else writes it.
    const auto curr_write_idx{write_index_.load(std::memory_order_relaxed)};
    // read_index_ belongs to the consumer: acquire so we observe its latest release
    // (from pop()) and don't overwrite a slot the consumer hasn't finished reading yet.
    const auto curr_read_idx{read_index_.load(std::memory_order_acquire)};

    if (curr_write_idx - curr_read_idx == Capacity)
        return false; // ring is full

    // Indices are monotonically increasing and never reset — mask_ maps them into
    // the physical slot range. This means write_index_/read_index_ can in principle
    // wrap around std::size_t after ~2^64 pushes, which is not a practical concern.
    slots_[curr_write_idx & mask_] = value;

    // release publishes both the index bump *and* the slot write above it (program
    // order + release semantics), so the consumer's acquire load of write_index_
    // is guaranteed to see the slot data too.
    write_index_.store(curr_write_idx + 1, std::memory_order_release);

    return true;
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
bool SPSCBuffer<T, Capacity>::pop(T& out) noexcept {
    // Consumer-only, mirror of push(): read_index_ is ours (relaxed), write_index_
    // is the producer's (acquire, to see the latest pushed slot).
    const auto curr_read_idx{read_index_.load(std::memory_order_relaxed)};
    const auto curr_write_idx{write_index_.load(std::memory_order_acquire)};

    if (curr_write_idx == curr_read_idx)
        return false; // ring is empty

    out = slots_[curr_read_idx & mask_];

    // release here syncs with push()'s acquire load of read_index_, telling the
    // producer this slot is now free to be overwritten again.
    read_index_.store(curr_read_idx + 1, std::memory_order_release);
    return true;
}
} // namespace ferrolink::ipc::spsc

