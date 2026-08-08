#pragma once

#include <cstddef>
#include <cstdint>

namespace zet::wire {

/// The cleartext part of a frame: len:u32be ‖ epoch:u8. All of it goes into AD.
inline constexpr std::size_t HEADER_SIZE = 5;

/// Upper bound on the frame body length after authentication.
///
/// 256 KiB is picked so that the output of even a very chatty command fits in a
/// handful of frames, while a peer cannot make us hold more per frame.
inline constexpr std::uint32_t MAX_FRAME = 256U * 1024U;

/// Upper bound before authentication.
///
/// The handshake is made of short messages, and until the MAC checks out a peer
/// has not earned any memory: 4 KiB covers any of them, TLV included, to spare.
inline constexpr std::uint32_t MAX_HANDSHAKE_FRAME = 4U * 1024U;

}  // namespace zet::wire
