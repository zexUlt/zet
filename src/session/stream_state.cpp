#include "zet/session/stream_state.hpp"

#include <algorithm>

#include "zet/wire/limits.hpp"

namespace zet::session {

StreamState::StreamState(SessionStreams streams,
                         EDetachedOverflow overflow) noexcept
    : Output_(streams.Storage, 0),
      Input_(0),
      Limits_(streams.Limits),
      Overflow_(overflow),
      DetachedSince_(streams.Now) {}

SessionResult<std::size_t> StreamState::Write(ByteSpan bytes) noexcept {
    if (State_ == ESessionState::Reaped) {
        return std::unexpected(ESessionError::SessionReaped);
    }
    // Resync mode: ring_overflowed means path B will deliver a snapshot; any
    // new bytes from the pty are discarded (the ring is full), but we return
    // bytes.size() so the pty loop does not retry stale data and block. §9.
    if (State_ == ESessionState::Detached &&
        Overflow_ == EDetachedOverflow::Resync && Output_.RingOverflowed()) {
        return bytes.size();
    }
    const std::size_t ceiling = State_ == ESessionState::Live
                                    ? Limits_.ResendMax
                                    : Limits_.DisconnectMax;
    const std::size_t held = Output_.Size();
    const std::size_t room =
        std::min(held < ceiling ? ceiling - held : 0, Output_.Room());

    if (bytes.size() > room && State_ == ESessionState::Detached) {
        if (Overflow_ == EDetachedOverflow::Reap) {
            // §9: silently corrupting the stream is the one thing not allowed.
            // Taking part of it and dropping the rest would be exactly that.
            Reap(ESessionError::DisconnectBufferOverflow);
            return std::unexpected(ESessionError::DisconnectBufferOverflow);
        }
        if (Overflow_ == EDetachedOverflow::Resync) {
            if (room == 0) {
                // Ring is full: set the flag that forces path B on reconnect.
                Output_.SetRingOverflowed();
                return 0;
            }
            // Partial fill: write what fits, then set the flag so the
            // next AcceptsWrites call sees RingOverflowed and stays true.
            // The PTY loop will keep draining even though no more bytes fit.
            auto written = Output_.Append(bytes.first(room));
            Output_.SetRingOverflowed();
            return written;
        }
    }
    return Output_.Append(bytes.first(std::min(bytes.size(), room)));
}

bool StreamState::AcceptsWrites() const noexcept {
    switch (State_) {
        case ESessionState::Live:
            return Output_.Size() < Limits_.ResendMax && Output_.Room() > 0;
        case ESessionState::Detached:
            if (Overflow_ == EDetachedOverflow::Reap) {
                // A reaping session keeps reading until it overflows: the shell
                // must not block on a client that may never come back, and the
                // overflow is how that ends.
                return true;
            }
            if (Output_.RingOverflowed()) {
                // Resync: ring full, path B is pending on reconnect.  The PTY
                // loop must keep draining the shell's output so the process
                // does not block on write(2) to the pseudo-terminal.  New
                // bytes are silently discarded by Write() (room=0, returns 0)
                // but AcceptsWrites must stay true so the loop keeps calling.
                return true;
            }
            return Output_.Size() < Limits_.DisconnectMax && Output_.Room() > 0;
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

void StreamState::SetOverflowMode(std::uint16_t negotiatedVer,
                                  bool gridCapable) noexcept {
    // Overflow mode is decided per authenticated connection, not sticky. §9.
    Overflow_ = (negotiatedVer >= wire::PROTOCOL_VERSION_GRID && gridCapable)
                    ? EDetachedOverflow::Resync
                    : EDetachedOverflow::Reap;
}

SessionResult<std::uint64_t> StreamState::AttachPathB() noexcept {
    if (State_ == ESessionState::Reaped) {
        return std::unexpected(ESessionError::SessionReaped);
    }
    // For path B we skip ResetSendNext: the caller calls NewGeneration()
    // immediately after to clear the ring (Base=0, End=0, SendNext=0).
    State_ = ESessionState::Live;
    ++Generation_;
    return Generation_;
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
