#pragma once

#include <sys/types.h>

#include <optional>
#include <span>

#include "zet/io/errno.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/pty.hpp"

/// Starting the shell on a fresh pty, per docs/design.md §10.
namespace zet::io {

/// What to run. Every pointer is the caller's and must stay valid for the
/// call; nothing is copied, because nothing may be allocated between fork and
/// execve.
struct SpawnSpec {
    /// Passed to execve as it is: no PATH search.
    const char* Path{nullptr};
    /// argv and envp as execve takes them: the last element is nullptr.
    std::span<const char* const> Argv;
    std::span<const char* const> Envp;
    WindowSize Size;
    /// The child's working directory, or nullptr to keep the parent's.
    const char* WorkingDirectory{nullptr};
};

/// A running child and the master of the pty it was given as its controlling
/// terminal, non-blocking and close-on-exec.
struct Child {
    pid_t Pid{-1};
    UniqueFd Master;
};

/// Runs `spec` on a new pty as the leader of a new session.
///
/// The child resets its signal mask and every disposition before execve: the
/// parent blocks the signals it reads through signalfd, and a shell started
/// with SIGINT blocked would never see Ctrl-C. A failed execve comes back as
/// an Errno with Call "execve", the child already reaped, rather than as a
/// child that exits with 127.
[[nodiscard]] IoResult<Child> Spawn(const SpawnSpec& spec) noexcept;

/// How a child ended.
struct ExitStatus {
    enum class EKind : std::uint8_t {
        /// Value is the exit code.
        Exited,
        /// Value is the signal that ended it.
        Signaled,
    };
    EKind Kind{EKind::Exited};
    int Value{0};

    [[nodiscard]] friend bool operator==(const ExitStatus&,
                                         const ExitStatus&) noexcept = default;
};

/// Reaps `pid` if it has ended, without waiting. nullopt while it still runs;
/// a stopped child counts as running.
[[nodiscard]] IoResult<std::optional<ExitStatus>> TryReap(pid_t pid) noexcept;

}  // namespace zet::io
