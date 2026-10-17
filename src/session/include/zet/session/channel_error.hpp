#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include "zet/core/error.hpp"
#include "zet/session/error.hpp"

namespace zet::session {

/// A unified error type for Channel, per docs/design.md §10.
///
/// The channel layer sits at the intersection of two error vocabularies: frame
/// openers speak EProtoError (tag failure, truncated body) and stream
/// processors speak ESessionError (ack beyond sent, stream gap). Both must
/// reach the caller with their disposition intact so the caller knows whether
/// to close the connection or kill the session.
///
/// Two enums and a flag rather than std::variant. Under -fno-exceptions
/// libc++ turns a bad std::get or a valueless std::visit into a call to
/// __libcpp_verbose_abort — a way to end the process from the protocol core,
/// which invariant 1 forbids, reached from whatever header code first asks.
/// The accessors here hand back optionals and cannot fail at all.
///
/// Named SessionError to match the layer name from docs/design.md §10:
/// "zet_session — expected<T, SessionError>".
class SessionError {
public:
    // NOLINTNEXTLINE(google-explicit-constructor)
    constexpr SessionError(EProtoError error) noexcept
        : Proto_(error), IsProto_(true) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    constexpr SessionError(ESessionError error) noexcept
        : Session_(error), IsProto_(false) {}

    [[nodiscard]] constexpr EDisposition Disposition() const noexcept {
        return IsProto_ ? DispositionOf(Proto_) : DispositionOf(Session_);
    }

    [[nodiscard]] std::string_view Description() const noexcept {
        return IsProto_ ? Describe(Proto_) : Describe(Session_);
    }

    /// The frame-level error, when this is one.
    [[nodiscard]] constexpr std::optional<EProtoError> Proto() const noexcept {
        return IsProto_ ? std::optional{Proto_} : std::nullopt;
    }

    /// The stream-level error, when this is one.
    [[nodiscard]] constexpr std::optional<ESessionError> Session()
        const noexcept {
        return IsProto_ ? std::nullopt : std::optional{Session_};
    }

    [[nodiscard]] friend constexpr bool operator==(
        const SessionError& left, const SessionError& right) noexcept {
        return left.Proto() == right.Proto() &&
               left.Session() == right.Session();
    }

private:
    // Only the one IsProto_ names is meaningful; the other keeps its default
    // so that the object is never partly uninitialised.
    EProtoError Proto_{};
    ESessionError Session_{};
    bool IsProto_;
};

template <typename TValue>
using ChannelResult = std::expected<TValue, SessionError>;

}  // namespace zet::session
