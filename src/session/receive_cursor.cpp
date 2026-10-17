#include "zet/session/receive_cursor.hpp"

#include <limits>

namespace zet::session {

ReceiveCursor::ReceiveCursor(std::uint64_t initialOffset) noexcept
    : RecvOffset_(initialOffset) {}

SessionResult<ByteSpan> ReceiveCursor::Receive(std::uint64_t offset,
                                               ByteSpan bytes) noexcept {
    // Guard against offset + bytes.size() wrapping past 2^64.  See header for
    // the rationale.  bytes.size() fits in uint64 because it came off a frame
    // bounded by MAX_FRAME (§7), but the check costs nothing and removes UB.
    if (bytes.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
        return std::unexpected(ESessionError::StreamGap);
    }
    const std::uint64_t endOffset =
        offset + static_cast<std::uint64_t>(bytes.size());

    if (offset > RecvOffset_) {
        // Data starts above what we have received: a gap in the ordered
        // stream.  §9: "выше — дыра, то есть CloseConnection".
        return std::unexpected(ESessionError::StreamGap);
    }

    if (endOffset <= RecvOffset_) {
        // Every byte in this frame is already received: complete duplicate.
        // Return an empty span of the original buffer — no copy needed.
        return bytes.first(0);
    }

    // Partial overlap: trim the already-seen prefix and deliver the new tail.
    // §9: "частично ниже — принимается хвост".
    const std::uint64_t skip = RecvOffset_ - offset;
    const ByteSpan newBytes = bytes.subspan(static_cast<std::size_t>(skip));

    RecvOffset_ += static_cast<std::uint64_t>(newBytes.size());
    return newBytes;
}

}  // namespace zet::session
