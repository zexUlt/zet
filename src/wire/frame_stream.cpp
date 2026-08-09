#include "zet/wire/frame_stream.hpp"

#include <algorithm>

namespace zet::wire {

FrameStream::FrameStream(MutableByteSpan storage,
                         std::uint32_t maxBodyLength) noexcept
    : Storage_(storage) {
    ApplyLimit(maxBodyLength);
}

void FrameStream::ApplyLimit(std::uint32_t maxBodyLength) noexcept {
    Capacity_ = std::min(Storage_.size(),
                         HEADER_SIZE + static_cast<std::size_t>(maxBodyLength));
    // Saturating: storage too small to hold even a header leaves no room for a
    // body, and the subtraction must not wrap into a limit of four gigabytes.
    MaxBodyLength_ = static_cast<std::uint32_t>(
        Capacity_ - std::min(Capacity_, HEADER_SIZE));
}

void FrameStream::RaiseLimit(std::uint32_t maxBodyLength) noexcept {
    ApplyLimit(std::max(maxBodyLength, MaxBodyLength_));
}

void FrameStream::DropPending() noexcept {
    if (Pending_ == 0) {
        return;
    }

    std::ranges::copy(Storage_.subspan(Pending_, Size_ - Pending_),
                      Storage_.begin());
    Size_ -= Pending_;
    Pending_ = 0;
}

ProtoResult<void> FrameStream::Append(ByteSpan chunk) noexcept {
    DropPending();

    if (chunk.size() > Capacity_ - Size_) {
        return std::unexpected(EProtoError::LengthLimitExceeded);
    }

    std::ranges::copy(chunk, Storage_.subspan(Size_).begin());
    Size_ += chunk.size();
    return {};
}

ProtoResult<std::optional<FrameView>> FrameStream::Next() noexcept {
    DropPending();

    const auto frame = ReadFrame(Storage_.first(Size_), MaxBodyLength_);
    if (!frame) {
        if (frame.error() == EProtoError::Truncated) {
            return std::optional<FrameView>{};
        }
        return std::unexpected(frame.error());
    }

    Pending_ = frame->Consumed;
    return std::optional<FrameView>{*frame};
}

}  // namespace zet::wire
