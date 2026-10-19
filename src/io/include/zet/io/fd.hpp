#pragma once

#include <utility>

#include "zet/core/bytes.hpp"
#include "zet/io/errno.hpp"

namespace zet::io {

/// Owns one file descriptor and closes it once. Move-only: two owners of a
/// descriptor is how a closed number gets reused under someone still writing
/// to it.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : Fd_(fd) {}
    ~UniqueFd() { Reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : Fd_(other.Release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] int Get() const noexcept { return Fd_; }
    [[nodiscard]] bool IsOpen() const noexcept { return Fd_ >= 0; }
    explicit operator bool() const noexcept { return IsOpen(); }

    /// Gives the descriptor up without closing it.
    [[nodiscard]] int Release() noexcept { return std::exchange(Fd_, -1); }

    /// Closes what it holds, if anything, and takes `fd` instead.
    void Reset(int fd = -1) noexcept;

private:
    int Fd_{-1};
};

/// Two ends of one pipe, both close-on-exec and non-blocking.
struct Pipe {
    UniqueFd Read;
    UniqueFd Write;
};

[[nodiscard]] IoResult<Pipe> MakePipe() noexcept;

/// A pipe that closes on exec but blocks: for the ends a child gets as its
/// stdio, which it expects to block on like any other.
[[nodiscard]] IoResult<Pipe> MakeBlockingPipe() noexcept;

/// A connected pair of stream sockets, both close-on-exec and non-blocking.
struct SocketPair {
    UniqueFd First;
    UniqueFd Second;
};

[[nodiscard]] IoResult<SocketPair> MakeSocketPair() noexcept;

/// Switches `fd` to non-blocking. Everything the event loop touches is: one
/// blocking call stops every session the loop carries.
[[nodiscard]] IoResult<void> SetNonBlocking(int fd) noexcept;

/// Marks `fd` close-on-exec, so that no child the process runs inherits it.
[[nodiscard]] IoResult<void> SetCloseOnExec(int fd) noexcept;

/// Close-on-exec and non-blocking: what every descriptor the event loop
/// opens gets, set after the call that opened it on every platform.
[[nodiscard]] IoResult<void> PrepareDescriptor(int fd) noexcept;

/// One read(2), retried on EINTR. 0 is end of file; a descriptor with nothing
/// to read yet fails with an Errno whose WouldBlock() is true.
[[nodiscard]] IoResult<std::size_t> ReadSome(int fd,
                                             MutableByteSpan into) noexcept;

/// One write(2), retried on EINTR. May take less than all of `bytes`; a
/// descriptor with no room fails with WouldBlock().
[[nodiscard]] IoResult<std::size_t> WriteSome(int fd, ByteSpan bytes) noexcept;

}  // namespace zet::io
