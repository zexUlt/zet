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

/// The versions this build speaks, inclusive on both ends. A peer takes
/// min(ver_max_theirs, ver_max_ours) out of the overlap and never compares for
/// equality — equality is what left EternalTerminal unable to change anything
/// without breaking every deployed client. One version so far, so the range is
/// a point, and the machinery exists before it is needed on purpose.
inline constexpr std::uint16_t PROTOCOL_VERSION_MIN = 1;
inline constexpr std::uint16_t PROTOCOL_VERSION_MAX = 1;

/// Options one handshake message may carry. The registry is a table in §7 that
/// grows by review, not by whatever fits in a frame; sixteen is far above
/// anything it will hold and keeps a parsed message a fixed-size object.
inline constexpr std::size_t MAX_TLV_OPTIONS = 16;

/// The ceiling of the u16 length field rather than a policy of ours:
/// MAX_HANDSHAKE_FRAME refuses an option this wide long before it is reached.
inline constexpr std::size_t MAX_TLV_VALUE_SIZE = 0xFFFF;

/// Handshake nonces a session keeps to catch a repeat, per §5 rule three.
///
/// Sixty-four of them is a kilobyte per session and covers far more reconnects
/// than the window in which a generator that has started repeating would go
/// unnoticed. The buffer forgets, which is deliberate: what it defends against
/// is a broken generator, not a peer replaying an old nonce — a replay earns
/// nothing, since the other side still contributes a fresh one.
inline constexpr std::size_t MAX_REMEMBERED_NONCES = 64;

}  // namespace zet::wire
