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

/// How long a connection may send nothing before it sends a Ping, per §9.
///
/// Keepalives are what lets PEER_SILENCE_TIMEOUT tell a quiet session from a
/// dead path: an idle shell sends nothing, and neither would its client. Five
/// seconds is a 40-byte frame each way per interval, nothing next to typing,
/// and four of them fit in the silence timeout, so a single lost Ping does
/// not end a connection.
inline constexpr Duration KEEPALIVE_INTERVAL = std::chrono::seconds{5};

/// How long a connection waits without hearing a single frame before it
/// gives up on the path, per §9.
///
/// The half-open case: a cut that drops packets instead of resetting leaves
/// TCP retransmitting for a quarter of an hour, and neither end learns of it
/// from the socket. Fifteen seconds is three keepalive intervals: one lost
/// Ping does not cut the connection, and a user who closed the lid notices a
/// reconnect, not a hang. ET gives up after ten (two of its five-second
/// keepalives), which a mobile link with ten-second stalls trips over; and a
/// false cut costs a reconnect, never a byte.
inline constexpr Duration PEER_SILENCE_TIMEOUT = std::chrono::seconds{15};

/// Path B (§9 ADR-030): how long to wait for the client's first Resize after
/// Resume before using the last known terminal size for the grid snapshot.
inline constexpr Duration RESIZE_WAIT_TIMEOUT = std::chrono::seconds{2};

/// Path B cost estimate: approximate scrollback contribution in bytes. §9.
inline constexpr std::uint32_t SNAPSHOT_SCROLLBACK_BYTES = 512U * 1024U;

/// Path B cost estimate: approximate bytes per terminal cell (truecolor SGR +
/// codepoint + reset). §9.
inline constexpr std::uint32_t SNAPSHOT_BYTES_PER_CELL = 35U;

/// Estimate the grid snapshot size to decide path A vs B. §9.
///
/// Only for the cost comparison; SNAPSHOT_MAX_BYTES in wire/limits.hpp is the
/// hard cap. A zero-size terminal returns SNAPSHOT_SCROLLBACK_BYTES, which is
/// cheaper than always forcing path B.
[[nodiscard]] inline std::uint64_t SnapshotEstimateBytes(
    std::uint32_t rows, std::uint32_t cols) noexcept {
    return std::uint64_t{rows} * cols * SNAPSHOT_BYTES_PER_CELL +
           SNAPSHOT_SCROLLBACK_BYTES;
}

/// The two liveness timers of one connection. Zero switches either off,
/// which only tests and fuzzers want.
struct LivenessLimits {
    Duration KeepaliveInterval{KEEPALIVE_INTERVAL};
    Duration PeerSilenceTimeout{PEER_SILENCE_TIMEOUT};
};

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
    /// For every connection of the session. An integration test shortens
    /// these to watch a half-open path end in seconds.
    LivenessLimits Liveness{};
};

}  // namespace zet::session
