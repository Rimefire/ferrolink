// Copyright (c) 2026-present Rimefire. All rights reserved.
// Proprietary and confidential — unauthorized copying, distribution, or use
// of this file, via any medium, is strictly prohibited. See LICENSE at the
// repository root.

#pragma once

#include <cerrno>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <type_traits>
#include <utility>

/// @file transport.hpp
/// @brief Channel-agnostic producer/consumer wrapper on top of any type satisfying
///        the Channel concept (e.g. shm_channel::SPSCShmChannel).
namespace ferrolink::ipc::transport {

/// @brief Constrains a buffer type to be usable by TransportProducer/TransportConsumer:
///        it must support `push(const Message&) -> bool` and `pop(Message&) -> bool`.
///
/// Satisfied by spsc::SPSCBuffer for any TriviallyCopyable @p Message.
template <typename Buff, typename Message>
concept Buffer = requires(Buff buffer, const Message& msg, Message& out) {
    { buffer.push(msg) } -> std::same_as<bool>;
    { buffer.pop(out) } -> std::same_as<bool>;
};

/// @brief Constrains a channel type to be usable by Transport: it must be
///        constructible via static `create`/`attach` factory functions returning
///        `T`, expose `buffer()`/`is_valid()`/`role()` accessors, and its `Buffer`
///        member type must itself satisfy the Buffer concept for @p Message.
///
/// Satisfied by shm_channel::SPSCShmChannel.
template <typename T, typename Message>
concept Channel = std::is_class_v<T> && requires {
    { T::create(std::string{}) } -> std::same_as<T>;
    { T::attach(std::string{}) } -> std::same_as<T>;
} && requires(T ch) {
    { ch.buffer() } -> std::convertible_to<typename T::Buffer*>;
    { ch.is_valid() } -> std::same_as<bool>;
    { ch.role() } -> std::same_as<bool>;
} && Buffer<typename T::Buffer, Message>;

/// @brief Send-only handle over a Channel, constructed via Transport::create().
///
/// Thin wrapper that validates the underlying channel once at construction and then
/// forwards try_send() calls directly to the channel's buffer. Non-copyable and
/// non-movable — a producer is meant to live for the duration of the sending
/// thread/process and not be relocated.
///
/// @tparam ChannelType Concrete channel type, must satisfy Channel<ChannelType, MessageType>.
/// @tparam MessageType Message element type sent through the channel.
template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
class TransportProducer {
  public:
    /// @brief Attempts to enqueue @p message without blocking.
    /// @param message Message to send.
    /// @return `true` if enqueued, `false` if the underlying buffer is full (caller
    ///         is expected to retry/spin, e.g. with bench::cpu_relax()).
    [[nodiscard, gnu::hot, gnu::always_inline]] bool try_send(const MessageType& message) noexcept;

    /// @name Non-copyable, non-movable
    ///@{
    TransportProducer(const TransportProducer&)            = delete;
    TransportProducer& operator=(const TransportProducer&) = delete;

    TransportProducer(TransportProducer&&)            = delete;
    TransportProducer& operator=(TransportProducer&&) = delete;
    ///@}

  private:
    /// @brief Constructs from an already-created/attached, valid @p channel.
    ///
    /// @param channel Channel to take ownership of (moved-from).
    /// @note Aborts the process if `channel.is_valid()` is `false` — a producer over
    ///       an invalid channel is a programming error, not a runtime condition to
    ///       recover from.
    explicit TransportProducer(ChannelType&& channel) noexcept : channel_{std::move(channel)} {
        if (!channel_.is_valid()) {
            std::print(stderr, "[FATAL] Producer channel is not valid.\n");
            std::abort();
        }
    }

    template <typename C, typename M>
        requires Channel<C, M>
    friend class Transport;

    ChannelType channel_; ///< Owned channel this producer sends into.
};

/// @brief Receive-only handle over a Channel, constructed via Transport::attach().
///
/// Thin wrapper that validates the underlying channel once at construction and then
/// forwards try_recv() calls directly to the channel's buffer. Non-copyable and
/// non-movable — a consumer is meant to live for the duration of the receiving
/// thread/process and not be relocated.
///
/// @tparam ChannelType Concrete channel type, must satisfy Channel<ChannelType, MessageType>.
/// @tparam MessageType Message element type received from the channel.
template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
class TransportConsumer {
  public:
    /// @brief Attempts to dequeue the next message without blocking.
    /// @param[out] out Destination for the received message. Left unmodified if
    ///                 nothing is available.
    /// @return `true` if a message was received, `false` if the underlying buffer
    ///         is empty (caller is expected to retry/spin, e.g. with
    ///         bench::cpu_relax()).
    [[nodiscard, gnu::hot, gnu::always_inline]] bool try_recv(MessageType& out) noexcept;

    /// @name Non-copyable, non-movable
    ///@{
    TransportConsumer(const TransportConsumer&)            = delete;
    TransportConsumer& operator=(const TransportConsumer&) = delete;

    TransportConsumer(TransportConsumer&&)            = delete;
    TransportConsumer& operator=(TransportConsumer&&) = delete;
    ///@}

  private:
    /// @brief Constructs from an already-created/attached, valid @p channel.
    ///
    /// @param channel Channel to take ownership of (moved-from).
    /// @note Aborts the process if `channel.is_valid()` is `false` — a consumer over
    ///       an invalid channel is a programming error, not a runtime condition to
    ///       recover from.
    explicit TransportConsumer(ChannelType&& channel) noexcept : channel_{std::move(channel)} {
        if (!channel_.is_valid()) {
            std::print(stderr, "[FATAL] Consumer channel is not valid.\n");
            std::abort();
        }
    }

    template <typename C, typename M>
        requires Channel<C, M>
    friend class Transport;

    ChannelType channel_; ///< Owned channel this consumer receives from.
};

/// @brief Static factory for creating matched TransportProducer/TransportConsumer
///        pairs over a given Channel type.
///
/// Never instantiated — purely a namespace-like holder for create()/attach().
///
/// @tparam ChannelType Concrete channel type, must satisfy Channel<ChannelType, MessageType>.
/// @tparam MessageType Message element type carried by the channel.
template <typename ChannelType, typename MessageType>
    requires Channel<ChannelType, MessageType>
class Transport {
  public:
    /// @brief Creates a new underlying channel (via `ChannelType::create`) and wraps
    ///        it in a TransportProducer.
    ///
    /// Must be called before the peer's attach() with the same @p name — see
    /// ChannelType::create()'s documentation for ordering requirements.
    ///
    /// @param name Channel/object name, forwarded to `ChannelType::create`.
    /// @return A TransportProducer owning the newly-created channel.
    [[nodiscard, gnu::cold]] static auto create(std::string name) noexcept
        -> TransportProducer<ChannelType, MessageType>;

    /// @brief Attaches to an existing underlying channel (via `ChannelType::attach`)
    ///        and wraps it in a TransportConsumer.
    ///
    /// @param name Channel/object name, matching a prior create() call.
    /// @return A TransportConsumer owning the attached channel.
    [[nodiscard, gnu::cold]] static auto attach(std::string name) noexcept
        -> TransportConsumer<ChannelType, MessageType>;

    Transport() = delete;

    /// @name Non-copyable, non-movable
    ///@{
    Transport(const Transport&)            = delete;
    Transport& operator=(const Transport&) = delete;

    Transport(Transport&&)            = delete;
    Transport& operator=(Transport&&) = delete;
    ///@}
};

} // namespace ferrolink::ipc::transport

#include <ferrolink/ipc/transport.inl>
