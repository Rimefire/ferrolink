#pragma once

#include <concepts>
#include <string>
#include <type_traits>

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
