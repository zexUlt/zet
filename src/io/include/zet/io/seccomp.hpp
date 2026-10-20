#pragma once

#include <cstdint>

#include "zet/io/errno.hpp"

/// The second line of §6: once zet-muxd is serving, the kernel itself
/// refuses it the file system. The first line is that its code makes no file
/// operation after start; this is for the day a bug or an exploit tries.
namespace zet::io {

enum class ESeccompProfile : std::uint8_t {
    /// zet-muxd after start: no path is opened, created, renamed, linked,
    /// removed or re-permissioned, and nothing is executed.
    NoFileOperations,
};

/// Installs `profile` for this thread and every thread and child created
/// after, irrevocably. PR_SET_NO_NEW_PRIVS goes first, as the kernel
/// requires of an unprivileged process.
///
/// What a forbidden call meets, by kind:
/// - the file operations fail with EPERM. A library on the mux's path may
///   still try one — a time zone lookup opening /etc/localtime for a log
///   line — and an error is how that degrades, not a dead mux;
/// - execve and execveat kill the process (SECCOMP_RET_KILL_PROCESS). The
///   mux runs nothing, ever; a call to exec is an exploit at work, and it
///   gets no retry;
/// - a system call made through another architecture's ABI (x32, or a
///   foreign audit arch) kills the process too: the numbers checked are
///   this architecture's, and a filter that read another's table would let
///   its open through as something else.
///
/// Everything else is allowed: the filter names what is forbidden rather
/// than what the loop needs, so that a libc update that moves a call to a
/// new syscall degrades nothing.
[[nodiscard]] IoResult<void> InstallSeccomp(ESeccompProfile profile) noexcept;

}  // namespace zet::io
