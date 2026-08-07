namespace ferrolink::ipc::transport {
    template<typename ChannelType, typename MessageType>
        requires Channel<ChannelType, MessageType>
    inline bool TransportProducer<ChannelType, MessageType>::try_send(const MessageType& message) noexcept {
        return channel_.buffer()->push(msg);
    }
}

