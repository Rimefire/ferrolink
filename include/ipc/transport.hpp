#pragma once

#include <concepts>
#include <string>
#include <type_traits>

namespace ferrolink::ipc::transport {
template <typename Buff, typename Message>
concept Buffer = requires(Buff buffer, const Message& msg, Message& out) {
    { buffer.push(msg) } -> std::same_as<bool>;
    { buffer.pop(out) } -> std::same_as<bool>;
};

template <typename T, typename Message>
concept Channel = std::is_class_v<T> && requires {
    { T::create(std::string{}) } -> std::same_as<T>;
    { T::attach(std::string{}) } -> std::same_as<T>;
} && requires(T ch) {
    { ch.buffer() } -> std::convertible_to<typename T::Buffer*>;
    { ch.is_valid() } -> std::same_as<bool>;
    { ch.role() } -> std::same_as<bool>;
} && Buffer<typename T::Buffer, Message>;

template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
class TransportProducer {
  public:
    bool try_send(const MessageType& message) noexcept;

    TransportProducer(const TransportProducer&)            = delete;
    TransportProducer& operator=(const TransportProducer&) = delete;

    TransportProducer(TransportProducer&&)          = delete;
    TransportProducer& operator=(TransportProducer) = delete;

  private:
    explicit TransportProducer(ChannelType&& channel) noexcept : channel_{std::move(channel)} {}

    template <typename C, typename M>
        requires Channel<C, M>
    friend class Transport;

    ChannelType channel_;
};

template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
class TransportConsumer {
  public:
    bool try_recv(MessageType& out) noexcept;

    TransportConsumer(const TransportConsumer&)            = delete;
    TransportConsumer& operator=(const TransportConsumer&) = delete;

    TransportConsumer(TransportConsumer&&)            = delete;
    TransportConsumer& operator=(TransportConsumer&&) = delete;

  private:
    explicit TransportConsumer(ChannelType&& channel) noexcept : channel_{std::move(channel)} {}

    template <typename C, typename M>
        requires Channel<C, M>
    friend class Transport;

    ChannelType channel_;
};

template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
class Transport {
  public:
    static auto create(std::string name) noexcept -> TransportProducer<ChannelType, MessageType>;
    static auto attach(std::string name) noexcept -> TransportConsumer<ChannelType, MessageType>;

    Transport() = delete;

    Transport(const Transport&)            = delete;
    Transport& operator=(const Transport&) = delete;

    Transport(Transport&&)            = delete;
    Transport& operator=(Transport&&) = delete;
};

} // namespace ferrolink::ipc::transport

#include <ipc/transport.hpp>
