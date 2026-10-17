#include "zet/session/stream_state.hpp"

#include <algorithm>

namespace zet::session {

StreamState::StreamState(SessionStreams streams,
                         EDetachedOverflow overflow) noexcept
    : Output_(streams.Storage, 0),
      Limits_(streams.Limits),
      Overflow_(overflow),
      DetachedSince_(streams.Now) {}

SessionResult<std::size_t> StreamState::Write(ByteSpan bytes) noexcept {
    if (State_ == ESessionState::Reaped) {
        return std::unexpected(ESessionError::SessionReaped);
    }
    const std::size_t ceiling = State_ == ESessionState::Live
                                    ? Limits_.ResendMax
                                    : Limits_.DisconnectMax;
    const std::size_t held = Output_.Size();
    const std::size_t room =
        std::min(held < ceiling ? ceiling - held : 0, Output_.Room());

    if (bytes.size() > room && State_ == ESessionState::Detached &&
        Overflow_ == EDetachedOverflow::Reap) {
        // §9: silently corrupting the stream is the one thing not allowed.
        // Taking part of it and dropping the rest would be exactly that.
        Reap(ESessionError::DisconnectBufferOverflow);
        return std::unexpected(ESessionError::DisconnectBufferOverflow);
    }
    return Output_.Append(bytes.first(std::min(bytes.size(), room)));
}

bool StreamState::AcceptsWrites() const noexcept {
    switch (State_) {
        case ESessionState::Live:
            return Output_.Size() < Limits_.ResendMax && Output_.Room() > 0;
        case ESessionState::Detached:
            // A reaping session keeps reading until it overflows: the shell
            // must not block on a client that may never come back, and the
            // overflow is how that ends.
            return Overflow_ == EDetachedOverflow::Reap ||
                   (Output_.Size() < Limits_.DisconnectMax &&
                    Output_.Room() > 0);
        case ESessionState::Reaped:
            return false;
    }
    return false;
}

Deadline StreamState::NextDeadline() const noexcept {
    if (State_ != ESessionState::Detached ||
        Limits_.OrphanTimeout == Duration::zero()) {
        return Deadline{};
    }
    return Deadline{DetachedSince_ + Limits_.OrphanTimeout};
}

void StreamState::OnTimer(Timestamp now) noexcept {
    if (NextDeadline().HasExpired(now)) {
        Reap(ESessionError::OrphanTimeout);
    }
}

SessionResult<std::uint64_t> StreamState::Attach(
    std::uint64_t peerReceiveOffset) noexcept {
    if (State_ == ESessionState::Reaped) {
        return std::unexpected(ESessionError::SessionReaped);
    }
    if (auto reset = Output_.ResetSendNext(peerReceiveOffset); !reset) {
        return std::unexpected(reset.error());
    }
    State_ = ESessionState::Live;
    ++Generation_;
    return Generation_;
}

void StreamState::Detach(std::uint64_t generation, Timestamp now) noexcept {
    if (!IsCurrent(generation)) {
        return;
    }
    State_ = ESessionState::Detached;
    DetachedSince_ = now;
}

void StreamState::Kill(ESessionError reason) noexcept { Reap(reason); }

void StreamState::Reap(ESessionError reason) noexcept {
    if (State_ == ESessionState::Reaped) {
        return;
    }
    State_ = ESessionState::Reaped;
    ReapReason_ = reason;
}

}  // namespace zet::session
