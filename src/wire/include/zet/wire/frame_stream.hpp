#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "zet/core/bytes.hpp"
#include "zet/core/error.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/limits.hpp"

namespace zet::wire {

/// Reassembles frames from bytes that arrive in whatever pieces a socket hands
/// over.
///
/// The stream allocates nothing, ever. It borrows the buffer it is built with
/// and never fills more than HEADER_SIZE + MaxBodyLength() of it, so what a
/// peer can make us hold is a number chosen here rather than one it sends.
/// Bytes past that are an error; growing is not on the menu.
///
/// Lifetime of a returned frame: FrameView::Body points into the borrowed
/// buffer, and the stream reclaims those bytes at the start of the next
/// Append() or Next(). A frame is good until the next call and not one call
/// further — decrypt or copy it out before asking for more. The class is
/// neither copyable nor movable so that a live view cannot be orphaned by the
/// stream moving out from under it.
class FrameStream {
public:
    /// `storage` is where a partial frame waits. The effective limit is capped
    /// by what fits in it: hand over HEADER_SIZE + the largest body this
    /// connection will ever be allowed, and MaxBodyLength() tells you what the
    /// stream settled on.
    FrameStream(MutableByteSpan storage, std::uint32_t maxBodyLength) noexcept;

    FrameStream(const FrameStream&) = delete;
    FrameStream& operator=(const FrameStream&) = delete;
    FrameStream(FrameStream&&) = delete;
    FrameStream& operator=(FrameStream&&) = delete;
    ~FrameStream() = default;

    /// Takes a chunk of the stream. Fails with LengthLimitExceeded when it does
    /// not fit under the capacity — a peer that keeps sending without ever
    /// finishing a frame gets its connection closed, not more of our memory.
    [[nodiscard]] ProtoResult<void> Append(ByteSpan chunk) noexcept;

    /// The next whole frame, or nothing while one is still on its way.
    ///
    /// Truncation is not reported: the caller has nothing to do differently
    /// between "half a header is here" and "half a body is here", and in both
    /// cases the answer is to read more. What does come back as an error is a
    /// reason to close the connection. The stream then stands still and keeps
    /// reporting it, because the bytes that caused it are still at the front.
    [[nodiscard]] ProtoResult<std::optional<FrameView>> Next() noexcept;

    /// Widens the per-frame limit: MAX_HANDSHAKE_FRAME gives way to MAX_FRAME
    /// once the peer is authenticated.
    ///
    /// Only upwards. Narrowing is not offered because the buffer may already
    /// hold bytes accepted under the wider limit, and a capacity its own
    /// contents contradict is a worse state than a limit left where it was.
    /// The new value is capped by the storage on hand, as in the constructor.
    void RaiseLimit(std::uint32_t maxBodyLength) noexcept;

    [[nodiscard]] std::uint32_t MaxBodyLength() const noexcept {
        return MaxBodyLength_;
    }

    /// The most the buffer will ever hold: HEADER_SIZE + MaxBodyLength().
    [[nodiscard]] std::size_t Capacity() const noexcept { return Capacity_; }

    /// Bytes held back waiting for the rest of their frame.
    [[nodiscard]] std::size_t Buffered() const noexcept {
        return Size_ - Pending_;
    }

    /// What the next Append() will still take. Reading at most this much off
    /// the socket is how a caller stays out of the error path.
    [[nodiscard]] std::size_t Room() const noexcept {
        return Capacity_ - Buffered();
    }

private:
    /// Retires the frame handed out by the last Next() and slides what follows
    /// to the front. Deferred to here so that the view stays valid for exactly
    /// as long as the header promises.
    void DropPending() noexcept;

    void ApplyLimit(std::uint32_t maxBodyLength) noexcept;

    MutableByteSpan Storage_;
    std::size_t Capacity_{0};
    std::size_t Size_{0};
    std::size_t Pending_{0};
    std::uint32_t MaxBodyLength_{0};
};

}  // namespace zet::wire
