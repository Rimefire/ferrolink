template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
static constexpr std::size_t SPSCBuffer<T, Capacity>::capacity() noexcept {
        return Capacity;
}

template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
bool SPSCBuffer<T, Capacity>::full() const noexcept {
    return (write_index_.load(std::memory_order_relaxed) - read_index_.load(std::memory_order_acquire)) == Capacity;
}
