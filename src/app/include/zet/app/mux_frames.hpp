#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "zet/core/byte_writer.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

/// Frames on mux.sock, both ends (design.md §6): cleartext frames of the
/// usual format carrying the local messages 0x20…0x23.
namespace zet::app {

/// The longest body on mux.sock: a MuxHandoff with the longest prefix.
inline constexpr std::size_t MUX_MAX_BODY =
    sizeof(std::uint8_t) + sizeof(std::uint16_t) + wire::MAX_HANDOFF_PREFIX;

/// The longest frame, header included.
inline constexpr std::size_t MUX_MAX_FRAME = wire::HEADER_SIZE + MUX_MAX_BODY;

/// What `write` puts in a body, as one cleartext frame. Empty if the message
/// does not encode.
template <typename TWrite>
[[nodiscard]] std::vector<std::byte> CleartextFrame(TWrite&& write) {
    std::vector<std::byte> frame(MUX_MAX_FRAME);
    ByteWriter body{MutableByteSpan{frame}.subspan(wire::HEADER_SIZE)};
    if (!write(body)) {
        return {};
    }
    const std::size_t length = body.Written().size();
    ByteWriter head{MutableByteSpan{frame}.first(wire::HEADER_SIZE)};
    if (!wire::WriteFrameHeader(
            head,
            wire::FrameHeader{.Length = static_cast<std::uint32_t>(length),
                              .Epoch = 0})) {
        return {};
    }
    frame.resize(wire::HEADER_SIZE + length);
    return frame;
}

/// Bytes off mux.sock, cut into frame bodies. Holds at most one frame and a
/// read's worth past it; a peer that sends a frame longer than MUX_MAX_BODY
/// or a sealed one is broken, and the caller drops it.
class MuxFrameReader {
public:
    /// How much the next read may bring.
    [[nodiscard]] std::size_t Room() const noexcept {
        return CAPACITY - Bytes_.size();
    }

    void Append(ByteSpan bytes);

    /// The next whole frame's body, nullopt until one is in, or an error for
    /// a frame no peer of ours sends.
    [[nodiscard]] ProtoResult<std::optional<std::vector<std::byte>>> Next();

private:
    static constexpr std::size_t CAPACITY = 2 * MUX_MAX_FRAME;

    std::vector<std::byte> Bytes_;
};

}  // namespace zet::app
