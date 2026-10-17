#pragma once

#include <cstdint>

#include "zet/io/errno.hpp"

/// The agent leaving the ssh session that started it, per docs/design.md §4:
/// once it has printed its bootstrap reply, ssh must be able to return while
/// the agent carries on.
namespace zet::io {

/// Which side of the fork a caller of Detach is on.
enum class EDetached : std::uint8_t {
    /// The process ssh started. It has nothing left to do and should _exit(0)
    /// at once: not exit, which would run the agent's destructors and atexit
    /// handlers a second time in a process that no longer owns anything.
    Parent,
    /// The agent from here on: a new session with no controlling terminal and
    /// stdio on /dev/null.
    Child,
};

/// Forks, and in the child starts a new session and puts 0, 1 and 2 on
/// /dev/null.
///
/// ssh returns once every writer of the channel's pipes is gone, so the
/// child must hold none of them; /dev/null also keeps the three numbers
/// taken, which Spawn relies on (zet/io/spawn.hpp). /dev/null is opened
/// before the fork, so that the one failure worth reporting still reaches
/// ssh's stderr. Output written through stdio buffers must be flushed before
/// the call; the reply is written with write(2), which has none.
///
/// The working directory stays: under ssh it is $HOME, where the shell the
/// agent starts is expected to begin. Nothing else about the process
/// changes — umask, limits and the signal mask are the caller's.
[[nodiscard]] IoResult<EDetached> Detach() noexcept;

}  // namespace zet::io
