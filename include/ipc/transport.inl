namespace ferrolink::ipc::transport {
template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
bool TransportProducer<ChannelType, MessageType>::try_send(const MessageType& message) noexcept {
    return channel_.buffer()->push(message);
}

template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
bool TransportConsumer<ChannelType, MessageType>::try_recv(MessageType& out) noexcept {
    return channel_.buffer()->pop(out);
}

template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
auto Transport<ChannelType, MessageType>::create(std::string name) noexcept
    -> TransportProducer<ChannelType, MessageType> {
    return TransportProducer<ChannelType, MessageType>{ChannelType::create(std::move(name))};
}

template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
auto Transport<ChannelType, MessageType>::attach(std::string name) noexcept -> TransportConsumer<ChannelType, MessageType> {
    return TransportConsumer<ChannelType, MessageType>{ChannelType::attach(std::move(name))};
}
} // namespace ferrolink::ipc::transport
