#pragma once

#include <cstdint>

#include "zet/core/bytes.hpp"
#include "zet/session/error.hpp"

namespace zet::session {

/// When the cursor tells the peer it holds a byte, which is where the agent
/// and the client part ways. The client writes what it receives to the
/// terminal inside the same turn of its loop, so receipt is delivery. The
/// agent's pty can refuse input for as long as the foreground program does
/// not read it, and the agent must keep reading the socket meanwhile (the
/// peer's Acks for our output travel in the same ordered stream behind that
/// input), so it queues the bytes and acknowledges them once the pty has
/// taken them. §9.
enum class EInputAck : std::uint8_t {
    /// AckOffset() follows RecvOffset().
    OnReceipt,
    /// AckOffset() follows MarkDelivered().
    OnDelivery,
};

/// Tracks the receive offset of an incoming byte stream and slices duplicate
/// or partially-overlapping Data frames down to the new tail.
///
/// The cursor allocates nothing and makes no copies.  A Data frame is
/// described by its absolute `offset` and its decrypted plaintext `bytes`;
/// Receive() slices off the already-seen prefix and returns a sub-span of the
/// original bytes.  The caller must consume or copy the result before the
/// underlying frame buffer is recycled.
///
/// recv_offset never decreases, except for Unreceive() and Reset().  §9:
/// "смещение recv_offset не убывает".
class ReceiveCursor {
public:
    /// `initialOffset` is 0 for the first connection of a session and
    /// c_recv_offset (Resume) / s_recv_offset (AuthOk) for a
    /// reconnect.  §9: offsets are absolute for the life of the session.
    explicit ReceiveCursor(std::uint64_t initialOffset = 0,
                           EInputAck ack = EInputAck::OnReceipt) noexcept;

    ReceiveCursor(const ReceiveCursor&) = default;
    ReceiveCursor& operator=(const ReceiveCursor&) = default;

    /// Process a Data frame that carries `bytes` starting at absolute
    /// `offset`.
    ///
    /// Returns:
    ///   Ok(sub-span): the portion of `bytes` that is new to this stream.
    ///     An empty span means a complete duplicate; the caller should
    ///     discard it without error.  §9: "целиком ниже recv_offset →
    ///     отбрасывается как дубль досылки".
    ///   Err(StreamGap): `offset` is strictly above recv_offset — there is
    ///     a hole in the ordered stream, and within one authenticated
    ///     connection that cannot happen honestly.  §9: "выше — дыра".
    ///   Err(StreamGap): `offset + bytes.size()` would overflow uint64.
    ///     The computed end position wraps past 2^64, which places it above
    ///     any legitimate recv_offset this session can ever reach — the same
    ///     effect as a gap.  Frames are capped by MAX_FRAME (§7) so this path
    ///     is unreachable over a correct peer; the check prevents UB from the
    ///     C++ abstract machine treating the wrap as "unreachable".
    [[nodiscard]] SessionResult<ByteSpan> Receive(std::uint64_t offset,
                                                  ByteSpan bytes) noexcept;

    /// The absolute offset of the next byte we expect to receive.
    [[nodiscard]] std::uint64_t RecvOffset() const noexcept {
        return RecvOffset_;
    }

    /// The offset the peer is told we hold, in Acks and in the AuthOk
    /// handshake message (s_recv_offset): everything below it is out of our
    /// hands and the peer may forget it. Never above RecvOffset(). With
    /// OnReceipt it is RecvOffset(); with OnDelivery it trails by the bytes
    /// MarkDelivered() has not covered yet. Duplicates are still cut at
    /// RecvOffset(), so a peer that resends from AckOffset() after a reconnect
    /// does not deliver anything twice.
    [[nodiscard]] std::uint64_t AckOffset() const noexcept {
        return RecvOffset_ - Pending_;
    }

    /// Bytes received and handed to the caller that MarkDelivered() has not
    /// covered. Always 0 with OnReceipt.
    [[nodiscard]] std::uint64_t Pending() const noexcept { return Pending_; }

    /// The caller is done with the next `count` bytes Receive() returned
    /// (written to the pty, or discarded because the shell is gone).
    /// OnDelivery only. A count past what is pending is a defect in the
    /// caller, not in the peer: nothing changes and the result is false, since
    /// the core may not abort (invariant 1) and clamping would hide an
    /// acknowledgement that covers bytes never received.
    [[nodiscard]] bool MarkDelivered(std::uint64_t count) noexcept {
        if (count > Pending_) {
            return false;
        }
        Pending_ -= count;
        return true;
    }

    /// The caller could not take the last `count` bytes Receive() returned
    /// (the agent's pty queue is full): they count as not received, so the
    /// peer's resend from AckOffset() brings them again instead of Receive()
    /// cutting them as duplicates. Only for bytes MarkDelivered() has not
    /// covered, which is all of them when called right after Receive();
    /// OnDelivery only. A count past what is pending changes nothing and
    /// returns false, as for MarkDelivered(). This and Reset() are the only
    /// ways RecvOffset() goes down. AckOffset() does not change.
    [[nodiscard]] bool Unreceive(std::uint64_t count) noexcept {
        if (count > Pending_) {
            return false;
        }
        Pending_ -= count;
        RecvOffset_ -= count;
        return true;
    }

    /// Reset the cursor to `offset`. Used when path B (§9 ADR-030) opens a
    /// new output generation on the agent and the client applies the grid
    /// snapshot: the agent's output restarts at 0, so the client's receive
    /// cursor must also reset. The cursor is monotonic within a generation;
    /// Reset is the only exception, and it is always followed by a new
    /// GridApplied that tells the agent the client is ready.
    void Reset(std::uint64_t offset = 0) noexcept {
        RecvOffset_ = offset;
        Pending_ = 0;
    }

private:
    std::uint64_t RecvOffset_{0};
    std::uint64_t Pending_{0};
    EInputAck Ack_{EInputAck::OnReceipt};
};

}  // namespace zet::session
