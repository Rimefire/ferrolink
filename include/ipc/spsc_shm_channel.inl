namespace ferrolink::ipc::shm_channel {
template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
SPSCShmChannel<T, Capacity> SPSCShmChannel<T, Capacity>::create(std::string obj_name) noexcept {
    std::strncpy(shm_name_, ("/" + obj_name).c_str(), sizeof(shm_name_) - 1);

    struct sigaction sig_act {};
    register_signal_handlers(sig_act);

    int fd{open_shm(obj_name, O_CREAT | O_EXCL | O_RDWR)};

    auto cleanup{[&fd]() { ::close(fd); }};

    if (::ftruncate(fd, shm_size_) != 0) {
        std::print(stderr, "[FATAL] Cannot truncate size of the object. ERRNO: {}\n", std::strerror(errno));
        cleanup();
        std::abort();
    }

    void* mmap_result{::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)};
    int   saved_errno{errno};
    cleanup();

    if (mmap_result == MAP_FAILED) {
        std::print(stderr, "[FATAL] Cannot map shared object to memory. ERRNO: {}\n", std::strerror(saved_errno));
        std::abort();
    }

    std::byte* shm_start{static_cast<std::byte*>(mmap_result)};

    setup_memory(shm_start);

    Buffer* buffer{new (shm_start) Buffer{}};

    return SPSCShmChannel(shm_start, buffer, std::move(obj_name), 1);
}

template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
SPSCShmChannel<T, Capacity> SPSCShmChannel<T, Capacity>::attach(std::string obj_name) noexcept {
    int fd{open_shm(obj_name, O_RDWR)};

    auto cleanup{[&fd]() { ::close(fd); }};

    void* mmap_result{::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)};

    int saved_errno{errno};
    cleanup();

    if (mmap_result == MAP_FAILED) {
        std::print(stderr, "[FATAL] Cannot map shared object to memory. ERRNO: {}\n", std::strerror(saved_errno));
        std::abort();
    }

    std::byte* shm_start{static_cast<std::byte*>(mmap_result)};

    setup_memory(shm_start);

    Buffer* buffer{reinterpret_cast<Buffer*>(shm_start)};

    return SPSCShmChannel(shm_start, buffer, std::move(obj_name), 0);
}

template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
void SPSCShmChannel<T, Capacity>::setup_memory(std::byte* shm_start) noexcept {
    static constexpr std::pair<int, const char*> flags[]{
        {MADV_SEQUENTIAL, "MADV_SEQUENTIAL"},
        {MADV_WILLNEED, "MADV_WILLNEED"},
        {MADV_HUGEPAGE, "MADV_HUGEPAGE"},
    };

    for (const auto& [flagnum, name] : flags) {
        checked_syscall([&] { return ::madvise(shm_start, shm_size_, flagnum); },
                        std::format("madvise with {} in SPSCShmChannel::setup_memory function failed", name),
                        "WARNING");
    }

    if (!checked_syscall(
            [&] { return ::mlock(shm_start, shm_size_); }, "Cannot lock virtual address space into RAM", "FATAL")) {
        std::abort();
    }
}

template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
int SPSCShmChannel<T, Capacity>::open_shm(const std::string& name, int flags) noexcept {
    int fd{::shm_open(("/" + name).c_str(), flags, 0660)};

    if (fd == -1) {
        std::print(
            stderr, "[FATAL] Cannot get file descriptor for shared memory block. ERRNO: {}\n", std::strerror(errno));
        std::abort();
    }

    return fd;
}

template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
void SPSCShmChannel<T, Capacity>::sig_handler([[maybe_unused]] int sig) noexcept {
    if (shm_name_[0] != '\0') {
        ::shm_unlink(shm_name_);
    }
}

template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
void SPSCShmChannel<T, Capacity>::register_signal_handlers(struct sigaction& sa) noexcept {
    sa.sa_handler = sig_handler;
    sa.sa_flags   = SA_RESETHAND;

    if (!checked_syscall([&] { return ::sigemptyset(&sa.sa_mask); }, "Failed to initialize signal mask", "FATAL")) {
        std::abort();
    }

    static constexpr int signals[] = {SIGABRT, SIGTERM, SIGSEGV, SIGINT};
    for (int signum : signals) {
        if (!checked_syscall([&] { return ::sigaction(signum, &sa, nullptr); },
                             std::format("Failed to register signal handler for signal {}", ::strsignal(signum)),
                             "FATAL")) {
            std::abort();
        }
    }
}

template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
template <typename Fn>
bool SPSCShmChannel<T, Capacity>::checked_syscall(Fn&&               fn,
                                                  const std::string& what,
                                                  const std::string& status) noexcept {
    if (fn() != 0) {
        std::print(stderr, "[{}] {}. ERRNO: {}", status, what, std::strerror(errno));
        return false;
    }
    return true;
}
} // namespace ferrolink::ipc::shm_channel
