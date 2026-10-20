#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "zet/core/byte_reader.hpp"
#include "zet/core/byte_writer.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"

/// The handshake messages of docs/design.md §7 and the TLV options two of them
/// carry, plus the sealed session messages that follow authentication.
///
/// Handshake messages (0x01…0x05) travel in the clear; session messages
/// (0x10…0x14) travel sealed inside an AEAD frame. The two groups share the
/// same type byte namespace so that a frame parser can route them all before
/// any decryption happens.
namespace zet::wire {

/// The registry from §7. 0x06…0x0F are reserved for bootstrap, 0x10…0x1F for
/// session messages, 0x20…0xFF are free.
enum class EMessageType : std::uint8_t {
    Hello = 0x01,
    Challenge = 0x02,
    Auth = 0x03,
    AuthOk = 0x04,
    Resume = 0x05,

    // Bootstrap (§4): cleartext, armored, and over the ssh channel only —
    // never on the TCP connection, where the handshake refuses them.
    BootstrapOffer = 0x06,
    BootstrapAccept = 0x07,

    // Session messages (sealed mode, §7). These arrive only after the
    // transcript MAC has been verified. The handshake state machine rejects
    // them pre-auth: ParseMessageType returning a known type is not the same
    // as the type being valid in the current state.
    Data = 0x10,
    Ack = 0x11,
    Resize = 0x12,
    Ping = 0x13,
    Pong = 0x14,
    SessionEnd = 0x15,

    // The local socket between zet-muxd and the agents (§6): cleartext, and on
    // /run/zet/mux.sock only. On TCP the handshake and the channel refuse
    // them like the bootstrap's.
    MuxRegister = 0x30,
    MuxRegistered = 0x31,
    MuxRefused = 0x32,
    MuxHandoff = 0x33,
};

/// Upper bound on the stream bytes a Data message may carry (§7).
///
/// Derived as: MAX_FRAME − crypto::TAG_SIZE − 1 (type byte) − 16 (ack:u64be ‖
/// offset:u64be) = 262144 − 16 − 1 − 16 = 262111.
/// A Data longer than this is rejected by both ParseData and WriteData.
inline constexpr std::size_t MAX_DATA_PAYLOAD = MAX_FRAME - crypto::TAG_SIZE -
                                                sizeof(std::uint8_t) -
                                                2 * sizeof(std::uint64_t);

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

/// The client's half of the key exchange (§4), written to the agent's stdin.
/// How many variables an Offer carries at most: one of each name
/// IsForwardedVariable admits.
inline constexpr std::size_t MAX_BOOTSTRAP_ENV = 17;

/// The longest name forwarded: LC_IDENTIFICATION is 17.
inline constexpr std::size_t MAX_ENV_NAME = 20;

/// The longest value: a terminal type or a locale, never a path.
inline constexpr std::size_t MAX_ENV_VALUE = 64;

/// Whether the client may hand `name` to the agent's shell (§4). ssh -T
/// carries no TERM and sshd's AcceptEnv usually only the locale, so the
/// Offer brings the terminal type and the locale and nothing else: the shell
/// is the user's own, but a list that can grow to LD_PRELOAD is a list
/// nobody reviews.
[[nodiscard]] bool IsForwardedVariable(std::string_view name) noexcept;

/// One variable for the agent's shell. Fixed storage, like every message.
struct EnvironmentVariable {
    std::array<char, MAX_ENV_NAME> NameBytes{};
    std::uint8_t NameLength{0};
    std::array<char, MAX_ENV_VALUE> ValueBytes{};
    std::uint8_t ValueLength{0};

    [[nodiscard]] std::string_view Name() const noexcept {
        return {NameBytes.data(), NameLength};
    }
    [[nodiscard]] std::string_view Value() const noexcept {
        return {ValueBytes.data(), ValueLength};
    }
};

/// The terminal environment the client hands the agent in the Offer.
class BootstrapEnvironment {
public:
    /// Adds a variable. False, and nothing added, for a name that is not
    /// forwarded, a duplicate, a name or value too long, a value with a byte
    /// outside printable ASCII, or a full list.
    [[nodiscard]] bool Add(std::string_view name,
                           std::string_view value) noexcept;

    [[nodiscard]] std::span<const EnvironmentVariable> Variables()
        const noexcept {
        return std::span{Entries_}.first(Count_);
    }

    /// The same variables with the same values, in the same order.
    [[nodiscard]] bool operator==(
        const BootstrapEnvironment& other) const noexcept;

private:
    std::array<EnvironmentVariable, MAX_BOOTSTRAP_ENV> Entries_{};
    std::size_t Count_{0};
};

struct BootstrapOfferMessage {
    std::uint16_t VersionMin{PROTOCOL_VERSION_MIN};
    std::uint16_t VersionMax{PROTOCOL_VERSION_MAX};
    crypto::PublicKey ClientKey{};
    BootstrapNonce ClientNonce{};
    BootstrapEnvironment Environment{};
};

/// The agent's half, printed on its stdout among whatever else the remote
/// shell prints there.
struct BootstrapAcceptMessage {
    /// Picked out of the Offer's range the way Challenge picks one out of
    /// Hello's: the highest both speak.
    std::uint16_t Version{PROTOCOL_VERSION_MAX};
    crypto::PublicKey AgentKey{};
    /// The Offer's nonce, back. Output of an earlier run, or anything else a
    /// .bashrc prints in the shape of an Accept, does not carry it (§4).
    BootstrapNonce ClientNonce{};
    BootstrapNonce AgentNonce{};
    /// Where the agent listens for the session's connections.
    std::uint16_t Port{0};
    /// How long the agent keeps the session without a client, in seconds; 0
    /// is for ever. The client gives up reconnecting after as long: by then
    /// there is nothing left to reconnect to (§9).
    std::uint32_t OrphanTimeoutSeconds{0};
};

/// §7. `Payload` is a non-owning view into the buffer the message was parsed
/// from; it does not outlive it and must not be empty (use Ack for "nothing
/// new, but I acknowledge").
struct DataMessage {
    std::uint64_t Ack{0};
    std::uint64_t Offset{0};
    ByteSpan Payload{};
};

struct AckMessage {
    std::uint64_t Ack{0};
};

/// Carries all four fields of struct winsize (§7). Pixel dimensions are needed
/// for sixel support (M6); putting them in the protocol now costs four bytes
/// per Resize and avoids a new message type later.
struct ResizeMessage {
    std::uint16_t Rows{0};
    std::uint16_t Cols{0};
    std::uint16_t XPixel{0};
    std::uint16_t YPixel{0};
};

struct PingMessage {
    std::uint64_t Token{0};
};

struct PongMessage {
    /// Must equal the token from the Ping this is answering (§7).
    std::uint64_t Token{0};
};

/// How the shell on the agent ended.
enum class ESessionEndKind : std::uint8_t {
    /// Value is the exit code.
    Exited = 0,
    /// Value is the number of the signal that ended it.
    Signaled = 1,
};

/// The agent's shell is gone and every byte it wrote has been acknowledged
/// (§9). Idempotent: the agent sends it on every connection until the client
/// stops coming back.
struct SessionEndMessage {
    ESessionEndKind Kind{ESessionEndKind::Exited};
    std::uint8_t Value{0};

    [[nodiscard]] friend bool operator==(
        const SessionEndMessage&, const SessionEndMessage&) noexcept = default;
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

[[nodiscard]] ProtoResult<BootstrapOfferMessage> ParseBootstrapOffer(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteBootstrapOffer(
    ByteWriter& writer, const BootstrapOfferMessage& message) noexcept;

[[nodiscard]] ProtoResult<BootstrapAcceptMessage> ParseBootstrapAccept(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteBootstrapAccept(
    ByteWriter& writer, const BootstrapAcceptMessage& message) noexcept;

[[nodiscard]] ProtoResult<DataMessage> ParseData(ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteData(ByteWriter& writer,
                                          const DataMessage& message) noexcept;

[[nodiscard]] ProtoResult<AckMessage> ParseAck(ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteAck(ByteWriter& writer,
                                         const AckMessage& message) noexcept;

[[nodiscard]] ProtoResult<ResizeMessage> ParseResize(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteResize(
    ByteWriter& writer, const ResizeMessage& message) noexcept;

[[nodiscard]] ProtoResult<PingMessage> ParsePing(ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WritePing(ByteWriter& writer,
                                          const PingMessage& message) noexcept;

[[nodiscard]] ProtoResult<PongMessage> ParsePong(ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WritePong(ByteWriter& writer,
                                          const PongMessage& message) noexcept;

/// A kind outside ESessionEndKind is MalformedField, on both sides: a newer
/// agent ending a session some new way would need a new version to say so.
/// What an agent claims its session with (§6): R, whose hash is the sid. A
/// secret like the others, and only ever sent over the local socket.
struct MuxRegisterMessage {
    crypto::Key Registration;
};

/// The registration holds. `Port` is where zet-muxd listens, the port the
/// agent announces in BootstrapAccept.
struct MuxRegisteredMessage {
    std::uint16_t Port{0};

    [[nodiscard]] friend bool operator==(const MuxRegisteredMessage&,
                                         const MuxRegisteredMessage&) noexcept =
        default;
};

enum class EMuxRefusal : std::uint8_t {
    /// The sid is registered by a live connection of another uid.
    Taken = 1,
    /// A limit of §6: registrations per uid, or the table is full.
    Limit = 2,
    /// The registration did not parse.
    Malformed = 3,
};

struct MuxRefusedMessage {
    EMuxRefusal Reason{EMuxRefusal::Malformed};

    [[nodiscard]] friend bool operator==(
        const MuxRefusedMessage&, const MuxRefusedMessage&) noexcept = default;
};

/// The most a handoff carries ahead of the socket: one handshake frame.
inline constexpr std::size_t MAX_HANDOFF_PREFIX =
    HEADER_SIZE + MAX_HANDSHAKE_FRAME;

/// One TCP connection handed to the agent (§6). On the stream:
///
///     frame header ‖ 0x33 ‖ prefix_len:u16be ‖ prefix[prefix_len]
///
/// in one frame, and the connection's descriptor rides as SCM_RIGHTS on the
/// sendmsg that carries the frame's first byte. The prefix is what the mux
/// already read from the connection — the client's Hello frame, header
/// included — and the agent reads it before the socket. A receiver pairs the
/// descriptors it collects with MuxHandoff frames in the order both arrive,
/// and closes any descriptor left without one. `Prefix` is a view into the
/// buffer the message was parsed from.
struct MuxHandoffMessage {
    ByteSpan Prefix{};
};

[[nodiscard]] ProtoResult<MuxRegisterMessage> ParseMuxRegister(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteMuxRegister(
    ByteWriter& writer, const MuxRegisterMessage& message) noexcept;

[[nodiscard]] ProtoResult<MuxRegisteredMessage> ParseMuxRegistered(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteMuxRegistered(
    ByteWriter& writer, const MuxRegisteredMessage& message) noexcept;

[[nodiscard]] ProtoResult<MuxRefusedMessage> ParseMuxRefused(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteMuxRefused(
    ByteWriter& writer, const MuxRefusedMessage& message) noexcept;

[[nodiscard]] ProtoResult<MuxHandoffMessage> ParseMuxHandoff(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteMuxHandoff(
    ByteWriter& writer, const MuxHandoffMessage& message) noexcept;

/// What zet-muxd learns from a connection's first bytes, and all it learns.
struct HelloPeek {
    /// The whole Hello frame, header included: what goes into the handoff
    /// prefix.
    std::size_t FrameSize{0};
    SessionId Id{};
};

/// Reads the frame header and the Hello prefix — type, "ZET1", both
/// versions, sid — and nothing past the sid: not the nonce, not the options.
/// `Truncated` means read more; the frame may be at most MAX_HANDSHAKE_FRAME.
/// Anything that is not a cleartext Hello is refused, as the agent's own
/// handshake would.
[[nodiscard]] ProtoResult<HelloPeek> PeekHello(ByteSpan buffer) noexcept;

[[nodiscard]] ProtoResult<SessionEndMessage> ParseSessionEnd(
    ByteReader& reader) noexcept;
[[nodiscard]] ProtoResult<void> WriteSessionEnd(
    ByteWriter& writer, const SessionEndMessage& message) noexcept;

}  // namespace zet::wire
