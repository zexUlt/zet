#pragma once

#include <span>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"

/// zet, the client, per docs/design.md §3, §4 and §9: bootstraps a session
/// over ssh, then carries the local terminal over TCP to the agent and back,
/// reconnecting for as long as the session lives. One thread, one Poller
/// loop, nothing blocking once the terminal is in raw mode.
namespace zet::client {

/// The whole program; returns the remote shell's exit status, or 128 plus the
/// signal that ended it.
[[nodiscard]] int Main(std::span<char*> args) noexcept;

/// Writes `bytes` to `fd`, handling partial writes and EAGAIN with poll.
/// Returns true only when every byte has been delivered; false on a real write
/// error (EPIPE, EIO, …) or if `until` is reached before all bytes are sent.
/// Used to render a grid snapshot to the local terminal and exposed so unit
/// tests can verify the write path without a live session.
[[nodiscard]] bool TryWriteSnapshot(int fd, ByteSpan bytes,
                                    Timestamp until) noexcept;

}  // namespace zet::client
