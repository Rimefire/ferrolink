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
    auto Transport<ChannelType, MessageType>::create(std::string name) -> TransportProducer<ChannelType, MessageType> {
        ChannelType channel{ChannelType::create(std::move(name))};

        if (!channel.is_valid()) {
            std::print(stderr, "[FATAL] Producer channel is not valid after creation.\n");
            std::abort();
        }

        return TransportProducer<ChannelType, MessageType>{std::move(channel)};
    }

    template <typename ChannelType, typename MessageType>
        requires Channel<ChannelType, MessageType>
    auto Transport<ChannelType, MessageType>::attach(std::string name) -> TransportConsumer<ChannelType, MessageType> {
        ChannelType channel{ChannelType::attach(std::move(name))};

        if (!channel.is_valid()) {
            std::print(stderr, "[FATAL] Consumer channel is not valid after creation.\n");
            std::abort();
        }
        return TransportConsumer<ChannelType, MessageType>{ChannelType::attach(std::move(name))};
    }
}

