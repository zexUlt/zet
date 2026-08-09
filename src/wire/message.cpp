#include "zet/wire/message.hpp"

#include <algorithm>

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
        // v1 defines no options at all, so every critical one is unknown. The
        // error is VersionMismatch and not MalformedField: the message is well
        // formed and understood, we simply do not implement what the sender
        // insists on. Retrying will not change that, and its disposition —
        // KillSession — says so.
        if (option.IsCritical()) {
            return std::unexpected(EProtoError::VersionMismatch);
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

}  // namespace zet::wire
