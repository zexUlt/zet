// The handshake message codec, with the whole input offered as one frame body.
//
// These are the least trusted bytes in the project: a Hello arrives in the
// clear from whoever reaches the port, before any key exists to check it with.
// Three properties ride on top of "does not crash":
//
//   - what parsed re-encodes and parses back to itself, so two peers reading
//     the same bytes cannot end up holding different messages;
//   - encoding is a function of the message, so a second pass over a parsed
//     message yields the bytes of the first;
//   - no parse failure carries KillSession — nothing here is authenticated,
//     and the sid a message names is readable off the wire by anyone.
//
// The input is offered to ParseOptions on its own as well, which reaches the
// TLV code without spending 41 bytes on an intact Hello prefix first.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace zet;
using namespace zet::wire;

/// A cleartext body cannot outgrow the frame that carries it, and an encoded
/// message is never longer than the bytes it was parsed from — so one buffer of
/// this size holds any message this fuzzer can reach. The one exception is the
/// handoff, which carries a whole handshake frame and its own few bytes.
constexpr std::size_t ENCODE_CAPACITY = std::max<std::size_t>(
    MAX_HANDSHAKE_FRAME,
    sizeof(std::uint8_t) + sizeof(std::uint16_t) + MAX_HANDOFF_PREFIX);

/// Handshake messages prove nothing, so none of the ways to malform one may
/// reap a session: the sid travels in the clear, and a KillSession here would
/// hand whoever read it a way to end that session.
void CheckDisposition(EProtoError error) noexcept {
    if (DispositionOf(error) == EDisposition::KillSession) {
        __builtin_trap();
    }
}

[[nodiscard]] bool Same(const TlvList& left, const TlvList& right) noexcept {
    return std::ranges::equal(left.Options(), right.Options(),
                              [](const TlvOption& a, const TlvOption& b) {
                                  return a.Type == b.Type &&
                                         std::ranges::equal(a.Value, b.Value);
                              });
}

[[nodiscard]] bool Same(const HelloMessage& left,
                        const HelloMessage& right) noexcept {
    return left.VersionMin == right.VersionMin &&
           left.VersionMax == right.VersionMax && left.Id == right.Id &&
           left.ClientNonce == right.ClientNonce &&
           Same(left.Options, right.Options);
}

[[nodiscard]] bool Same(const ChallengeMessage& left,
                        const ChallengeMessage& right) noexcept {
    return left.Version == right.Version &&
           left.ServerNonce == right.ServerNonce &&
           Same(left.Options, right.Options);
}

[[nodiscard]] bool Same(const AuthMessage& left,
                        const AuthMessage& right) noexcept {
    return left.ClientTag == right.ClientTag;
}

[[nodiscard]] bool Same(const AuthOkMessage& left,
                        const AuthOkMessage& right) noexcept {
    return left.ServerTag == right.ServerTag &&
           left.ServerReceiveOffset == right.ServerReceiveOffset;
}

[[nodiscard]] bool Same(const ResumeMessage& left,
                        const ResumeMessage& right) noexcept {
    return left.StreamGen == right.StreamGen &&
           left.ClientReceiveOffset == right.ClientReceiveOffset;
}

[[nodiscard]] bool Same(const GridChunkMessage& left,
                        const GridChunkMessage& right) noexcept {
    return left.ChunkOffset == right.ChunkOffset &&
           std::ranges::equal(left.Data, right.Data);
}

[[nodiscard]] bool Same(const GridBeginMessage& left,
                        const GridBeginMessage& right) noexcept {
    return left.Flags == right.Flags && left.StreamGen == right.StreamGen &&
           left.ScrollbackLen == right.ScrollbackLen &&
           left.TotalLen == right.TotalLen;
}

[[nodiscard]] bool Same(const GridEndMessage&, const GridEndMessage&) noexcept {
    return true;
}

[[nodiscard]] bool Same(const GridAppliedMessage& left,
                        const GridAppliedMessage& right) noexcept {
    return left.StreamGen == right.StreamGen;
}

[[nodiscard]] bool Same(const BootstrapOfferMessage& left,
                        const BootstrapOfferMessage& right) noexcept {
    return left.VersionMin == right.VersionMin &&
           left.VersionMax == right.VersionMax &&
           left.ClientKey == right.ClientKey &&
           left.ClientNonce == right.ClientNonce &&
           left.Environment == right.Environment;
}

[[nodiscard]] bool Same(const BootstrapAcceptMessage& left,
                        const BootstrapAcceptMessage& right) noexcept {
    return left.Version == right.Version && left.AgentKey == right.AgentKey &&
           left.ClientNonce == right.ClientNonce &&
           left.AgentNonce == right.AgentNonce && left.Port == right.Port &&
           left.OrphanTimeoutSeconds == right.OrphanTimeoutSeconds;
}

[[nodiscard]] bool Same(const DataMessage& left,
                        const DataMessage& right) noexcept {
    return left.Ack == right.Ack && left.Offset == right.Offset &&
           std::ranges::equal(left.Payload, right.Payload);
}

[[nodiscard]] bool Same(const AckMessage& left,
                        const AckMessage& right) noexcept {
    return left.Ack == right.Ack;
}

[[nodiscard]] bool Same(const ResizeMessage& left,
                        const ResizeMessage& right) noexcept {
    return left.Rows == right.Rows && left.Cols == right.Cols &&
           left.XPixel == right.XPixel && left.YPixel == right.YPixel;
}

[[nodiscard]] bool Same(const PingMessage& left,
                        const PingMessage& right) noexcept {
    return left.Token == right.Token;
}

[[nodiscard]] bool Same(const PongMessage& left,
                        const PongMessage& right) noexcept {
    return left.Token == right.Token;
}

[[nodiscard]] bool Same(const SessionEndMessage& left,
                        const SessionEndMessage& right) noexcept {
    return left == right;
}

[[nodiscard]] bool Same(const MuxRegisterMessage& left,
                        const MuxRegisterMessage& right) noexcept {
    return left.Registration.Expose() == right.Registration.Expose();
}

[[nodiscard]] bool Same(const MuxRegisteredMessage& left,
                        const MuxRegisteredMessage& right) noexcept {
    return left == right;
}

[[nodiscard]] bool Same(const MuxRefusedMessage& left,
                        const MuxRefusedMessage& right) noexcept {
    return left == right;
}

[[nodiscard]] bool Same(const MuxHandoffMessage& left,
                        const MuxHandoffMessage& right) noexcept {
    return std::ranges::equal(left.Prefix, right.Prefix);
}

// Version-pinned wrappers so the function-pointer helpers keep their
// single-argument signature while Resume / SessionEnd grow version params.

ProtoResult<ResumeMessage> ParseResumeV1(ByteReader& r) noexcept {
    return ParseResume(r, PROTOCOL_VERSION_MIN);
}
ProtoResult<void> WriteResumeV1(ByteWriter& w,
                                const ResumeMessage& m) noexcept {
    return WriteResume(w, m, PROTOCOL_VERSION_MIN);
}
ProtoResult<ResumeMessage> ParseResumeV2(ByteReader& r) noexcept {
    return ParseResume(r, PROTOCOL_VERSION_GRID);
}
ProtoResult<void> WriteResumeV2(ByteWriter& w,
                                const ResumeMessage& m) noexcept {
    return WriteResume(w, m, PROTOCOL_VERSION_GRID);
}
ProtoResult<SessionEndMessage> ParseSessionEndV1(ByteReader& r) noexcept {
    return ParseSessionEnd(r, PROTOCOL_VERSION_MIN);
}
ProtoResult<void> WriteSessionEndV1(ByteWriter& w,
                                    const SessionEndMessage& m) noexcept {
    return WriteSessionEnd(w, m, PROTOCOL_VERSION_MIN);
}
ProtoResult<SessionEndMessage> ParseSessionEndV2(ByteReader& r) noexcept {
    return ParseSessionEnd(r, PROTOCOL_VERSION_GRID);
}
ProtoResult<void> WriteSessionEndV2(ByteWriter& w,
                                    const SessionEndMessage& m) noexcept {
    return WriteSessionEnd(w, m, PROTOCOL_VERSION_GRID);
}

template <typename TMessage>
using MessageParser = ProtoResult<TMessage> (*)(ByteReader&) noexcept;

template <typename TMessage>
using MessageWriter = ProtoResult<void> (*)(ByteWriter&,
                                            const TMessage&) noexcept;

/// Encodes a parsed message, parses that back, and encodes the result again.
/// The first pass catches two halves of the codec that disagree about the same
/// bytes; the second catches an encoding that depends on anything but the
/// message.
template <typename TMessage>
void CheckRoundTrip(EMessageType type, const TMessage& message,
                    MessageParser<TMessage> parse,
                    MessageWriter<TMessage> write) noexcept {
    std::array<std::byte, ENCODE_CAPACITY> firstBytes{};
    ByteWriter first{MutableByteSpan{firstBytes}};
    // Whatever parsed is encodable: TlvList refuses at Append everything
    // WriteOptions could choke on, and the encoding fits where the input did.
    if (!write(first, message)) {
        __builtin_trap();
    }

    // Through the type byte as a receiver would: a message that re-encodes as
    // something else is the asymmetry this is looking for.
    ByteReader reader{first.Written()};
    const auto reparsedType = ParseMessageType(reader);
    if (!reparsedType || *reparsedType != type) {
        __builtin_trap();
    }
    const auto reparsed = parse(reader);
    if (!reparsed || !reader.Exhausted() || !Same(message, *reparsed)) {
        __builtin_trap();
    }

    // The options of `reparsed` are views into firstBytes, which outlives this.
    std::array<std::byte, ENCODE_CAPACITY> secondBytes{};
    ByteWriter second{MutableByteSpan{secondBytes}};
    if (!write(second, *reparsed)) {
        __builtin_trap();
    }
    if (!std::ranges::equal(first.Written(), second.Written())) {
        __builtin_trap();
    }
}

template <typename TMessage>
void ParseThenRoundTrip(ByteReader& reader, EMessageType type,
                        MessageParser<TMessage> parse,
                        MessageWriter<TMessage> write) noexcept {
    const auto parsed = parse(reader);
    if (!parsed) {
        CheckDisposition(parsed.error());
        return;
    }
    CheckRoundTrip(type, *parsed, parse, write);
}

void ParseBody(ByteSpan input) noexcept {
    ByteReader reader{input};
    const auto type = ParseMessageType(reader);
    if (!type) {
        CheckDisposition(type.error());
        return;
    }
    switch (*type) {
        case EMessageType::Hello:
            ParseThenRoundTrip(reader, *type, ParseHello, WriteHello);
            return;
        case EMessageType::Challenge:
            ParseThenRoundTrip(reader, *type, ParseChallenge, WriteChallenge);
            return;
        case EMessageType::Auth:
            ParseThenRoundTrip(reader, *type, ParseAuth, WriteAuth);
            return;
        case EMessageType::AuthOk:
            ParseThenRoundTrip(reader, *type, ParseAuthOk, WriteAuthOk);
            return;
        case EMessageType::Resume: {
            // Exercise both encodings from the same body bytes; each
            // version is self-consistent independently.
            const ByteSpan body = reader.Rest();
            ByteReader r1{body};
            ParseThenRoundTrip(r1, *type, ParseResumeV1, WriteResumeV1);
            ByteReader r2{body};
            ParseThenRoundTrip(r2, *type, ParseResumeV2, WriteResumeV2);
            // Cross-version trap: v1 and v2 have different wire lengths (9 vs
            // 13 bytes total), so the same body cannot be accepted by both.  If
            // both parse successfully the codec has a structural defect.
            {
                ByteReader check1{body};
                ByteReader check2{body};
                const auto ok1 = ParseResume(check1, PROTOCOL_VERSION_MIN);
                const auto ok2 = ParseResume(check2, PROTOCOL_VERSION_GRID);
                if (ok1.has_value() && ok2.has_value()) {
                    __builtin_trap();
                }
            }
            return;
        }
        case EMessageType::BootstrapOffer:
            ParseThenRoundTrip(reader, *type, ParseBootstrapOffer,
                               WriteBootstrapOffer);
            return;
        case EMessageType::BootstrapAccept:
            ParseThenRoundTrip(reader, *type, ParseBootstrapAccept,
                               WriteBootstrapAccept);
            return;
        case EMessageType::Data:
            ParseThenRoundTrip(reader, *type, ParseData, WriteData);
            return;
        case EMessageType::Ack:
            ParseThenRoundTrip(reader, *type, ParseAck, WriteAck);
            return;
        case EMessageType::Resize:
            ParseThenRoundTrip(reader, *type, ParseResize, WriteResize);
            return;
        case EMessageType::Ping:
            ParseThenRoundTrip(reader, *type, ParsePing, WritePing);
            return;
        case EMessageType::Pong:
            ParseThenRoundTrip(reader, *type, ParsePong, WritePong);
            return;
        case EMessageType::SessionEnd: {
            const ByteSpan body = reader.Rest();
            ByteReader r1{body};
            ParseThenRoundTrip(r1, *type, ParseSessionEndV1, WriteSessionEndV1);
            ByteReader r2{body};
            ParseThenRoundTrip(r2, *type, ParseSessionEndV2, WriteSessionEndV2);
            return;
        }
        case EMessageType::GridChunk:
            ParseThenRoundTrip(reader, *type, ParseGridChunk, WriteGridChunk);
            return;
        case EMessageType::GridBegin:
            ParseThenRoundTrip(reader, *type, ParseGridBegin, WriteGridBegin);
            return;
        case EMessageType::GridEnd:
            ParseThenRoundTrip(reader, *type, ParseGridEnd, WriteGridEnd);
            return;
        case EMessageType::GridApplied:
            ParseThenRoundTrip(reader, *type, ParseGridApplied,
                               WriteGridApplied);
            return;
        case EMessageType::MuxRegister:
            ParseThenRoundTrip(reader, *type, ParseMuxRegister,
                               WriteMuxRegister);
            return;
        case EMessageType::MuxRegistered:
            ParseThenRoundTrip(reader, *type, ParseMuxRegistered,
                               WriteMuxRegistered);
            return;
        case EMessageType::MuxRefused:
            ParseThenRoundTrip(reader, *type, ParseMuxRefused, WriteMuxRefused);
            return;
        case EMessageType::MuxHandoff:
            ParseThenRoundTrip(reader, *type, ParseMuxHandoff, WriteMuxHandoff);
            return;
    }
}

/// The option list on its own. An unknown option that is not critical has to
/// survive being carried past us untouched, and that is only worth checking
/// where the fuzzer can still reach the list — behind a Hello prefix, any
/// mutation in the first 41 bytes costs it the whole message.
void ParseOptionList(ByteSpan input) noexcept {
    ByteReader reader{input};
    const auto parsed = ParseOptions(reader);
    if (!parsed) {
        CheckDisposition(parsed.error());
        return;
    }

    std::array<std::byte, ENCODE_CAPACITY> firstBytes{};
    ByteWriter first{MutableByteSpan{firstBytes}};
    if (!WriteOptions(first, *parsed)) {
        __builtin_trap();
    }

    ByteReader again{first.Written()};
    const auto reparsed = ParseOptions(again);
    if (!reparsed || !again.Exhausted() || !Same(*parsed, *reparsed)) {
        __builtin_trap();
    }

    std::array<std::byte, ENCODE_CAPACITY> secondBytes{};
    ByteWriter second{MutableByteSpan{secondBytes}};
    if (!WriteOptions(second, *reparsed)) {
        __builtin_trap();
    }
    if (!std::ranges::equal(first.Written(), second.Written())) {
        __builtin_trap();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    // Longer than a cleartext body can be: the frame parser turns such a length
    // away before the codec is ever handed the bytes.
    if (size > zet::wire::MAX_HANDSHAKE_FRAME) {
        return 0;
    }

    const zet::ByteSpan input{reinterpret_cast<const std::byte*>(data), size};
    ParseBody(input);
    ParseOptionList(input);
    return 0;
}
