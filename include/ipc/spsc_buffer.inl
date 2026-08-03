template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
constexpr std::size_t SPSCBuffer<T, Capacity>::capacity() noexcept {
        return Capacity;
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
bool SPSCBuffer<T, Capacity>::full() const noexcept {
    return (write_index_.load(std::memory_order_relaxed) - read_index_.load(std::memory_order_acquire)) == Capacity;
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
bool SPSCBuffer<T, Capacity>::empty() const noexcept {
    return write_index_.load(std::memory_order_acquire) == read_index_.load(std::memory_order_relaxed);
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
std::size_t SPSCBuffer<T, Capacity>::filled_size() const noexcept {
    return write_index_.load(std::memory_order_acquire) - read_index_.load(std::memory_order_acquire);
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
bool SPSCBuffer<T, Capacity>::push(T value) noexcept {
    const auto curr_write_idx{write_index_.load(std::memory_order_relaxed)};
    const auto curr_read_idx{read_index_.load(std::memory_order_acquire)}

    if (curr_write_idx - curr_read_idx == Capacity) return false;

    slots_[curr_write_idx & mask_] = value;
    
    write_index_.store(curr_write_idx + 1, std::memory_order_release);

    return true;
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
bool SPSCBuffer<T, Capacity>::pop(T& out) noexcept {
    const auto curr_read_idx{read_index_.load(std::memory_order_relaxed)};
    const auto curr_write_idx{write_index_.load(std::memory_order_acquire)};

    if (curr_write_idx == curr_read_idx) return false;

    out = slots_[curr_read_idx & mask_];

    read_index_.store(curr_read_idx + 1, std::memory_order_release);
    return true;
}
