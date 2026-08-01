#pragma once

#include <cstddef>
#include <cstdint>

#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"

namespace zet {

/// The only way to take protocol bytes apart.
///
/// Every read is bounds-checked and reports failure through the return value,
/// so a parser cannot index past the end however carelessly it is written.
/// Direct indexing of wire buffers is banned elsewhere in the protocol core
/// precisely so that this stays the one path.
///
/// A failed read leaves the position untouched. Callers may therefore try a
/// read, fail, and hand the same reader to a different decoder without having
/// to unwind a partial consume.
class ByteReader {
public:
    explicit constexpr ByteReader(ByteSpan data) noexcept : data_(data) {}

    [[nodiscard]] ProtoResult<std::uint8_t> ReadU8() noexcept;
    [[nodiscard]] ProtoResult<std::uint16_t> ReadU16BE() noexcept;
    [[nodiscard]] ProtoResult<std::uint32_t> ReadU32BE() noexcept;
    [[nodiscard]] ProtoResult<std::uint64_t> ReadU64BE() noexcept;

    /// Borrows `count` bytes. The span points into the buffer this reader was
    /// built from and does not outlive it.
    [[nodiscard]] ProtoResult<ByteSpan> ReadBytes(std::size_t count) noexcept;

    [[nodiscard]] ProtoResult<void> Skip(std::size_t count) noexcept;

    /// Everything not yet consumed, without consuming it.
    [[nodiscard]] constexpr ByteSpan Rest() const noexcept {
        return data_.subspan(pos_);
    }

    [[nodiscard]] constexpr std::size_t Offset() const noexcept { return pos_; }
    [[nodiscard]] constexpr std::size_t Remaining() const noexcept {
        return data_.size() - pos_;
    }
    [[nodiscard]] constexpr bool Exhausted() const noexcept {
        return pos_ == data_.size();
    }

private:
    ByteSpan data_;
    std::size_t pos_{0};
};

}  // namespace zet
