#include "zet/core/byte_writer.hpp"

#include <algorithm>
#include <concepts>

namespace zet {
namespace {

/// Mirror of DecodeBE in byte_reader.cpp; keep the two in step.
template <typename TValue>
    requires std::unsigned_integral<TValue>
void EncodeBE(MutableByteSpan out, TValue value) noexcept {
    constexpr std::size_t width = sizeof(TValue);
    for (std::size_t i = 0; i < width; ++i) {
        const unsigned shift = static_cast<unsigned>((width - 1 - i) * 8);
        out[i] = static_cast<std::byte>((value >> shift) & TValue{0xFF});
    }
}

}  // namespace

ProtoResult<MutableByteSpan> ByteWriter::Reserve(std::size_t count) noexcept {
    if (count > Remaining()) {
        return std::unexpected(EProtoError::BufferTooSmall);
    }
    const MutableByteSpan slot = Out_.subspan(Pos_, count);
    Pos_ += count;
    return slot;
}

ProtoResult<void> ByteWriter::WriteBytes(ByteSpan bytes) noexcept {
    auto slot = Reserve(bytes.size());
    if (!slot) {
        return std::unexpected(slot.error());
    }
    std::ranges::copy(bytes, slot->begin());
    return {};
}

ProtoResult<void> ByteWriter::WriteU8(std::uint8_t value) noexcept {
    return Reserve(1).transform(
        [value](MutableByteSpan slot) { EncodeBE(slot, value); });
}

ProtoResult<void> ByteWriter::WriteU16BE(std::uint16_t value) noexcept {
    return Reserve(2).transform(
        [value](MutableByteSpan slot) { EncodeBE(slot, value); });
}

ProtoResult<void> ByteWriter::WriteU32BE(std::uint32_t value) noexcept {
    return Reserve(4).transform(
        [value](MutableByteSpan slot) { EncodeBE(slot, value); });
}

ProtoResult<void> ByteWriter::WriteU64BE(std::uint64_t value) noexcept {
    return Reserve(8).transform(
        [value](MutableByteSpan slot) { EncodeBE(slot, value); });
}

}  // namespace zet
