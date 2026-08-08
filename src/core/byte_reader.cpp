#include "zet/core/byte_reader.hpp"

#include <concepts>

namespace zet {
namespace {

/// Reads `sizeof(TValue)` bytes big-endian. Written as shifts rather than a
/// memcpy plus a byte swap so the result does not depend on the host's byte
/// order — the handshake framing in EternalTerminal writes an int64 length
/// with no conversion at all, which quietly limits it to same-endian peers.
template <typename TValue>
    requires std::unsigned_integral<TValue>
[[nodiscard]] TValue DecodeBE(ByteSpan bytes) noexcept {
    constexpr std::size_t width = sizeof(TValue);
    TValue value{0};
    for (std::size_t i = 0; i < width; ++i) {
        value = static_cast<TValue>(value << 8) |
                static_cast<TValue>(std::to_integer<std::uint8_t>(bytes[i]));
    }
    return value;
}

}  // namespace

ProtoResult<ByteSpan> ByteReader::ReadBytes(std::size_t count) noexcept {
    if (count > Remaining()) {
        return std::unexpected(EProtoError::Truncated);
    }
    const ByteSpan taken = Data_.subspan(Pos_, count);
    Pos_ += count;
    return taken;
}

ProtoResult<void> ByteReader::Skip(std::size_t count) noexcept {
    if (count > Remaining()) {
        return std::unexpected(EProtoError::Truncated);
    }
    Pos_ += count;
    return {};
}

ProtoResult<std::uint8_t> ByteReader::ReadU8() noexcept {
    return ReadBytes(1).transform(
        [](ByteSpan b) { return std::to_integer<std::uint8_t>(b[0]); });
}

ProtoResult<std::uint16_t> ByteReader::ReadU16BE() noexcept {
    return ReadBytes(2).transform(
        [](ByteSpan b) { return DecodeBE<std::uint16_t>(b); });
}

ProtoResult<std::uint32_t> ByteReader::ReadU32BE() noexcept {
    return ReadBytes(4).transform(
        [](ByteSpan b) { return DecodeBE<std::uint32_t>(b); });
}

ProtoResult<std::uint64_t> ByteReader::ReadU64BE() noexcept {
    return ReadBytes(8).transform(
        [](ByteSpan b) { return DecodeBE<std::uint64_t>(b); });
}

}  // namespace zet
