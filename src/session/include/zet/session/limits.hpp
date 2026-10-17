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

/// How long one resume exchange may take, Hello to the opened Resume, per §5.
/// A stranger can hold a half-finished handshake open for this long and no
/// longer; a real client on any network a terminal is usable over finishes
/// in a few round trips.
inline constexpr Duration HANDSHAKE_TIMEOUT = std::chrono::seconds{5};

/// The policy one session runs under, defaulting to the figures above.
///
/// A value rather than the constants themselves, the way FrameSealer takes its
/// limits: a test can reach backpressure, overflow and the orphan timeout with
/// kilobytes and virtual seconds instead of sixteen megabytes and half an hour.
struct SessionLimits {
    std::size_t ResendMax{RESEND_BUFFER_MAX};
    std::size_t DisconnectMax{DISCONNECT_BUFFER_MAX};
    /// Zero means forever, as `--orphan-timeout 0` does.
    Duration OrphanTimeout{DEFAULT_ORPHAN_TIMEOUT};
};

}  // namespace zet::session
