#include "zet/io/terminal.hpp"

#include <unistd.h>

#include <utility>

namespace zet::io {

bool IsTerminal(int fd) noexcept { return ::isatty(fd) == 1; }

IoResult<RawMode> RawMode::Enter(int fd) noexcept {
    termios saved{};
    if (::tcgetattr(fd, &saved) != 0) {
        return std::unexpected(LastErrno("tcgetattr"));
    }
    termios raw = saved;
    ::cfmakeraw(&raw);
    // cfmakeraw leaves software flow control on input alone.
    raw.c_iflag &= ~static_cast<tcflag_t>(IXOFF | IXANY);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(fd, TCSAFLUSH, &raw) != 0) {
        return std::unexpected(LastErrno("tcsetattr"));
    }
    return RawMode{fd, saved};
}

RawMode::RawMode(RawMode&& other) noexcept
    : Fd_(std::exchange(other.Fd_, -1)), Saved_(other.Saved_) {}

IoResult<void> RawMode::Restore() noexcept {
    if (Fd_ < 0) {
        return {};
    }
    const int fd = std::exchange(Fd_, -1);
    if (::tcsetattr(fd, TCSADRAIN, &Saved_) != 0) {
        return std::unexpected(LastErrno("tcsetattr"));
    }
    return {};
}

}  // namespace zet::io
