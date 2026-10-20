#pragma once

#include <sys/types.h>

#include <cstddef>
#include <optional>

#include "zet/core/bytes.hpp"
#include "zet/io/errno.hpp"
#include "zet/io/fd.hpp"

/// Local sockets between zet-muxd and the agents, per docs/design.md §6:
/// listening on a path with the right mode from the first instant, knowing
/// who is on the other end, and handing a descriptor across. Linux only, as
/// the mux and the agent are (§12).
namespace zet::io {

/// How many connecting agents the kernel queues before the mux accepts them.
inline constexpr int UNIX_LISTEN_BACKLOG = 64;

/// Listens on `path` with `mode` (e.g. 0666) from the moment it exists.
///
/// The umask is set around bind rather than the mode fixed by chmod after
/// it: in between, the socket would exist with whatever the process umask
/// allowed, which is the race dtach has (master.c:221). An existing file at
/// `path` is an error (EADDRINUSE); removing a stale one is the caller's
/// decision. Non-blocking and close-on-exec.
[[nodiscard]] IoResult<UniqueFd> ListenUnix(const char* path,
                                            mode_t mode) noexcept;

/// One pending connection, non-blocking and close-on-exec; none pending
/// fails with WouldBlock().
[[nodiscard]] IoResult<UniqueFd> AcceptUnix(int listener) noexcept;

/// Connects to `path`, non-blocking and close-on-exec. A local stream
/// connect is never in progress: it is done or refused at once, and a full
/// backlog is EAGAIN rather than EINPROGRESS.
[[nodiscard]] IoResult<UniqueFd> ConnectUnix(const char* path) noexcept;

/// Who is at the other end of a local socket, as the kernel recorded it at
/// connect: never what the peer says about itself (§6).
struct PeerCredentials {
    uid_t Uid{0};
    gid_t Gid{0};
    pid_t Pid{0};
};

[[nodiscard]] IoResult<PeerCredentials> PeerCredentialsOf(int fd) noexcept;

/// Checks that `path` is a directory itself — not a symlink to one — owned
/// by `owner` and with exactly `mode` in its permission bits, the way tmux
/// (tmux.c:229-246) and abduco (abduco.c:279-352) check theirs. A mismatch
/// fails with ENOTDIR, EPERM for the owner or EACCES for the mode, Call
/// "CheckDirectory"; an lstat failure keeps its own errno and Call "lstat".
[[nodiscard]] IoResult<void> CheckDirectory(const char* path, uid_t owner,
                                            mode_t mode) noexcept;

/// Sends `data` with `passFd` attached to its first byte (SCM_RIGHTS).
///
/// `data` must not be empty: on a stream socket the descriptor rides with a
/// byte, and without one there is nothing to carry it. Returns how many bytes
/// went: fewer than all is possible, the descriptor went with the first, and
/// the rest is sent with WriteSome. A full socket fails with WouldBlock()
/// and sends neither.
[[nodiscard]] IoResult<std::size_t> SendWithFd(int fd, ByteSpan data,
                                               int passFd) noexcept;

/// What one ReceiveWithFd took: bytes, and the descriptor that rode with them
/// if one did — close-on-exec and non-blocking already.
struct ReceivedWithFd {
    std::size_t Bytes{0};
    std::optional<UniqueFd> Fd;
};

/// One recvmsg, retried on EINTR. 0 bytes and no descriptor is end of file.
///
/// At most one descriptor per message is accepted. More than one, or control
/// data the kernel had to truncate (MSG_CTRUNC), closes every descriptor that
/// did arrive and fails with EBADMSG, Call "recvmsg": a peer that sends what
/// the protocol does not allow gets nothing kept open on its behalf.
[[nodiscard]] IoResult<ReceivedWithFd> ReceiveWithFd(
    int fd, MutableByteSpan into) noexcept;

}  // namespace zet::io
