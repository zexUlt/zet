#include "zet/io/fd.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>

namespace zet::io {

void UniqueFd::Reset(int fd) noexcept {
    if (Fd_ >= 0) {
        // Not retried on EINTR: on Linux the descriptor is gone either way,
        // and a retry could close one that another open just reused.
        static_cast<void>(::close(Fd_));
    }
    Fd_ = fd;
}

IoResult<Pipe> MakePipe() noexcept {
    std::array<int, 2> fds{-1, -1};
    if (::pipe2(fds.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
        return std::unexpected(LastErrno("pipe2"));
    }
    return Pipe{.Read = UniqueFd{fds[0]}, .Write = UniqueFd{fds[1]}};
}

IoResult<SocketPair> MakeSocketPair() noexcept {
    std::array<int, 2> fds{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0,
                     fds.data()) != 0) {
        return std::unexpected(LastErrno("socketpair"));
    }
    return SocketPair{.First = UniqueFd{fds[0]}, .Second = UniqueFd{fds[1]}};
}

IoResult<void> SetNonBlocking(int fd) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        return std::unexpected(LastErrno("fcntl"));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return std::unexpected(LastErrno("fcntl"));
    }
    return {};
}

IoResult<std::size_t> ReadSome(int fd, MutableByteSpan into) noexcept {
    for (;;) {
        const ssize_t n = ::read(fd, into.data(), into.size());
        if (n >= 0) {
            return static_cast<std::size_t>(n);
        }
        if (errno != EINTR) {
            return std::unexpected(LastErrno("read"));
        }
    }
}

IoResult<std::size_t> WriteSome(int fd, ByteSpan bytes) noexcept {
    for (;;) {
        const ssize_t n = ::write(fd, bytes.data(), bytes.size());
        if (n >= 0) {
            return static_cast<std::size_t>(n);
        }
        if (errno != EINTR) {
            return std::unexpected(LastErrno("write"));
        }
    }
}

}  // namespace zet::io
