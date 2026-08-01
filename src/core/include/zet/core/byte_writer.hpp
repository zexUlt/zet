#pragma once

#include <cstddef>
#include <cstdint>

#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"

namespace zet {

/// Encodes into a buffer the caller owns.
///
/// There is no growing here on purpose. Frames are bounded by MAX_FRAME, so the
/// size is known before encoding starts, and a fixed buffer keeps the protocol
/// core free of allocation — which in turn keeps it free of the one exception
/// (bad_alloc) that would otherwise escape it.
///
/// A failed write leaves the buffer and the position untouched: a caller that
/// runs out of room gets `BufferTooSmall` and a writer it can still inspect,
/// not a half-written frame.
class ByteWriter {
public:
    explicit constexpr ByteWriter(MutableByteSpan out) noexcept : out_(out) {}

    [[nodiscard]] ProtoResult<void> WriteU8(std::uint8_t value) noexcept;
    [[nodiscard]] ProtoResult<void> WriteU16BE(std::uint16_t value) noexcept;
    [[nodiscard]] ProtoResult<void> WriteU32BE(std::uint32_t value) noexcept;
    [[nodiscard]] ProtoResult<void> WriteU64BE(std::uint64_t value) noexcept;
    [[nodiscard]] ProtoResult<void> WriteBytes(ByteSpan bytes) noexcept;

    /// Reserves `count` bytes and hands them back to be filled in place, for
    /// payloads written by something else (an AEAD seal, say).
    [[nodiscard]] ProtoResult<MutableByteSpan> Reserve(
        std::size_t count) noexcept;

    /// What has been written so far.
    [[nodiscard]] constexpr ByteSpan Written() const noexcept {
        return ByteSpan{out_.subspan(0, pos_)};
    }

    [[nodiscard]] constexpr std::size_t Size() const noexcept { return pos_; }
    [[nodiscard]] constexpr std::size_t Capacity() const noexcept {
        return out_.size();
    }
    [[nodiscard]] constexpr std::size_t Remaining() const noexcept {
        return out_.size() - pos_;
    }

private:
    MutableByteSpan out_;
    std::size_t pos_{0};
};

}  // namespace zet
