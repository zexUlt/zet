#include "zet/wire/message.hpp"

#include "zet/wire/frame.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace zet::wire {

namespace {

/// Copies exactly `tSize` bytes into a field the message owns. Key material and
/// nonces are held by value, never as a view into a buffer that will be reused
/// under them.
template <std::size_t tSize>
[[nodiscard]] ProtoResult<void> ReadArray(
    ByteReader& reader, std::array<std::byte, tSize>& out) noexcept {
    auto bytes = reader.ReadBytes(tSize);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    std::ranges::copy(*bytes, out.begin());
    return {};
}

/// Auth, AuthOk and Resume have no options, so anything left over is not part
/// of the message. Refusing it keeps a peer from parking bytes behind a field
/// that a later version might start reading.
[[nodiscard]] ProtoResult<void> RequireExhausted(
    const ByteReader& reader) noexcept {
    if (!reader.Exhausted()) {
        return std::unexpected(EProtoError::MalformedField);
    }
    return {};
}

[[nodiscard]] ProtoResult<void> WriteMessageType(ByteWriter& writer,
                                                 EMessageType type) noexcept {
    return writer.WriteU8(static_cast<std::uint8_t>(type));
}

}  // namespace

ProtoResult<void> TlvList::Append(const TlvOption& option) noexcept {
    if (option.Value.size() > MAX_TLV_VALUE_SIZE) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }
    for (const auto& present : Options()) {
        if (present.Type == option.Type) {
            return std::unexpected(EProtoError::MalformedField);
        }
    }
    if (Count_ == MAX_TLV_OPTIONS) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }
    Items_[Count_] = option;
    ++Count_;
    return {};
}

ProtoResult<EMessageType> ParseMessageType(ByteReader& reader) noexcept {
    auto type = reader.ReadU8();
    if (!type) {
        return std::unexpected(type.error());
    }
    switch (static_cast<EMessageType>(*type)) {
        case EMessageType::Hello:
        case EMessageType::Challenge:
        case EMessageType::Auth:
        case EMessageType::AuthOk:
        case EMessageType::Resume:
        case EMessageType::BootstrapOffer:
        case EMessageType::BootstrapAccept:
        case EMessageType::Data:
        case EMessageType::Ack:
        case EMessageType::Resize:
        case EMessageType::Ping:
        case EMessageType::Pong:
        case EMessageType::SessionEnd:
        case EMessageType::MuxRegister:
        case EMessageType::MuxRegistered:
        case EMessageType::MuxRefused:
        case EMessageType::MuxHandoff:
            return static_cast<EMessageType>(*type);
    }
    // Reserved and unassigned types alike. The disposition attached to this
    // error is Ignore, which is how a newer peer talks to an older one after
    // authentication; before it, the handshake state machine has no state to
    // skip the message with and closes the connection instead.
    return std::unexpected(EProtoError::UnknownMessageType);
}

ProtoResult<TlvList> ParseOptions(ByteReader& reader) noexcept {
    TlvList options;
    while (!reader.Exhausted()) {
        auto type = reader.ReadU16BE();
        if (!type) {
            return std::unexpected(type.error());
        }
        auto length = reader.ReadU16BE();
        if (!length) {
            return std::unexpected(length.error());
        }
        auto value = reader.ReadBytes(*length);
        if (!value) {
            return std::unexpected(value.error());
        }

        const TlvOption option{.Type = *type, .Value = *value};
        // v1 defines no options at all, so every critical one is unknown. Not
        // MalformedField: the message is well formed and understood, we simply
        // do not implement what the sender insists on. Not VersionMismatch
        // either — that one reaps the session, and this option arrived in a
        // cleartext Hello that proved nothing.
        if (option.IsCritical()) {
            return std::unexpected(EProtoError::UnsupportedCriticalOption);
        }
        if (auto appended = options.Append(option); !appended) {
            return std::unexpected(appended.error());
        }
    }
    return options;
}

ProtoResult<void> WriteOptions(ByteWriter& writer,
                               const TlvList& options) noexcept {
    for (const auto& option : options.Options()) {
        if (auto type = writer.WriteU16BE(option.Type); !type) {
            return type;
        }
        // Append refused anything wider than the length field, so the narrowing
        // here cannot lose a byte of it.
        const auto length = static_cast<std::uint16_t>(option.Value.size());
        if (auto written = writer.WriteU16BE(length); !written) {
            return written;
        }
        if (auto value = writer.WriteBytes(option.Value); !value) {
            return value;
        }
    }
    return {};
}

ProtoResult<HelloMessage> ParseHello(ByteReader& reader) noexcept {
    auto magic = reader.ReadBytes(HELLO_MAGIC.size());
    if (!magic) {
        return std::unexpected(magic.error());
    }
    if (!std::ranges::equal(*magic, HELLO_MAGIC)) {
        return std::unexpected(EProtoError::MalformedField);
    }

    auto versionMin = reader.ReadU16BE();
    if (!versionMin) {
        return std::unexpected(versionMin.error());
    }
    auto versionMax = reader.ReadU16BE();
    if (!versionMax) {
        return std::unexpected(versionMax.error());
    }
    // Not version negotiation — that happens in the state machine, over ranges
    // this layer has already found to be ranges at all.
    if (*versionMin > *versionMax) {
        return std::unexpected(EProtoError::MalformedField);
    }

    HelloMessage message{};
    message.VersionMin = *versionMin;
    message.VersionMax = *versionMax;
    if (auto id = ReadArray(reader, message.Id); !id) {
        return std::unexpected(id.error());
    }
    if (auto nonce = ReadArray(reader, message.ClientNonce); !nonce) {
        return std::unexpected(nonce.error());
    }

    auto options = ParseOptions(reader);
    if (!options) {
        return std::unexpected(options.error());
    }
    message.Options = *options;
    return message;
}

ProtoResult<void> WriteHello(ByteWriter& writer,
                             const HelloMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::Hello); !type) {
        return type;
    }
    if (auto magic = writer.WriteBytes(ByteSpan{HELLO_MAGIC}); !magic) {
        return magic;
    }
    if (auto versionMin = writer.WriteU16BE(message.VersionMin); !versionMin) {
        return versionMin;
    }
    if (auto versionMax = writer.WriteU16BE(message.VersionMax); !versionMax) {
        return versionMax;
    }
    if (auto id = writer.WriteBytes(ByteSpan{message.Id}); !id) {
        return id;
    }
    if (auto nonce = writer.WriteBytes(ByteSpan{message.ClientNonce}); !nonce) {
        return nonce;
    }
    return WriteOptions(writer, message.Options);
}

ProtoResult<ChallengeMessage> ParseChallenge(ByteReader& reader) noexcept {
    auto version = reader.ReadU16BE();
    if (!version) {
        return std::unexpected(version.error());
    }

    ChallengeMessage message{};
    message.Version = *version;
    if (auto nonce = ReadArray(reader, message.ServerNonce); !nonce) {
        return std::unexpected(nonce.error());
    }

    auto options = ParseOptions(reader);
    if (!options) {
        return std::unexpected(options.error());
    }
    message.Options = *options;
    return message;
}

ProtoResult<void> WriteChallenge(ByteWriter& writer,
                                 const ChallengeMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::Challenge); !type) {
        return type;
    }
    if (auto version = writer.WriteU16BE(message.Version); !version) {
        return version;
    }
    if (auto nonce = writer.WriteBytes(ByteSpan{message.ServerNonce}); !nonce) {
        return nonce;
    }
    return WriteOptions(writer, message.Options);
}

ProtoResult<AuthMessage> ParseAuth(ByteReader& reader) noexcept {
    AuthMessage message{};
    if (auto tag = ReadArray(reader, message.ClientTag); !tag) {
        return std::unexpected(tag.error());
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return message;
}

ProtoResult<void> WriteAuth(ByteWriter& writer,
                            const AuthMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::Auth); !type) {
        return type;
    }
    return writer.WriteBytes(ByteSpan{message.ClientTag});
}

ProtoResult<AuthOkMessage> ParseAuthOk(ByteReader& reader) noexcept {
    AuthOkMessage message{};
    if (auto tag = ReadArray(reader, message.ServerTag); !tag) {
        return std::unexpected(tag.error());
    }
    auto offset = reader.ReadU64BE();
    if (!offset) {
        return std::unexpected(offset.error());
    }
    message.ServerReceiveOffset = *offset;
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return message;
}

ProtoResult<void> WriteAuthOk(ByteWriter& writer,
                              const AuthOkMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::AuthOk); !type) {
        return type;
    }
    if (auto tag = writer.WriteBytes(ByteSpan{message.ServerTag}); !tag) {
        return tag;
    }
    return writer.WriteU64BE(message.ServerReceiveOffset);
}

ProtoResult<ResumeMessage> ParseResume(ByteReader& reader) noexcept {
    auto offset = reader.ReadU64BE();
    if (!offset) {
        return std::unexpected(offset.error());
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return ResumeMessage{.ClientReceiveOffset = *offset};
}

ProtoResult<void> WriteResume(ByteWriter& writer,
                              const ResumeMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::Resume); !type) {
        return type;
    }
    return writer.WriteU64BE(message.ClientReceiveOffset);
}

namespace {

// The terminal type and the locale: LANG, LANGUAGE and the thirteen LC_*
// categories glibc knows. Spelled out rather than matched as LC_*, so that
// the list is what a reviewer reads and nothing else.
constexpr std::array<std::string_view, MAX_BOOTSTRAP_ENV> FORWARDED_NAMES{
    "TERM",           "COLORTERM",   "LANG",
    "LANGUAGE",       "LC_ALL",      "LC_ADDRESS",
    "LC_COLLATE",     "LC_CTYPE",    "LC_IDENTIFICATION",
    "LC_MEASUREMENT", "LC_MESSAGES", "LC_MONETARY",
    "LC_NAME",        "LC_NUMERIC",  "LC_PAPER",
    "LC_TELEPHONE",   "LC_TIME"};
constexpr char FIRST_PRINTABLE = 0x20;
constexpr char LAST_PRINTABLE = 0x7E;

[[nodiscard]] bool IsPrintable(std::string_view text) noexcept {
    return std::ranges::all_of(text, [](char c) {
        return c >= FIRST_PRINTABLE && c <= LAST_PRINTABLE;
    });
}

[[nodiscard]] std::string_view AsText(ByteSpan bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] ProtoResult<std::string_view> ReadShortText(
    ByteReader& reader) noexcept {
    auto length = reader.ReadU8();
    if (!length) {
        return std::unexpected(length.error());
    }
    auto bytes = reader.ReadBytes(*length);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return AsText(*bytes);
}

[[nodiscard]] ProtoResult<void> WriteShortText(ByteWriter& writer,
                                               std::string_view text) noexcept {
    if (auto length = writer.WriteU8(static_cast<std::uint8_t>(text.size()));
        !length) {
        return length;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return writer.WriteBytes(
        ByteSpan{reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

}  // namespace

bool IsForwardedVariable(std::string_view name) noexcept {
    // == only: libc++ builds starts_with and the positional compare on
    // substr, which can throw, and under -fno-exceptions a throw is an abort
    // the core may not contain (invariant 1).
    return std::ranges::find(FORWARDED_NAMES, name) != FORWARDED_NAMES.end();
}

bool BootstrapEnvironment::Add(std::string_view name,
                               std::string_view value) noexcept {
    if (Count_ == Entries_.size() || name.size() > MAX_ENV_NAME ||
        value.size() > MAX_ENV_VALUE || !IsForwardedVariable(name) ||
        !IsPrintable(value)) {
        return false;
    }
    const auto taken = Variables();
    if (std::ranges::any_of(taken, [&](const EnvironmentVariable& variable) {
            return variable.Name() == name;
        })) {
        return false;
    }
    // In range: Count_ < Entries_.size() was checked above. Not .at(), which
    // under -fno-exceptions ends the process instead of throwing.
    EnvironmentVariable& slot =
        *std::next(Entries_.begin(), static_cast<std::ptrdiff_t>(Count_));
    std::ranges::copy(name, slot.NameBytes.begin());
    slot.NameLength = static_cast<std::uint8_t>(name.size());
    std::ranges::copy(value, slot.ValueBytes.begin());
    slot.ValueLength = static_cast<std::uint8_t>(value.size());
    ++Count_;
    return true;
}

bool BootstrapEnvironment::operator==(
    const BootstrapEnvironment& other) const noexcept {
    return std::ranges::equal(
        Variables(), other.Variables(),
        [](const EnvironmentVariable& x, const EnvironmentVariable& y) {
            return x.Name() == y.Name() && x.Value() == y.Value();
        });
}

ProtoResult<BootstrapOfferMessage> ParseBootstrapOffer(
    ByteReader& reader) noexcept {
    BootstrapOfferMessage message{};
    auto versionMin = reader.ReadU16BE();
    if (!versionMin) {
        return std::unexpected(versionMin.error());
    }
    auto versionMax = reader.ReadU16BE();
    if (!versionMax) {
        return std::unexpected(versionMax.error());
    }
    // Checked here rather than left to whoever picks the version: an empty
    // range is not a range a newer client could have meant.
    if (*versionMin > *versionMax) {
        return std::unexpected(EProtoError::MalformedField);
    }
    message.VersionMin = *versionMin;
    message.VersionMax = *versionMax;
    if (auto key = ReadArray(reader, message.ClientKey); !key) {
        return std::unexpected(key.error());
    }
    if (auto nonce = ReadArray(reader, message.ClientNonce); !nonce) {
        return std::unexpected(nonce.error());
    }
    auto count = reader.ReadU8();
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*count > MAX_BOOTSTRAP_ENV) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }
    for (std::uint8_t i = 0; i < *count; ++i) {
        auto name = ReadShortText(reader);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto value = ReadShortText(reader);
        if (!value) {
            return std::unexpected(value.error());
        }
        // Whatever Add would refuse from the client's own environment is
        // refused on the wire too: the agent sets nothing the client could
        // not have.
        if (!message.Environment.Add(*name, *value)) {
            return std::unexpected(EProtoError::MalformedField);
        }
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return message;
}

ProtoResult<void> WriteBootstrapOffer(
    ByteWriter& writer, const BootstrapOfferMessage& message) noexcept {
    if (message.VersionMin > message.VersionMax) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (auto type = WriteMessageType(writer, EMessageType::BootstrapOffer);
        !type) {
        return type;
    }
    if (auto low = writer.WriteU16BE(message.VersionMin); !low) {
        return low;
    }
    if (auto high = writer.WriteU16BE(message.VersionMax); !high) {
        return high;
    }
    if (auto key = writer.WriteBytes(ByteSpan{message.ClientKey}); !key) {
        return key;
    }
    if (auto nonce = writer.WriteBytes(ByteSpan{message.ClientNonce}); !nonce) {
        return nonce;
    }
    const auto variables = message.Environment.Variables();
    if (auto count =
            writer.WriteU8(static_cast<std::uint8_t>(variables.size()));
        !count) {
        return count;
    }
    for (const EnvironmentVariable& variable : variables) {
        if (auto name = WriteShortText(writer, variable.Name()); !name) {
            return name;
        }
        if (auto value = WriteShortText(writer, variable.Value()); !value) {
            return value;
        }
    }
    return {};
}

ProtoResult<BootstrapAcceptMessage> ParseBootstrapAccept(
    ByteReader& reader) noexcept {
    BootstrapAcceptMessage message{};
    auto version = reader.ReadU16BE();
    if (!version) {
        return std::unexpected(version.error());
    }
    message.Version = *version;
    if (auto key = ReadArray(reader, message.AgentKey); !key) {
        return std::unexpected(key.error());
    }
    if (auto echoed = ReadArray(reader, message.ClientNonce); !echoed) {
        return std::unexpected(echoed.error());
    }
    if (auto nonce = ReadArray(reader, message.AgentNonce); !nonce) {
        return std::unexpected(nonce.error());
    }
    auto port = reader.ReadU16BE();
    if (!port) {
        return std::unexpected(port.error());
    }
    // Port zero is "pick one for me" to bind(2), and nobody can connect to it.
    if (*port == 0) {
        return std::unexpected(EProtoError::MalformedField);
    }
    message.Port = *port;
    auto orphanTimeout = reader.ReadU32BE();
    if (!orphanTimeout) {
        return std::unexpected(orphanTimeout.error());
    }
    message.OrphanTimeoutSeconds = *orphanTimeout;
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return message;
}

ProtoResult<void> WriteBootstrapAccept(
    ByteWriter& writer, const BootstrapAcceptMessage& message) noexcept {
    if (message.Port == 0) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (auto type = WriteMessageType(writer, EMessageType::BootstrapAccept);
        !type) {
        return type;
    }
    if (auto version = writer.WriteU16BE(message.Version); !version) {
        return version;
    }
    if (auto key = writer.WriteBytes(ByteSpan{message.AgentKey}); !key) {
        return key;
    }
    if (auto echoed = writer.WriteBytes(ByteSpan{message.ClientNonce});
        !echoed) {
        return echoed;
    }
    if (auto nonce = writer.WriteBytes(ByteSpan{message.AgentNonce}); !nonce) {
        return nonce;
    }
    if (auto port = writer.WriteU16BE(message.Port); !port) {
        return port;
    }
    return writer.WriteU32BE(message.OrphanTimeoutSeconds);
}

ProtoResult<DataMessage> ParseData(ByteReader& reader) noexcept {
    auto ack = reader.ReadU64BE();
    if (!ack) {
        return std::unexpected(ack.error());
    }
    auto offset = reader.ReadU64BE();
    if (!offset) {
        return std::unexpected(offset.error());
    }
    // Everything remaining is the stream payload. Empty means the sender
    // should have used Ack instead (§7). Oversize means the frame that
    // carried this plaintext was already too large, but we check here
    // as well so that the codec is self-contained.
    const ByteSpan payload = reader.Rest();
    if (payload.empty()) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (payload.size() > MAX_DATA_PAYLOAD) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }
    if (auto skip = reader.Skip(payload.size()); !skip) {
        return std::unexpected(skip.error());
    }
    return DataMessage{.Ack = *ack, .Offset = *offset, .Payload = payload};
}

ProtoResult<void> WriteData(ByteWriter& writer,
                            const DataMessage& message) noexcept {
    if (message.Payload.empty()) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (message.Payload.size() > MAX_DATA_PAYLOAD) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }
    if (auto type = WriteMessageType(writer, EMessageType::Data); !type) {
        return type;
    }
    if (auto ack = writer.WriteU64BE(message.Ack); !ack) {
        return ack;
    }
    if (auto offset = writer.WriteU64BE(message.Offset); !offset) {
        return offset;
    }
    return writer.WriteBytes(message.Payload);
}

ProtoResult<AckMessage> ParseAck(ByteReader& reader) noexcept {
    auto ack = reader.ReadU64BE();
    if (!ack) {
        return std::unexpected(ack.error());
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return AckMessage{.Ack = *ack};
}

ProtoResult<void> WriteAck(ByteWriter& writer,
                           const AckMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::Ack); !type) {
        return type;
    }
    return writer.WriteU64BE(message.Ack);
}

ProtoResult<ResizeMessage> ParseResize(ByteReader& reader) noexcept {
    auto rows = reader.ReadU16BE();
    if (!rows) {
        return std::unexpected(rows.error());
    }
    auto cols = reader.ReadU16BE();
    if (!cols) {
        return std::unexpected(cols.error());
    }
    auto xpixel = reader.ReadU16BE();
    if (!xpixel) {
        return std::unexpected(xpixel.error());
    }
    auto ypixel = reader.ReadU16BE();
    if (!ypixel) {
        return std::unexpected(ypixel.error());
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return ResizeMessage{
        .Rows = *rows, .Cols = *cols, .XPixel = *xpixel, .YPixel = *ypixel};
}

ProtoResult<void> WriteResize(ByteWriter& writer,
                              const ResizeMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::Resize); !type) {
        return type;
    }
    if (auto rows = writer.WriteU16BE(message.Rows); !rows) {
        return rows;
    }
    if (auto cols = writer.WriteU16BE(message.Cols); !cols) {
        return cols;
    }
    if (auto xpixel = writer.WriteU16BE(message.XPixel); !xpixel) {
        return xpixel;
    }
    return writer.WriteU16BE(message.YPixel);
}

ProtoResult<PingMessage> ParsePing(ByteReader& reader) noexcept {
    auto token = reader.ReadU64BE();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return PingMessage{.Token = *token};
}

ProtoResult<void> WritePing(ByteWriter& writer,
                            const PingMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::Ping); !type) {
        return type;
    }
    return writer.WriteU64BE(message.Token);
}

ProtoResult<PongMessage> ParsePong(ByteReader& reader) noexcept {
    auto token = reader.ReadU64BE();
    if (!token) {
        return std::unexpected(token.error());
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return PongMessage{.Token = *token};
}

ProtoResult<void> WritePong(ByteWriter& writer,
                            const PongMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::Pong); !type) {
        return type;
    }
    return writer.WriteU64BE(message.Token);
}

namespace {

[[nodiscard]] bool IsSessionEndKind(std::uint8_t kind) noexcept {
    switch (static_cast<ESessionEndKind>(kind)) {
        case ESessionEndKind::Exited:
        case ESessionEndKind::Signaled:
            return true;
    }
    return false;
}

}  // namespace

ProtoResult<SessionEndMessage> ParseSessionEnd(ByteReader& reader) noexcept {
    auto kind = reader.ReadU8();
    if (!kind) {
        return std::unexpected(kind.error());
    }
    auto value = reader.ReadU8();
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!IsSessionEndKind(*kind)) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return SessionEndMessage{.Kind = static_cast<ESessionEndKind>(*kind),
                             .Value = *value};
}

ProtoResult<void> WriteSessionEnd(ByteWriter& writer,
                                  const SessionEndMessage& message) noexcept {
    if (!IsSessionEndKind(static_cast<std::uint8_t>(message.Kind))) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (auto type = WriteMessageType(writer, EMessageType::SessionEnd); !type) {
        return type;
    }
    if (auto kind = writer.WriteU8(static_cast<std::uint8_t>(message.Kind));
        !kind) {
        return kind;
    }
    return writer.WriteU8(message.Value);
}

namespace {

/// What every frame before authentication says in its epoch byte (§7).
constexpr std::uint8_t CLEARTEXT_EPOCH = 0;

[[nodiscard]] bool IsMuxRefusal(std::uint8_t reason) noexcept {
    switch (static_cast<EMuxRefusal>(reason)) {
        case EMuxRefusal::Taken:
        case EMuxRefusal::Limit:
        case EMuxRefusal::Malformed:
            return true;
    }
    return false;
}

}  // namespace

ProtoResult<MuxRegisterMessage> ParseMuxRegister(ByteReader& reader) noexcept {
    MuxRegisterMessage message{};
    if (auto key = ReadArray(reader, message.Registration.Expose()); !key) {
        return std::unexpected(key.error());
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return message;
}

ProtoResult<void> WriteMuxRegister(ByteWriter& writer,
                                   const MuxRegisterMessage& message) noexcept {
    if (auto type = WriteMessageType(writer, EMessageType::MuxRegister);
        !type) {
        return type;
    }
    return writer.WriteBytes(ByteSpan{message.Registration.Expose()});
}

ProtoResult<MuxRegisteredMessage> ParseMuxRegistered(
    ByteReader& reader) noexcept {
    auto port = reader.ReadU16BE();
    if (!port) {
        return std::unexpected(port.error());
    }
    // As in BootstrapAccept: nobody can connect to port zero.
    if (*port == 0) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return MuxRegisteredMessage{.Port = *port};
}

ProtoResult<void> WriteMuxRegistered(
    ByteWriter& writer, const MuxRegisteredMessage& message) noexcept {
    if (message.Port == 0) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (auto type = WriteMessageType(writer, EMessageType::MuxRegistered);
        !type) {
        return type;
    }
    return writer.WriteU16BE(message.Port);
}

ProtoResult<MuxRefusedMessage> ParseMuxRefused(ByteReader& reader) noexcept {
    auto reason = reader.ReadU8();
    if (!reason) {
        return std::unexpected(reason.error());
    }
    if (!IsMuxRefusal(*reason)) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return MuxRefusedMessage{.Reason = static_cast<EMuxRefusal>(*reason)};
}

ProtoResult<void> WriteMuxRefused(ByteWriter& writer,
                                  const MuxRefusedMessage& message) noexcept {
    if (!IsMuxRefusal(static_cast<std::uint8_t>(message.Reason))) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (auto type = WriteMessageType(writer, EMessageType::MuxRefused); !type) {
        return type;
    }
    return writer.WriteU8(static_cast<std::uint8_t>(message.Reason));
}

ProtoResult<MuxHandoffMessage> ParseMuxHandoff(ByteReader& reader) noexcept {
    auto length = reader.ReadU16BE();
    if (!length) {
        return std::unexpected(length.error());
    }
    // An empty prefix is not a connection anyone read a Hello from.
    if (*length == 0 || *length > MAX_HANDOFF_PREFIX) {
        return std::unexpected(EProtoError::MalformedField);
    }
    auto prefix = reader.ReadBytes(*length);
    if (!prefix) {
        return std::unexpected(prefix.error());
    }
    if (auto rest = RequireExhausted(reader); !rest) {
        return std::unexpected(rest.error());
    }
    return MuxHandoffMessage{.Prefix = *prefix};
}

ProtoResult<void> WriteMuxHandoff(ByteWriter& writer,
                                  const MuxHandoffMessage& message) noexcept {
    if (message.Prefix.empty() || message.Prefix.size() > MAX_HANDOFF_PREFIX) {
        return std::unexpected(EProtoError::MalformedField);
    }
    if (auto type = WriteMessageType(writer, EMessageType::MuxHandoff); !type) {
        return type;
    }
    if (auto length = writer.WriteU16BE(
            static_cast<std::uint16_t>(message.Prefix.size()));
        !length) {
        return length;
    }
    return writer.WriteBytes(message.Prefix);
}

ProtoResult<HelloPeek> PeekHello(ByteSpan buffer) noexcept {
    const auto frame = ReadFrame(buffer, MAX_HANDSHAKE_FRAME);
    if (!frame) {
        return std::unexpected(frame.error());
    }
    if (frame->Header.Epoch != CLEARTEXT_EPOCH) {
        return std::unexpected(EProtoError::UnexpectedMessage);
    }
    ByteReader reader{frame->Body};
    const auto type = ParseMessageType(reader);
    if (!type) {
        return std::unexpected(type.error());
    }
    if (*type != EMessageType::Hello) {
        return std::unexpected(EProtoError::UnexpectedMessage);
    }
    auto magic = reader.ReadBytes(HELLO_MAGIC.size());
    if (!magic) {
        return std::unexpected(magic.error());
    }
    if (!std::ranges::equal(*magic, HELLO_MAGIC)) {
        return std::unexpected(EProtoError::MalformedField);
    }
    auto versionMin = reader.ReadU16BE();
    if (!versionMin) {
        return std::unexpected(versionMin.error());
    }
    auto versionMax = reader.ReadU16BE();
    if (!versionMax) {
        return std::unexpected(versionMax.error());
    }
    if (*versionMin > *versionMax) {
        return std::unexpected(EProtoError::MalformedField);
    }
    HelloPeek peek{.FrameSize = frame->Consumed, .Id = {}};
    if (auto id = ReadArray(reader, peek.Id); !id) {
        return std::unexpected(id.error());
    }
    return peek;
}

}  // namespace zet::wire
