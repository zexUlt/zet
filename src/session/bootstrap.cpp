#include "zet/session/bootstrap.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/wire/frame.hpp"

namespace zet::session {

namespace {

/// The epoch byte of every cleartext frame (§7).
constexpr std::uint8_t CLEARTEXT_EPOCH = 0;

/// A frame of one bootstrap message, armored into `out`. `body` writes the
/// message; the frame around it is the same as for any other cleartext one.
template <std::size_t tBodySize, typename TWrite>
[[nodiscard]] ProtoResult<std::size_t> WriteArmoredMessage(
    std::span<char> out, TWrite&& write) noexcept {
    std::array<std::byte, wire::HEADER_SIZE + tBodySize> frame{};
    ByteWriter writer{MutableByteSpan{frame}};
    const wire::FrameHeader header{
        .Length = static_cast<std::uint32_t>(tBodySize),
        .Epoch = CLEARTEXT_EPOCH};
    if (auto written = wire::WriteFrameHeader(writer, header); !written) {
        return std::unexpected(written.error());
    }
    if (auto written = std::forward<TWrite>(write)(writer); !written) {
        return std::unexpected(written.error());
    }
    return wire::WriteArmor(ByteSpan{frame}, out);
}

/// The message type and the reader positioned after it, if `block` is
/// exactly one cleartext frame. Anything else is someone else's armor.
[[nodiscard]] std::optional<ByteSpan> FrameBody(ByteSpan block) noexcept {
    const auto frame = wire::ReadFrame(block, wire::MAX_HANDSHAKE_FRAME);
    if (!frame || frame->Consumed != block.size() ||
        frame->Header.Epoch != CLEARTEXT_EPOCH) {
        return std::nullopt;
    }
    return frame->Body;
}

/// The highest version both ranges hold, as Challenge picks one (§7).
[[nodiscard]] std::optional<std::uint16_t> PickVersion(
    std::uint16_t peerMin, std::uint16_t peerMax) noexcept {
    const std::uint16_t high = std::min(peerMax, wire::PROTOCOL_VERSION_MAX);
    const std::uint16_t low = std::max(peerMin, wire::PROTOCOL_VERSION_MIN);
    if (low > high) {
        return std::nullopt;
    }
    return high;
}

/// Feeds `chunk` through `scanner`, handing each block to `consider` until it
/// produces a result or fails. Shared by both ends, which differ only in what
/// they look for.
template <typename TConsider>
[[nodiscard]] ChannelResult<BootstrapStep> Scan(wire::ArmorScanner& scanner,
                                                ByteSpan chunk,
                                                TConsider&& consider) noexcept {
    std::size_t consumed = 0;
    while (consumed < chunk.size()) {
        const auto scan = scanner.Feed(chunk.subspan(consumed));
        consumed += scan.Consumed;
        if (!scan.Block) {
            continue;
        }
        auto considered = consider(*scan.Block);
        if (!considered) {
            return std::unexpected(considered.error());
        }
        if (*considered) {
            return BootstrapStep{.Consumed = consumed,
                                 .Result = std::move(*considered)};
        }
    }
    return BootstrapStep{.Consumed = consumed, .Result = std::nullopt};
}

}  // namespace

BootstrapClient::BootstrapClient(const crypto::Key& seed,
                                 const wire::BootstrapNonce& nonce) noexcept
    : Keys_(crypto::KeyPairFromSeed(seed)), Nonce_(nonce) {}

ProtoResult<std::size_t> BootstrapClient::WriteOffer(
    std::span<char> out) const noexcept {
    const wire::BootstrapOfferMessage offer{
        .VersionMin = wire::PROTOCOL_VERSION_MIN,
        .VersionMax = wire::PROTOCOL_VERSION_MAX,
        .ClientKey = Keys_.Public,
        .ClientNonce = Nonce_};
    return WriteArmoredMessage<BOOTSTRAP_OFFER_SIZE>(
        out, [&](ByteWriter& writer) {
            return wire::WriteBootstrapOffer(writer, offer);
        });
}

ChannelResult<BootstrapStep> BootstrapClient::Feed(ByteSpan chunk) noexcept {
    if (Done_) {
        return BootstrapStep{};
    }
    auto step = Scan(Scanner_, chunk,
                     [this](ByteSpan frame) { return Consider(frame); });
    if (step && step->Result) {
        Done_ = true;
    }
    return step;
}

ChannelResult<std::optional<BootstrapResult>> BootstrapClient::Consider(
    ByteSpan frame) noexcept {
    const auto body = FrameBody(frame);
    if (!body) {
        ++Ignored_;
        return std::nullopt;
    }
    ByteReader reader{*body};
    const auto type = wire::ParseMessageType(reader);
    if (!type || *type != wire::EMessageType::BootstrapAccept) {
        ++Ignored_;
        return std::nullopt;
    }
    const auto accept = wire::ParseBootstrapAccept(reader);
    // Not ours until it echoes our nonce: a garbled Accept, or one from
    // another bootstrap, is noise like the rest.
    if (!accept || accept->ClientNonce != Nonce_) {
        ++Ignored_;
        return std::nullopt;
    }

    // Ours from here on, so a refusal is final: the agent that wrote it will
    // not write another.
    if (accept->Version < wire::PROTOCOL_VERSION_MIN ||
        accept->Version > wire::PROTOCOL_VERSION_MAX) {
        return std::unexpected(SessionError{EProtoError::VersionMismatch});
    }
    auto exchanged = crypto::DeriveClientKeys(Keys_, accept->AgentKey);
    if (!exchanged) {
        return std::unexpected(SessionError{exchanged.error()});
    }
    // The client's transmit key is the agent's receive key: one value both
    // ends hold, and the root of everything the session derives.
    const crypto::Key master = wire::DeriveBootstrapMaster(
        exchanged->Transmit, accept->Version, Nonce_, accept->AgentNonce);
    return BootstrapResult{.Secrets = wire::DeriveSessionSecrets(master),
                           .Version = accept->Version,
                           .Port = accept->Port};
}

BootstrapAgent::BootstrapAgent(const crypto::Key& seed,
                               const wire::BootstrapNonce& nonce,
                               std::uint16_t port) noexcept
    : Keys_(crypto::KeyPairFromSeed(seed)), Nonce_(nonce), Port_(port) {}

ChannelResult<BootstrapStep> BootstrapAgent::Feed(ByteSpan chunk) noexcept {
    if (Accept_) {
        return BootstrapStep{};
    }
    return Scan(Scanner_, chunk,
                [this](ByteSpan frame) { return Consider(frame); });
}

ChannelResult<std::optional<BootstrapResult>> BootstrapAgent::Consider(
    ByteSpan frame) noexcept {
    const auto body = FrameBody(frame);
    if (!body) {
        ++Ignored_;
        return std::nullopt;
    }
    ByteReader reader{*body};
    const auto type = wire::ParseMessageType(reader);
    if (!type || *type != wire::EMessageType::BootstrapOffer) {
        ++Ignored_;
        return std::nullopt;
    }
    const auto offer = wire::ParseBootstrapOffer(reader);
    if (!offer) {
        return std::unexpected(SessionError{offer.error()});
    }
    const auto version = PickVersion(offer->VersionMin, offer->VersionMax);
    if (!version) {
        return std::unexpected(SessionError{EProtoError::VersionMismatch});
    }
    auto exchanged = crypto::DeriveServerKeys(Keys_, offer->ClientKey);
    if (!exchanged) {
        return std::unexpected(SessionError{exchanged.error()});
    }
    const crypto::Key master = wire::DeriveBootstrapMaster(
        exchanged->Receive, *version, offer->ClientNonce, Nonce_);
    Accept_ = wire::BootstrapAcceptMessage{.Version = *version,
                                           .AgentKey = Keys_.Public,
                                           .ClientNonce = offer->ClientNonce,
                                           .AgentNonce = Nonce_,
                                           .Port = Port_};
    return BootstrapResult{.Secrets = wire::DeriveSessionSecrets(master),
                           .Version = *version,
                           .Port = Port_};
}

ProtoResult<std::size_t> BootstrapAgent::WriteAccept(
    std::span<char> out) const noexcept {
    if (!Accept_) {
        return std::unexpected(EProtoError::UnexpectedMessage);
    }
    return WriteArmoredMessage<BOOTSTRAP_ACCEPT_SIZE>(
        out, [this](ByteWriter& writer) {
            return wire::WriteBootstrapAccept(writer, *Accept_);
        });
}

}  // namespace zet::session
