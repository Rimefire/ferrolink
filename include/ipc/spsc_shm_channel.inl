// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

namespace ferrolink::ipc::shm_channel {
template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
SPSCShmChannel<T, Capacity> SPSCShmChannel<T, Capacity>::create(std::string obj_name) noexcept {
    // Install the crash-safety signal handlers on first use (idempotent) so this
    // segment gets shm_unlink'd even if we abort partway through setup below.
    detail::shm_registry::ensure_signal_handlers_registered();
    std::size_t registry_slot{detail::shm_registry::register_name(("/" + obj_name).c_str())};

    // O_CREAT | O_EXCL: fail loudly (EEXIST) rather than silently reusing/racing on
    // a stale segment left behind by a previous crashed run under the same name.
    int fd{open_shm(obj_name, O_CREAT | O_EXCL | O_RDWR)};

    // fd must be closed once mmap'd — the mapping itself keeps the object alive,
    // the descriptor is only needed to create the mapping.
    auto cleanup{[&fd]() { ::close(fd); }};

    if (::ftruncate(fd, shm_size_) != 0) {
        std::print(stderr, "[FATAL] Cannot truncate size of the object. ERRNO: {}\n", std::strerror(errno));
        cleanup();
        std::abort();
    }

    void* mmap_result{::mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)};
    // Grab errno before cleanup() can clobber it via ::close().
    int   saved_errno{errno};
    cleanup();

    if (mmap_result == MAP_FAILED) {
        std::print(stderr, "[FATAL] Cannot map shared object to memory. ERRNO: {}\n", std::strerror(saved_errno));
        std::abort();
    }

    std::byte* shm_start{static_cast<std::byte*>(mmap_result)};

    // madvise hints + mlock the pages resident before we touch them, so the very
    // first writes below (placement-new of Buffer) don't take a page fault.
    setup_memory(shm_start);

    // Placement-new: the segment is freshly ftruncate'd (zero-filled) raw memory,
    // so this actually runs Buffer's constructor (initializing the atomics) rather
    // than reinterpreting pre-existing bytes — unlike attach() below.
    Buffer* buffer{new (shm_start) Buffer{}};

    return SPSCShmChannel(shm_start, buffer, std::move(obj_name), registry_slot, 1);
}

template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
SPSCShmChannel<T, Capacity> SPSCShmChannel<T, Capacity>::attach(std::string obj_name) noexcept {
    // Also idempotent/harmless to call again here even though the creator side
    // already did — needed in case this process never called create() itself.
    detail::shm_registry::ensure_signal_handlers_registered();

    // No O_CREAT: this call fails if the creator hasn't run yet. Caller (e.g. the
    // benchmarks) is responsible for sequencing so create() has already happened.
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

    // reinterpret, NOT placement-new: the Buffer object already exists (constructed
    // by the creator's create() call) at this address in the shared segment — we
    // must not re-run its constructor and reset the atomics out from under the
    // producer that may already be using them.
    Buffer* buffer{reinterpret_cast<Buffer*>(shm_start)};

    // registry_slot=0, is_creator=0: this side never registers with shm_registry
    // and never unlinks — only the creator owns the segment's lifetime.
    return SPSCShmChannel(shm_start, buffer, std::move(obj_name), 0, 0);
}

template <typename T, std::size_t Capacity>
    requires spsc::TriviallyCopyable<T>
void SPSCShmChannel<T, Capacity>::setup_memory(std::byte* shm_start) noexcept {
    // Best-effort hints for the kernel's paging/readahead behavior on this mapping:
    //  - SEQUENTIAL: we scan the ring buffer roughly linearly cache-line by cache-line.
    //  - WILLNEED:   we're about to touch this memory immediately (avoid lazy fault-in).
    //  - HUGEPAGE:   opportunistically use transparent huge pages to cut TLB misses.
    // None of these are fatal if unsupported/denied — that's why they go through
    // checked_syscall with "WARNING" rather than aborting.
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

    // mlock, unlike the madvise hints, IS treated as fatal: if we can't pin these
    // pages resident, a page fault mid-benchmark would inject unpredictable latency
    // spikes into exactly the measurements this library exists to produce.
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
template <typename Fn>
bool SPSCShmChannel<T, Capacity>::checked_syscall(Fn&&               fn,
                                                  const std::string& what,
                                                  const std::string& status) noexcept {
    // Generic wrapper for the common "0 on success, check errno on failure" POSIX
    // convention — just logs and reports success/failure, leaves the fatal-or-not
    // decision to the caller (see setup_memory()'s two different usages above).
    if (fn() != 0) {
        std::print(stderr, "[{}] {}. ERRNO: {}", status, what, std::strerror(errno));
        return false;
    }
    return true;
}
} // namespace ferrolink::ipc::shm_channel

