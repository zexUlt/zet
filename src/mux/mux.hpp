#pragma once

#include <span>

/// zet-muxd, per docs/design.md §6: one TCP port for every user's sessions.
/// It reads the first frame of each connection, finds the agent that
/// registered the sid it names, and hands that agent the socket itself.
/// Unprivileged, one thread, one Poller loop, and no file operation once it
/// serves: seccomp holds it to that.
namespace zet::mux {

/// The whole program; returns its exit status.
[[nodiscard]] int Main(std::span<char*> args) noexcept;

}  // namespace zet::mux
