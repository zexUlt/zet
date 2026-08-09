#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"

/// The handshake messages of docs/design.md §7 and the TLV options two of them
/// carry.
///
/// Handshake messages travel in the clear: the connection keys are derived from
/// nonces these very messages exchange, so at Hello time there is nothing to
/// encrypt with. Their integrity is established afterwards by the transcript
/// MAC in Auth/AuthOk (§5), which is why nothing here may act on what it
/// parsed.
namespace zet::wire {

/// The registry from §7. 0x06…0x0F are reserved for bootstrap, 0x10…0x1F for
/// session messages, 0x20…0xFF are free.
enum class EMessageType : std::uint8_t {
    Hello = 0x01,
    Challenge = 0x02,
    Auth = 0x03,
    AuthOk = 0x04,
    Resume = 0x05,
};

/// "ZET1", the four bytes after the type byte of every Hello there will ever
/// be.
inline constexpr std::array<std::byte, 4> HELLO_MAGIC{
    std::byte{'Z'}, std::byte{'E'}, std::byte{'T'}, std::byte{'1'}};

/// type ‖ "ZET1" ‖ ver_min ‖ ver_max. §7 pins these nine bytes for good:
/// version negotiation has to work against a peer that agrees on nothing else,
/// and the type byte is read before the magic, so it cannot move either.
inline constexpr std::size_t HELLO_PREFIX_SIZE =
    sizeof(std::uint8_t) + HELLO_MAGIC.size() + 2 * sizeof(std::uint16_t);

/// Top bit of an option type: the sender cannot do without this option.
inline constexpr std::uint16_t TLV_CRITICAL_BIT = 0x8000;

/// One option. `Value` is a view into the buffer the message was parsed from
/// and does not outlive it.
struct TlvOption {
    std::uint16_t Type{0};
    ByteSpan Value{};

    [[nodiscard]] constexpr bool IsCritical() const noexcept {
        return (Type & TLV_CRITICAL_BIT) != 0;
    }
};

/// The options of one message, in wire order — which carries no meaning, §7
/// says the order is not significant.
///
/// Fixed capacity because the protocol core allocates nothing. The list also
/// owns the two rules that make an option list well formed: no duplicate type,
/// and no value too wide for the length field. A list that exists is therefore
/// always encodable.
class TlvList {
public:
    [[nodiscard]] ProtoResult<void> Append(const TlvOption& option) noexcept;

    [[nodiscard]] std::span<const TlvOption> Options() const noexcept {
        return std::span<const TlvOption>{Items_}.first(Count_);
    }

private:
    std::array<TlvOption, MAX_TLV_OPTIONS> Items_{};
    std::size_t Count_{0};
};

struct HelloMessage {
    std::uint16_t VersionMin{PROTOCOL_VERSION_MIN};
    std::uint16_t VersionMax{PROTOCOL_VERSION_MAX};
    /// A routing label, not a credential (§5): it travels in the clear and
    /// proves nothing on its own.
    SessionId Id{};
    HandshakeNonce ClientNonce{};
    TlvList Options;
};

struct ChallengeMessage {
    /// The version the agent picked out of the client's range. Choosing it is
    /// the handshake state machine's job, not this layer's.
    std::uint16_t Version{PROTOCOL_VERSION_MAX};
    HandshakeNonce ServerNonce{};
    TlvList Options;
};

struct AuthMessage {
    crypto::Mac ClientTag{};
};

struct AuthOkMessage {
    crypto::Mac ServerTag{};
    /// How much of the client's stream the agent already has, so the client
    /// replays from there and not from the start.
    std::uint64_t ServerReceiveOffset{0};
};

struct ResumeMessage {
    std::uint64_t ClientReceiveOffset{0};
};

/// Reads the byte every frame body opens with.
///
/// Parse* below pick up where this left off: dispatching on the type is what
/// tells a caller which of them to call, so re-reading the byte inside each
/// would mean handing every parser a reader rewound to a position it must then
/// trust. Write* do emit the type byte themselves — a message cannot be put on
/// the wire without saying what it is.
[[nodiscard]] ProtoResult<EMessageType> ParseMessageType(
    ByteReader& reader) noexcept;

/// Reads options until the reader runs out. A cut inside an option is an error
/// rather than the end of the list: otherwise truncating a message would
/// quietly turn it into a shorter, still-valid one.
[[nodiscard]] ProtoResult<TlvList> ParseOptions(ByteReader& reader) noexcept;

[[nodiscard]] ProtoResult<void> WriteOptions(ByteWriter& writer,
                                             const TlvList& options) noexcept;

[[nodiscard]] ProtoResult<HelloMessage> ParseHello(ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteHello(
    ByteWriter& writer, const HelloMessage& message) noexcept;

[[nodiscard]] ProtoResult<ChallengeMessage> ParseChallenge(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteChallenge(
    ByteWriter& writer, const ChallengeMessage& message) noexcept;

[[nodiscard]] ProtoResult<AuthMessage> ParseAuth(ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteAuth(ByteWriter& writer,
                                          const AuthMessage& message) noexcept;

[[nodiscard]] ProtoResult<AuthOkMessage> ParseAuthOk(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteAuthOk(
    ByteWriter& writer, const AuthOkMessage& message) noexcept;

[[nodiscard]] ProtoResult<ResumeMessage> ParseResume(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteResume(
    ByteWriter& writer, const ResumeMessage& message) noexcept;

}  // namespace zet::wire
