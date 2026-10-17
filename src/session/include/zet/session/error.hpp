#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include "zet/core/error.hpp"

namespace zet::session {

/// Errors of the session layer, per docs/design.md §9.
///
/// Separate from EProtoError because they are about the streams rather than
/// about the bytes of one message: every frame that raises one of these parsed
/// and authenticated cleanly. That is also why KillSession is reachable here
/// and nowhere in the handshake — whoever raised it has proved who they are.
enum class ESessionError : std::uint8_t {
    /// The peer acknowledged bytes we never sent. It is authenticated, so this
    /// is not noise on the wire but a peer that is wrong about the stream, and
    /// nothing it says about offsets can be trusted on this connection.
    AckBeyondSent,
    /// Data starts past the end of what we have received. Within one ordered
    /// connection that cannot happen honestly; the resend after a reconnect
    /// starts at the offset we confirmed in the handshake.
    StreamGap,
    /// More output piled up while no client was connected than §9 allows. The
    /// session cannot be resumed without losing bytes, and losing them silently
    /// is the one thing §9 forbids.
    DisconnectBufferOverflow,
};

template <typename TValue>
using SessionResult = std::expected<TValue, ESessionError>;

[[nodiscard]] constexpr EDisposition DispositionOf(
    ESessionError error) noexcept {
    switch (error) {
        // The connection is suspect, the session is not: a fresh handshake
        // starts again from offsets both ends have authenticated.
        case ESessionError::AckBeyondSent:
        case ESessionError::StreamGap:
            return EDisposition::CloseConnection;

        case ESessionError::DisconnectBufferOverflow:
            return EDisposition::KillSession;
    }
    return EDisposition::CloseConnection;
}

/// For the log line and for the reason that reaches the user.
[[nodiscard]] std::string_view Describe(ESessionError error) noexcept;

}  // namespace zet::session
