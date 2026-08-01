#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

namespace zet {

/// What the caller must do about an error. Carried by the error itself so that
/// deciding is not left to whoever happens to catch it.
enum class EDisposition : std::uint8_t {
    /// Skip this message, keep the connection. Unknown post-auth message types
    /// land here: they are how a newer peer talks to an older one.
    Ignore,
    /// Drop this connection. The session survives and can be resumed.
    CloseConnection,
    /// The session cannot continue. Reap it and tell the user why.
    KillSession,
};

enum class EProtoError : std::uint16_t {
    /// Fewer bytes available than the field needs.
    Truncated,
    /// A declared length exceeds the limit for its stage.
    LengthLimitExceeded,
    /// Well-formed framing, but the value makes no sense for the field.
    MalformedField,
    /// Message type is not known at this protocol version.
    UnknownMessageType,
    /// Message is known but not allowed in the current state.
    UnexpectedMessage,
    /// No overlap between the peer's version range and ours.
    VersionMismatch,
    /// AEAD tag did not verify.
    AuthenticationFailed,
    /// Output buffer is smaller than the encoded message.
    BufferTooSmall,
};

template <typename TValue>
using ProtoResult = std::expected<TValue, EProtoError>;

[[nodiscard]] constexpr EDisposition DispositionOf(EProtoError error) noexcept {
    switch (error) {
        case EProtoError::UnknownMessageType:
            return EDisposition::Ignore;

        case EProtoError::Truncated:
        case EProtoError::LengthLimitExceeded:
        case EProtoError::MalformedField:
        case EProtoError::UnexpectedMessage:
        case EProtoError::AuthenticationFailed:
        case EProtoError::BufferTooSmall:
            return EDisposition::CloseConnection;

        // A version mismatch will not resolve itself on a retry, so there is
        // nothing left to resume.
        case EProtoError::VersionMismatch:
            return EDisposition::KillSession;
    }
    return EDisposition::CloseConnection;
}

[[nodiscard]] constexpr std::string_view Describe(EProtoError error) noexcept {
    switch (error) {
        case EProtoError::Truncated:
            return "truncated";
        case EProtoError::LengthLimitExceeded:
            return "length limit exceeded";
        case EProtoError::MalformedField:
            return "malformed field";
        case EProtoError::UnknownMessageType:
            return "unknown message type";
        case EProtoError::UnexpectedMessage:
            return "unexpected message";
        case EProtoError::VersionMismatch:
            return "version mismatch";
        case EProtoError::AuthenticationFailed:
            return "authentication failed";
        case EProtoError::BufferTooSmall:
            return "buffer too small";
    }
    return "unknown error";
}

}  // namespace zet
