#include "zet/wire/sealed_frame.hpp"

#include <algorithm>
#include <tuple>

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
    std::ignore = writer.WriteU64BE(counter);
    return nonce;
}

}  // namespace

FrameSealer::FrameSealer(crypto::Key key, DirectionSalt salt,
                         std::uint64_t frameLimit,
                         std::uint64_t byteLimit) noexcept
    : Key_(std::move(key)),
      Salt_(salt),
      FrameLimit_(frameLimit),
      ByteLimit_(byteLimit) {}

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
    BytesSealed_ += plaintext.size();
    return HEADER_SIZE + *sealed;
}

bool FrameSealer::NeedsRekey() const noexcept {
    return Counter_ >= FrameLimit_ || BytesSealed_ >= ByteLimit_;
}

ProtoResult<void> FrameSealer::Rekey() noexcept {
    // The epoch is a byte on the wire, so it wraps at 256. Wrapping would put
    // an old epoch number on a new key, and a receiver holding the previous one
    // would try it against the wrong generation. Nothing local fixes this: the
    // connection has spent every generation it had.
    if (Epoch_ == 0xFF) {
        return std::unexpected(EProtoError::EpochsExhausted);
    }

    auto next = crypto::DeriveSubkey(Key_, Epoch_ + 1U, REKEY_CONTEXT);
    if (!next) {
        return std::unexpected(next.error());
    }

    // Assignment wipes what the key held, so the previous generation is gone
    // rather than merely unreferenced.
    Key_ = std::move(*next);
    ++Epoch_;
    Counter_ = 0;
    BytesSealed_ = 0;
    return {};
}

FrameOpener::FrameOpener(crypto::Key key, DirectionSalt salt,
                         std::uint64_t frameLimit) noexcept
    : Key_(std::move(key)), Salt_(salt), FrameLimit_(frameLimit) {}

ProtoResult<void> FrameOpener::PrepareNext() noexcept {
    if (NextReady_ || Epoch_ == 0xFF) {
        return {};
    }
    auto next = crypto::DeriveSubkey(Key_, Epoch_ + 1U, REKEY_CONTEXT);
    if (!next) {
        return std::unexpected(next.error());
    }
    NextKey_ = std::move(*next);
    NextCounter_ = 0;
    NextReady_ = true;
    return {};
}

ProtoResult<ByteSpan> FrameOpener::Open(MutableByteSpan out,
                                        const FrameView& frame) noexcept {
    // The sender moves to the next epoch on its own schedule, so frames of both
    // generations are in flight at the moment of the change. The next key is
    // stood up in advance and only becomes current once a frame actually opens
    // under it — the two-phase change from WireGuard, which is what keeps the
    // frames already on the wire from being lost.
    const bool isNextEpoch =
        frame.Header.Epoch == static_cast<std::uint8_t>(Epoch_ + 1U);
    if (isNextEpoch) {
        if (auto prepared = PrepareNext(); !prepared) {
            return std::unexpected(prepared.error());
        }
    } else if (frame.Header.Epoch != Epoch_) {
        // Neither the current generation nor the next one. Nothing here can be
        // opened, and guessing which key to try would be an oracle.
        return std::unexpected(EProtoError::AuthenticationFailed);
    }

    std::uint64_t& counter = isNextEpoch ? NextCounter_ : Counter_;
    if (counter >= FrameLimit_) {
        return std::unexpected(EProtoError::RekeyRequired);
    }

    // The buffer is exactly a header wide, so the write cannot fail; the result
    // is discarded rather than checked to keep that visible.
    std::array<std::byte, HEADER_SIZE> headerBytes{};
    ByteWriter writer{MutableByteSpan{headerBytes}};
    std::ignore = WriteFrameHeader(writer, frame.Header);

    const crypto::Key& key = isNextEpoch ? NextKey_ : Key_;
    const auto nonce = MakeNonce(Salt_, counter);
    const auto opened = crypto::AeadOpen(out, frame.Body, ByteSpan{headerBytes},
                                         key, ByteSpan{nonce});
    if (!opened) {
        return std::unexpected(opened.error());
    }

    ++counter;

    if (isNextEpoch) {
        // The confirming frame arrived: the new generation is now the only one.
        // Assignment wipes the old key rather than leaving it behind.
        Key_ = std::move(NextKey_);
        Counter_ = NextCounter_;
        ++Epoch_;
        NextReady_ = false;
        NextCounter_ = 0;
    }

    return ByteSpan{out}.first(*opened);
}

}  // namespace zet::wire
