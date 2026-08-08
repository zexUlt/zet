#pragma once

#include <cstddef>
#include <cstdint>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/wire/limits.hpp"

namespace zet::wire {

/// The cleartext part of a frame. Both fields are covered by the AD, so
/// tampering with either fails the tag instead of steering the parse.
struct FrameHeader {
    /// Body length: ciphertext together with the tag.
    std::uint32_t Length{0};
    /// Key generation. Grows on rekey.
    std::uint8_t Epoch{0};
};

/// A frame found in a buffer. The body is a view over the input, never a copy.
struct FrameView {
    FrameHeader Header;
    ByteSpan Body;
    /// How many bytes of the buffer the whole frame took, header included.
    std::size_t Consumed{0};
};

[[nodiscard]] ProtoResult<FrameHeader> ReadFrameHeader(
    ByteReader& reader) noexcept;

[[nodiscard]] ProtoResult<void> WriteFrameHeader(ByteWriter& writer,
                                                 FrameHeader header) noexcept;

/// Extracts one frame from the front of the buffer.
///
/// `maxBodyLength` is the limit for the stage: MAX_HANDSHAKE_FRAME before
/// authentication, MAX_FRAME after. The declared length is checked against it
/// before anyone decides to allocate for it.
///
/// `Truncated` means "read more and call again", not damage.
[[nodiscard]] ProtoResult<FrameView> ReadFrame(
    ByteSpan buffer, std::uint32_t maxBodyLength) noexcept;

}  // namespace zet::wire
