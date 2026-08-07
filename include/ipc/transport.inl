namespace ferrolink::ipc::transport {
    template <typename ChannelType, typename MessageType>
        requires Channel<ChannelType, MessageType>
    bool TransportProducer<ChannelType, MessageType>::try_send(const MessageType& message) noexcept {
        return channel_.buffer()->push(msg);
    }

    template <typename ChannelType, typename MessageType>
        requires Channel<ChannelType, MessageType>
    bool TransportConsumer<ChannelType, MessageType>::try_recv(MessageType& out) noexcept {
        return channel_.buffer()->pop(out);
    }
}

