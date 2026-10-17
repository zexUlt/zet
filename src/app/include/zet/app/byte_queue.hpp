#pragma once

#include <cstddef>
#include <vector>

#include "zet/core/bytes.hpp"

namespace zet::app {

/// Bytes waiting for a descriptor that took less than all of them, up to a
/// ceiling. Not a ring: the front is consumed from an offset and the storage
/// is compacted once the front has passed half of it, which keeps every
/// Front() one contiguous span for a single write(2).
class ByteQueue {
public:
    explicit ByteQueue(std::size_t limit) noexcept : Limit_(limit) {}

    /// Appends all of `bytes`, or nothing if that would pass the ceiling.
    [[nodiscard]] bool Append(ByteSpan bytes);

    /// What is waiting, in order.
    [[nodiscard]] ByteSpan Front() const noexcept {
        return ByteSpan{Bytes_}.subspan(Head_);
    }

    /// Drops the first `count` bytes of Front().
    void Consume(std::size_t count) noexcept;

    [[nodiscard]] std::size_t Size() const noexcept {
        return Bytes_.size() - Head_;
    }
    [[nodiscard]] bool Empty() const noexcept { return Size() == 0; }
    [[nodiscard]] std::size_t Room() const noexcept { return Limit_ - Size(); }

private:
    std::size_t Limit_;
    std::vector<std::byte> Bytes_;
    std::size_t Head_{0};
};

}  // namespace zet::app
