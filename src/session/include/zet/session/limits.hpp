#pragma once

#include <chrono>
#include <cstddef>

#include "zet/core/time.hpp"

namespace zet::session {

/// Unacknowledged output held while a client is connected, per §9.
///
/// Reaching it stops reading the pty, and the shell blocks on its write: the
/// pressure travels upstream instead of bytes being thrown away. In a steady
/// state the buffer holds about one bandwidth-delay product, so 4 MiB is the
/// ceiling for a slow or stalled peer, not what an ordinary session uses.
inline constexpr std::size_t RESEND_BUFFER_MAX = 4U * 1024U * 1024U;

/// Unacknowledged output held while no client is connected, per §9.
///
/// Wider than RESEND_BUFFER_MAX because nobody is there to apply backpressure
/// for: a shell left running a build overnight should not stall on the first
/// four megabytes. Past it the session is declared unresumable and reaped
/// with a reason, until M6 replaces replay with a redraw from the grid.
inline constexpr std::size_t DISCONNECT_BUFFER_MAX = 16U * 1024U * 1024U;

/// How long a receiver waits for outgoing Data to carry its ack before sending
/// a bare Ack, per §9. Short enough that the sender's buffer is trimmed well
/// within a second, long enough that typing does not double the frame count.
inline constexpr Duration ACK_IDLE_DELAY = std::chrono::milliseconds{200};

/// How long a session waits for a client before it is reaped, per §9. The
/// option `--orphan-timeout` overrides it, and 0 there means forever.
inline constexpr Duration DEFAULT_ORPHAN_TIMEOUT = std::chrono::minutes{30};

/// The time trigger for a rekey within one connection, per §8. The frame and
/// byte triggers live with FrameSealer; time needs a clock, and the session is
/// where the clock is handed in.
inline constexpr Duration REKEY_INTERVAL = std::chrono::hours{1};

}  // namespace zet::session
