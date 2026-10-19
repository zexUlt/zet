#pragma once

#include <span>

/// zet, the client, per docs/design.md §3, §4 and §9: bootstraps a session
/// over ssh, then carries the local terminal over TCP to the agent and back,
/// reconnecting for as long as the session lives. One thread, one Poller
/// loop, nothing blocking once the terminal is in raw mode.
namespace zet::client {

/// The whole program; returns the remote shell's exit status, or 128 plus the
/// signal that ended it.
[[nodiscard]] int Main(std::span<char*> args) noexcept;

}  // namespace zet::client
