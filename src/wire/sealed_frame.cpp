#include "zet/wire/sealed_frame.hpp"

#include <algorithm>

#include "zet/core/byte_writer.hpp"

namespace zet::wire {
namespace {

/// nonce = salt ‖ u64be counter, per docs/design.md §8.
[[nodiscard]] std::array<std::byte, crypto::NONCE_SIZE> MakeNonce(
    const DirectionSalt& salt, std::uint64_t counter) noexcept {
    std::array<std::byte, crypto::NONCE_SIZE> nonce{};
    std::ranges::copy(salt, nonce.begin());

    ByteWriter writer{MutableByteSpan{nonce}.subspan(SALT_SIZE)};
    // The span is exactly eight bytes wide, so the write cannot fail; the
    // result is discarded rather than checked to keep that visible.
    static_cast<void>(writer.WriteU64BE(counter));
    return nonce;
}

}  // namespace

FrameSealer::FrameSealer(crypto::Key key, DirectionSalt salt,
                         std::uint64_t frameLimit) noexcept
    : Key_(std::move(key)), Salt_(salt), FrameLimit_(frameLimit) {}

ProtoResult<std::size_t> FrameSealer::Seal(MutableByteSpan out,
                                           ByteSpan plaintext) noexcept {
    // Refuse before doing anything else: past this point the counter would have
    // to move, and reusing one with the same key is the failure this guards.
    if (Counter_ >= FrameLimit_) {
        return std::unexpected(EProtoError::RekeyRequired);
    }

    const std::size_t bodyLength = plaintext.size() + crypto::TAG_SIZE;
    if (bodyLength > MAX_FRAME) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }
    if (out.size() < HEADER_SIZE + bodyLength) {
        return std::unexpected(EProtoError::BufferTooSmall);
    }

    const FrameHeader header{.Length = static_cast<std::uint32_t>(bodyLength),
                             .Epoch = Epoch_};

    ByteWriter writer{out};
    if (auto written = WriteFrameHeader(writer, header); !written) {
        return std::unexpected(written.error());
    }

    // The header just written is the associated data: both its fields are
    // covered by the tag, so flipping a bit in either fails the open instead of
    // redirecting the parse.
    const ByteSpan associatedData = ByteSpan{out}.first(HEADER_SIZE);
    const auto nonce = MakeNonce(Salt_, Counter_);

    const auto sealed = crypto::AeadSeal(out.subspan(HEADER_SIZE), plaintext,
                                         associatedData, Key_, ByteSpan{nonce});
    if (!sealed) {
        return std::unexpected(sealed.error());
    }

    ++Counter_;
    return HEADER_SIZE + *sealed;
}

FrameOpener::FrameOpener(crypto::Key key, DirectionSalt salt,
                         std::uint64_t frameLimit) noexcept
    : Key_(std::move(key)), Salt_(salt), FrameLimit_(frameLimit) {}

ProtoResult<ByteSpan> FrameOpener::Open(MutableByteSpan out,
                                        const FrameView& frame) noexcept {
    if (Counter_ >= FrameLimit_) {
        return std::unexpected(EProtoError::RekeyRequired);
    }

    std::array<std::byte, HEADER_SIZE> headerBytes{};
    ByteWriter writer{MutableByteSpan{headerBytes}};
    if (auto written = WriteFrameHeader(writer, frame.Header); !written) {
        return std::unexpected(written.error());
    }

    const auto nonce = MakeNonce(Salt_, Counter_);
    const auto opened = crypto::AeadOpen(out, frame.Body, ByteSpan{headerBytes},
                                         Key_, ByteSpan{nonce});
    if (!opened) {
        return std::unexpected(opened.error());
    }

    ++Counter_;
    return ByteSpan{out}.first(*opened);
}

}  // namespace zet::wire
