// The frame parser, with the whole input offered as one frame.
//
// Parsed twice, once per stage limit: MAX_HANDSHAKE_FRAME is what an
// unauthenticated peer faces, MAX_FRAME what an authenticated one does. A
// length that is legal at one stage and not at the other is exactly the case
// worth getting wrong, and a single limit would never reach it.

#include <cstddef>
#include <cstdint>

#include "zet/core/bytes.hpp"
#include "zet/wire/frame.hpp"
#include "zet/wire/limits.hpp"

namespace {

void ParseOnce(zet::ByteSpan input, std::uint32_t maxBodyLength) noexcept {
    const auto frame = zet::wire::ReadFrame(input, maxBodyLength);
    if (!frame) {
        return;
    }

    // A frame that parsed describes bytes that are really there: the body is
    // a view into the input, so a Consumed past the end would hand the next
    // layer a span over memory nobody owns.
    const bool consistent =
        frame->Body.size() == frame->Header.Length &&
        frame->Consumed == zet::wire::HEADER_SIZE + frame->Header.Length &&
        frame->Consumed <= input.size() && frame->Header.Length != 0 &&
        frame->Header.Length <= maxBodyLength;
    if (!consistent) {
        __builtin_trap();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    const zet::ByteSpan input{reinterpret_cast<const std::byte*>(data), size};
    ParseOnce(input, zet::wire::MAX_HANDSHAKE_FRAME);
    ParseOnce(input, zet::wire::MAX_FRAME);
    return 0;
}
