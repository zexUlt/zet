#include "zet/wire/frame.hpp"

namespace zet::wire {

ProtoResult<FrameHeader> ReadFrameHeader(ByteReader& reader) noexcept {
    auto length = reader.ReadU32BE();
    if (!length) {
        return std::unexpected(length.error());
    }
    auto epoch = reader.ReadU8();
    if (!epoch) {
        return std::unexpected(epoch.error());
    }
    return FrameHeader{.Length = *length, .Epoch = *epoch};
}

ProtoResult<void> WriteFrameHeader(ByteWriter& writer,
                                   FrameHeader header) noexcept {
    auto length = writer.WriteU32BE(header.Length);
    if (!length) {
        return std::unexpected(length.error());
    }
    return writer.WriteU8(header.Epoch);
}

ProtoResult<FrameView> ReadFrame(ByteSpan buffer,
                                 std::uint32_t maxBodyLength) noexcept {
    ByteReader reader{buffer};
    auto header = ReadFrameHeader(reader);
    if (!header) {
        return std::unexpected(header.error());
    }

    // Before any allocation and before reaching for the body. The order is the
    // defence: the peer declares a length, we check it against the stage limit,
    // and only then look at how many bytes actually arrived.
    if (header->Length > maxBodyLength) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }

    // An empty body is never valid: it has no room even for the AEAD tag.
    // Rejected at this layer so a zero-length frame never reaches parsing — a
    // one-byte packet took down EternalTerminal's router in exactly that way.
    if (header->Length == 0) {
        return std::unexpected(EProtoError::MalformedField);
    }

    auto body = reader.ReadBytes(header->Length);
    if (!body) {
        return std::unexpected(body.error());
    }

    return FrameView{
        .Header = *header,
        .Body = *body,
        .Consumed = HEADER_SIZE + header->Length,
    };
}

}  // namespace zet::wire
