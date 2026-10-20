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

// Flags are set after the call rather than through pipe2, SOCK_CLOEXEC and
// SOCK_NONBLOCK, which macOS does not have (design.md §10). The window in
// between matters only to a process that forks from another thread while it
// is open, and the loops here fork from the one thread they run on.

IoResult<Pipe> MakeBlockingPipe() noexcept {
    std::array<int, 2> fds{-1, -1};
    if (::pipe(fds.data()) != 0) {
        return std::unexpected(LastErrno("pipe"));
    }
    Pipe pipe{.Read = UniqueFd{fds[0]}, .Write = UniqueFd{fds[1]}};
    for (const int fd : {pipe.Read.Get(), pipe.Write.Get()}) {
        if (auto closing = SetCloseOnExec(fd); !closing) {
            return std::unexpected(closing.error());
        }
    }
    return pipe;
}

IoResult<Pipe> MakePipe() noexcept {
    auto pipe = MakeBlockingPipe();
    if (!pipe) {
        return pipe;
    }
    for (const int fd : {pipe->Read.Get(), pipe->Write.Get()}) {
        if (auto nonBlocking = SetNonBlocking(fd); !nonBlocking) {
            return std::unexpected(nonBlocking.error());
        }
    }
    return pipe;
}

IoResult<SocketPair> MakeSocketPair() noexcept {
    std::array<int, 2> fds{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0) {
        return std::unexpected(LastErrno("socketpair"));
    }
    SocketPair pair{.First = UniqueFd{fds[0]}, .Second = UniqueFd{fds[1]}};
    for (const int fd : {pair.First.Get(), pair.Second.Get()}) {
        if (auto prepared = PrepareDescriptor(fd); !prepared) {
            return std::unexpected(prepared.error());
        }
    }
    return pair;
}

IoResult<void> SetCloseOnExec(int fd) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
        return std::unexpected(LastErrno("fcntl"));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        return std::unexpected(LastErrno("fcntl"));
    }
    return {};
}

IoResult<void> PrepareDescriptor(int fd) noexcept {
    if (auto closing = SetCloseOnExec(fd); !closing) {
        return closing;
    }
    return SetNonBlocking(fd);
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
