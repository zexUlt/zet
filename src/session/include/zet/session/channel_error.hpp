#pragma once

#include <cstdint>
#include <expected>
#include <string_view>
#include <variant>

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
/// A single variant wraps both rather than a third, flattened enum: adding a
/// value to either underlying type extends this one for free, no sync needed.
/// The two types are distinct so the variant never needs a discriminant beyond
/// the index, and the index is one byte — the whole struct stays cheap to copy.
///
/// Named SessionError to match the layer name from docs/design.md §10:
/// "zet_session — expected<T, SessionError>".
class SessionError {
public:
    // NOLINTNEXTLINE(google-explicit-constructor)
    constexpr SessionError(EProtoError e) noexcept : Error_(e) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    constexpr SessionError(ESessionError e) noexcept : Error_(e) {}

    [[nodiscard]] constexpr EDisposition Disposition() const noexcept {
        return std::visit([](auto e) { return DispositionOf(e); }, Error_);
    }

    [[nodiscard]] std::string_view Description() const noexcept {
        return std::visit(
            [](auto e) -> std::string_view { return Describe(e); }, Error_);
    }

    [[nodiscard]] bool IsProto() const noexcept {
        return std::holds_alternative<EProtoError>(Error_);
    }

    [[nodiscard]] EProtoError AsProto() const noexcept {
        return std::get<EProtoError>(Error_);
    }

    [[nodiscard]] bool IsSession() const noexcept {
        return std::holds_alternative<ESessionError>(Error_);
    }

    [[nodiscard]] ESessionError AsSession() const noexcept {
        return std::get<ESessionError>(Error_);
    }

    [[nodiscard]] bool operator==(const SessionError&) const noexcept = default;

private:
    std::variant<EProtoError, ESessionError> Error_;
};

template <typename TValue>
using ChannelResult = std::expected<TValue, SessionError>;

}  // namespace zet::session
