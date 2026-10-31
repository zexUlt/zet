#include "zet/session/send_buffer.hpp"

#include <algorithm>
#include <tuple>

namespace zet::session {

SendBuffer::SendBuffer(MutableByteSpan storage,
                       std::uint64_t initialBase) noexcept
    : Storage_(storage),
      Base_(initialBase),
      End_(initialBase),
      SendNext_(initialBase) {}

std::size_t SendBuffer::Size() const noexcept {
    return static_cast<std::size_t>(End_ - Base_);
}

std::size_t SendBuffer::Room() const noexcept {
    return Storage_.size() - Size();
}

std::size_t SendBuffer::Unsent() const noexcept {
    return static_cast<std::size_t>(End_ - SendNext_);
}

std::size_t SendBuffer::Append(ByteSpan bytes) noexcept {
    if (Storage_.empty()) {
        return 0;
    }
    const std::size_t take = std::min(bytes.size(), Room());
    if (take == 0) {
        return 0;
    }

    // Write into the ring, potentially in two pieces if the write wraps.
    // writePos = End_ mod capacity: the slot where the next byte goes.
    const std::size_t cap = Storage_.size();
    const std::size_t writePos =
        static_cast<std::size_t>(End_ % static_cast<std::uint64_t>(cap));
    const std::size_t firstPiece = std::min(take, cap - writePos);

    std::ranges::copy(bytes.first(firstPiece),
                      Storage_.subspan(writePos).begin());
    if (firstPiece < take) {
        std::ranges::copy(bytes.subspan(firstPiece, take - firstPiece),
                          Storage_.begin());
    }

    End_ += static_cast<std::uint64_t>(take);
    return take;
}

SessionResult<void> SendBuffer::Acknowledge(std::uint64_t ackOffset) noexcept {
    if (ackOffset <= Base_) {
        // Late or duplicate cumulative ack: already trimmed past this point,
        // nothing to do.  §9: "значение меньше уже присланного игнорируется".
        return {};
    }
    if (ackOffset > End_) {
        // Peer claims to have received bytes we never appended.  §9:
        // "больше, чем отправитель вообще отправил — CloseConnection".
        return std::unexpected(ESessionError::AckBeyondSent);
    }
    Base_ = ackOffset;
    // Keep SendNext in [Base, End]: it cannot lag behind the acked window.
    if (SendNext_ < Base_) {
        SendNext_ = Base_;
    }
    return {};
}

MutableByteSpan SendBuffer::CopyOut(std::uint64_t fromOffset,
                                    MutableByteSpan dest) const noexcept {
    if (Storage_.empty() || fromOffset < Base_ || fromOffset >= End_) {
        // Out of range: programming error.  Return empty rather than abort
        // (invariant §1: the protocol core must not terminate the process).
        return dest.first(0);
    }

    const std::size_t cap = Storage_.size();
    const std::size_t available = static_cast<std::size_t>(End_ - fromOffset);
    const std::size_t take = std::min(dest.size(), available);
    if (take == 0) {
        return dest.first(0);
    }

    // readPos = fromOffset mod capacity.
    const std::size_t readPos =
        static_cast<std::size_t>(fromOffset % static_cast<std::uint64_t>(cap));
    const std::size_t firstPiece = std::min(take, cap - readPos);

    std::ranges::copy(Storage_.subspan(readPos, firstPiece), dest.begin());
    if (firstPiece < take) {
        std::ranges::copy(Storage_.subspan(0, take - firstPiece),
                          dest.subspan(firstPiece).begin());
    }

    return dest.first(take);
}

SessionResult<void> SendBuffer::ResetSendNext(
    std::uint64_t peerRecvOffset) noexcept {
    if (peerRecvOffset > End_) {
        return std::unexpected(ESessionError::AckBeyondSent);
    }
    if (peerRecvOffset < Base_) {
        return std::unexpected(ESessionError::PeerBehindBuffer);
    }
    // Within [Base, End], so the acknowledgement cannot fail.
    std::ignore = Acknowledge(peerRecvOffset);
    SendNext_ = peerRecvOffset;
    return {};
}

void SendBuffer::AdvanceSendNext(std::size_t count) noexcept {
    const std::uint64_t next = SendNext_ + static_cast<std::uint64_t>(count);
    // Clamp to End_ so SendNext never overshoots.
    SendNext_ = (next > End_) ? End_ : next;
}

void SendBuffer::NewGeneration() noexcept {
    // Open a new output generation (§9 ADR-030): the pty stream restarts at
    // absolute offset 0 in the new epoch. Base, End, SendNext all reset to 0.
    // RingOverflowed is cleared; SessionGen is bumped so the agent can embed
    // it in GridBegin and the client can echo it in GridApplied / Resume.
    ++SessionGen_;
    Base_ = 0;
    End_ = 0;
    SendNext_ = 0;
    RingOverflowed_ = false;
}

}  // namespace zet::session
