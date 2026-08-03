template <typename T, std::size_t Capacity>
    requires TriviallyCopyable<T>
static constexpr std::size_t SPSCBuffer<T, Capacity>::capacity() noexcept {
        return Capacity;
    }
