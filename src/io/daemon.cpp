#include "zet/io/daemon.hpp"

#include <fcntl.h>
#include <unistd.h>

#include "zet/io/fd.hpp"

namespace zet::io {

IoResult<EDetached> Detach() noexcept {
    UniqueFd null{::open("/dev/null", O_RDWR | O_CLOEXEC)};
    if (!null) {
        return std::unexpected(LastErrno("open"));
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        return std::unexpected(LastErrno("fork"));
    }
    if (pid > 0) {
        return EDetached::Parent;
    }
    // A fresh child is never a process group leader, so setsid cannot fail
    // with EPERM; it has no other way to.
    static_cast<void>(::setsid());
    for (const int target : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        if (::dup2(null.Get(), target) < 0) {
            return std::unexpected(LastErrno("dup2"));
        }
        // /dev/null may itself have come out as 0, 1 or 2 if the caller had
        // them closed, and dup2 onto itself keeps close-on-exec.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (::fcntl(target, F_SETFD, 0) != 0) {
            return std::unexpected(LastErrno("fcntl"));
        }
    }
    // Kept open if it is one of the three: closing it on the way out would
    // leave the very descriptor it was just made to fill free again.
    if (null.Get() <= STDERR_FILENO) {
        static_cast<void>(null.Release());
    }
    return EDetached::Child;
}

}  // namespace zet::io
