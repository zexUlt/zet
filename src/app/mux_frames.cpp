#include "zet/app/mux_frames.hpp"

#include <algorithm>

namespace zet::app {

void MuxFrameReader::Append(ByteSpan bytes) {
    Bytes_.insert(Bytes_.end(), bytes.begin(), bytes.end());
}

ProtoResult<std::optional<std::vector<std::byte>>> MuxFrameReader::Next() {
    const auto frame = wire::ReadFrame(
        ByteSpan{Bytes_}, static_cast<std::uint32_t>(MUX_MAX_BODY));
    if (!frame) {
        if (frame.error() == EProtoError::Truncated) {
            return std::nullopt;
        }
        return std::unexpected(frame.error());
    }
    if (frame->Header.Epoch != 0) {
        return std::unexpected(EProtoError::UnexpectedMessage);
    }
    std::vector<std::byte> body{frame->Body.begin(), frame->Body.end()};
    Bytes_.erase(Bytes_.begin(),
                 Bytes_.begin() + static_cast<std::ptrdiff_t>(frame->Consumed));
    return body;
}

}  // namespace zet::app
