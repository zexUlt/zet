#pragma once

#include <span>

/// zet-agent, per docs/design.md §3, §4 and §9: answers the bootstrap on the
/// ssh channel, leaves the ssh session, and carries one shell on a pty for as
/// long as the session lives. One thread, one Poller loop, nothing blocking.
namespace zet::agent {

/// The whole program; returns its exit status. The parent half of the detach
/// returns 0 straight away so that ssh ends; the child returns once the
/// session is over.
[[nodiscard]] int Main(std::span<char*> args) noexcept;

}  // namespace zet::agent
