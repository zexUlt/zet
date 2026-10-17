#pragma once

#include <cstdint>

#include "zet/core/bytes.hpp"
#include "zet/session/error.hpp"

namespace zet::session {

/// Tracks the receive offset of an incoming byte stream and slices duplicate
/// or partially-overlapping Data frames down to the new tail.
///
/// The cursor allocates nothing and makes no copies.  A Data frame is
/// described by its absolute `offset` and its decrypted plaintext `bytes`;
/// Receive() slices off the already-seen prefix and returns a sub-span of the
/// original bytes.  The caller must consume or copy the result before the
/// underlying frame buffer is recycled.
///
/// recv_offset never decreases.  §9: "смещение recv_offset не убывает".
class ReceiveCursor {
public:
    /// `initialOffset` is 0 for the first connection of a session and
    /// c_recv_offset / s_recv_offset (from the Resume handshake) for a
    /// reconnect.  §9: offsets are absolute for the life of the session.
    explicit ReceiveCursor(std::uint64_t initialOffset = 0) noexcept;

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

private:
    std::uint64_t RecvOffset_{0};
};

}  // namespace zet::session
