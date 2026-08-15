namespace ferrolink::ipc::transport {
template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
bool TransportProducer<ChannelType, MessageType>::try_send(const MessageType& message) noexcept {
    // Pure pass-through to the underlying channel's buffer — Transport adds no
    // buffering/queueing of its own, it's just a validated, typed handle.
    return channel_.buffer()->push(message);
}

template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
bool TransportConsumer<ChannelType, MessageType>::try_recv(MessageType& out) noexcept {
    // Mirror of try_send() above, pass-through to the buffer's pop().
    return channel_.buffer()->pop(out);
}

template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
auto Transport<ChannelType, MessageType>::create(std::string name) noexcept
    -> TransportProducer<ChannelType, MessageType> {
    // ChannelType::create() does the actual OS-level work (e.g. shm_open+mmap);
    // we just wrap the resulting channel in a producer handle.
    return TransportProducer<ChannelType, MessageType>{ChannelType::create(std::move(name))};
}

template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
auto Transport<ChannelType, MessageType>::attach(std::string name) noexcept
    -> TransportConsumer<ChannelType, MessageType> {
    // Same idea as create() above, but for the attaching (consumer) side.
    return TransportConsumer<ChannelType, MessageType>{ChannelType::attach(std::move(name))};
}
} // namespace ferrolink::ipc::transport

