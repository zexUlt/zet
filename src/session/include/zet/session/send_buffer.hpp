#pragma once

#include <cstddef>
#include <cstdint>

#include "zet/core/bytes.hpp"
#include "zet/session/error.hpp"

namespace zet::session {

/// Unacknowledged outgoing plaintext held by absolute 64-bit offset over
/// storage the caller provides.  The buffer is a power-of-two-agnostic ring;
/// it neither allocates nor grows.
///
/// **Policy is the caller's responsibility.**  The buffer reports Size() and
/// Room() so the session layer can enforce the two limits from docs/design.md
/// §9 and limits.hpp:
///   - while connected:    back-pressure at RESEND_BUFFER_MAX
///   - while disconnected: DisconnectBufferOverflow past DISCONNECT_BUFFER_MAX
/// Keeping the limit in the buffer would couple it to the connection state it
/// does not own and make the overflow path untestable without a session
/// fixture.  Silently dropping old bytes is not offered: Append() returns
/// exactly how many bytes it took and stops when the ring is full; the session
/// decides what to do with the rest.
///
/// **SendNext tracking.**  The buffer tracks the "next byte to send" cursor
/// separately from Base.  On a fresh connection SendNext == End (nothing
/// queued).  After a reconnect the session calls ResetSendNext() with the
/// peer_recv_offset from the Resume message; CopyOut() + AdvanceSendNext()
/// then step through the retransmit window.  §9.
///
/// Invariants maintained at every call boundary:
///   Base  <= SendNext <= End
///   End   - Base      <= Storage.size()
class SendBuffer {
public:
    /// `storage` is borrowed for the lifetime of this object.
    /// `initialBase` is the absolute byte offset of the first slot: 0 for a
    /// new session, the current base of the stream on reconnect.  §9: offsets
    /// are absolute for the life of the session and never restart at zero.
    explicit SendBuffer(MutableByteSpan storage,
                        std::uint64_t initialBase = 0) noexcept;

    SendBuffer(const SendBuffer&) = delete;
    SendBuffer& operator=(const SendBuffer&) = delete;
    SendBuffer(SendBuffer&&) = delete;
    SendBuffer& operator=(SendBuffer&&) = delete;
    ~SendBuffer() = default;

    /// Copy up to `bytes.size()` bytes into the ring.  Returns the count
    /// actually taken, which may be less than `bytes.size()` if the ring is
    /// full.  Never negative, never more than Room().
    [[nodiscard]] std::size_t Append(ByteSpan bytes) noexcept;

    /// Acknowledge the cumulative stream up to `ackOffset`.
    ///   ackOffset <  Base  → ignored (late duplicate ack, harmless).
    ///   ackOffset >  End   → AckBeyondSent: the peer claims to have received
    ///                        bytes we never sent. §9 / error.hpp.
    ///   otherwise          → trims [Base, ackOffset) from the ring.
    [[nodiscard]] SessionResult<void> Acknowledge(
        std::uint64_t ackOffset) noexcept;

    /// Copy bytes starting at `fromOffset` into `dest`, returning the prefix
    /// of `dest` that was actually written.  The result is shorter than
    /// `dest.size()` when the ring holds fewer than `dest.size()` bytes past
    /// `fromOffset`.
    ///
    /// `fromOffset` must lie in [Base, End).  Passing a value outside that
    /// range is a programming error: to avoid aborting in the protocol core
    /// (invariant §1) the call returns an empty span rather than crashing.
    [[nodiscard]] MutableByteSpan CopyOut(std::uint64_t fromOffset,
                                          MutableByteSpan dest) const noexcept;

    /// Set the send cursor to `peerRecvOffset` for retransmission after a
    /// reconnect (peer_recv_offset from the handshake), §9. The offset is
    /// cumulative, so it also acknowledges everything below it.
    ///
    /// Nothing is clamped. Above End the peer claims bytes we never sent —
    /// AckBeyondSent. Below Base it asks for bytes it acknowledged before and
    /// we have since dropped — PeerBehindBuffer. Resending from Base instead
    /// would leave a hole the receiver refuses as StreamGap, and the session
    /// would reconnect into the same refusal for ever.
    [[nodiscard]] SessionResult<void> ResetSendNext(
        std::uint64_t peerRecvOffset) noexcept;

    /// Advance the send cursor by `count` bytes once a Data frame has been
    /// sealed from a CopyOut result.  Clamped to End.
    void AdvanceSendNext(std::size_t count) noexcept;

    /// The first un-acknowledged byte: the oldest byte still in the ring.
    [[nodiscard]] std::uint64_t Base() const noexcept { return Base_; }
    /// One past the last appended byte.
    [[nodiscard]] std::uint64_t End() const noexcept { return End_; }
    /// The next byte to be sealed into a Data frame.
    [[nodiscard]] std::uint64_t SendNext() const noexcept { return SendNext_; }
    /// Bytes currently held in the ring (== End - Base).
    [[nodiscard]] std::size_t Size() const noexcept;
    /// Bytes the ring still has room for (== Capacity - Size).
    [[nodiscard]] std::size_t Room() const noexcept;
    /// The maximum the ring can ever hold: the size of the borrowed storage.
    [[nodiscard]] std::size_t Capacity() const noexcept {
        return Storage_.size();
    }
    /// Bytes appended but not yet sent through SendNext (== End - SendNext).
    [[nodiscard]] std::size_t Unsent() const noexcept;

private:
    MutableByteSpan Storage_;
    std::uint64_t Base_{0};
    std::uint64_t End_{0};
    std::uint64_t SendNext_{0};
};

}  // namespace zet::session
