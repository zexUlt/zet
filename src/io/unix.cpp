#include "zet/io/unix.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <string_view>

namespace zet::io {

namespace {

/// Room for more descriptors than one message may carry, so that a peer
/// sending extra ones is seen doing it and every one of them gets closed. The
/// kernel closes what does not fit at all, and says so with MSG_CTRUNC.
constexpr std::size_t RECEIVE_FD_ROOM = 4;

constexpr mode_t PERMISSION_BITS = 07777;

/// `path` as a sockaddr_un, or ENAMETOOLONG: a path that does not fit is not
/// truncated into a different one.
IoResult<sockaddr_un> AddressOf(const char* path, std::string_view call) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string_view text{path};
    if (text.empty() || text.size() >= sizeof(address.sun_path)) {
        return std::unexpected(Errno{.Code = ENAMETOOLONG, .Call = call});
    }
    std::memcpy(address.sun_path, text.data(), text.size());
    return address;
}

IoResult<UniqueFd> OpenUnixStream() noexcept {
    UniqueFd fd{
        ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return std::unexpected(LastErrno("socket"));
    }
    return fd;
}

/// CMSG_NXTHDR as a function: musl's macro compares a size with a pointer
/// difference, which -Wsign-compare refuses wherever it is expanded.
cmsghdr* NextHeader(msghdr& message, cmsghdr* header) noexcept {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
    return CMSG_NXTHDR(&message, header);
#pragma clang diagnostic pop
}

/// Closes every descriptor in a control message.
void CloseAll(msghdr& message) noexcept {
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = NextHeader(message, header)) {
        if (header->cmsg_level != SOL_SOCKET ||
            header->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        const std::size_t count =
            (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t i = 0; i < count; ++i) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(int));
            static_cast<void>(::close(fd));
        }
    }
}

}  // namespace

IoResult<UniqueFd> ListenUnix(const char* path, mode_t mode) noexcept {
    auto address = AddressOf(path, "bind");
    if (!address) {
        return std::unexpected(address.error());
    }
    auto fd = OpenUnixStream();
    if (!fd) {
        return fd;
    }
    // The file bind creates gets 0777 & ~umask: with the umask the complement
    // of `mode`, it is born with `mode` and is never anything else.
    const mode_t previous = ::umask(static_cast<mode_t>(~mode) & 0777);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const int bound =
        ::bind(fd->Get(), reinterpret_cast<const sockaddr*>(&*address),
               sizeof(*address));
    const Errno bindError = LastErrno("bind");
    ::umask(previous);
    if (bound != 0) {
        return std::unexpected(bindError);
    }
    if (::listen(fd->Get(), UNIX_LISTEN_BACKLOG) != 0) {
        return std::unexpected(LastErrno("listen"));
    }
    return fd;
}

IoResult<UniqueFd> AcceptUnix(int listener) noexcept {
    for (;;) {
        UniqueFd fd{::accept4(listener, nullptr, nullptr,
                              SOCK_NONBLOCK | SOCK_CLOEXEC)};
        if (fd) {
            return fd;
        }
        // A connection that went away while queued says ECONNABORTED; the
        // next one may be fine.
        if (errno != EINTR && errno != ECONNABORTED) {
            return std::unexpected(LastErrno("accept4"));
        }
    }
}

IoResult<UniqueFd> ConnectUnix(const char* path) noexcept {
    auto address = AddressOf(path, "connect");
    if (!address) {
        return std::unexpected(address.error());
    }
    auto fd = OpenUnixStream();
    if (!fd) {
        return fd;
    }
    for (;;) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::connect(fd->Get(), reinterpret_cast<const sockaddr*>(&*address),
                      sizeof(*address)) == 0) {
            return fd;
        }
        if (errno != EINTR) {
            return std::unexpected(LastErrno("connect"));
        }
    }
}

IoResult<PeerCredentials> PeerCredentialsOf(int fd) noexcept {
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0) {
        return std::unexpected(LastErrno("getsockopt"));
    }
    return PeerCredentials{
        .Uid = credentials.uid, .Gid = credentials.gid, .Pid = credentials.pid};
}

IoResult<void> CheckDirectory(const char* path, uid_t owner,
                              mode_t mode) noexcept {
    struct stat status{};
    if (::lstat(path, &status) != 0) {
        return std::unexpected(LastErrno("lstat"));
    }
    if (!S_ISDIR(status.st_mode)) {
        return std::unexpected(
            Errno{.Code = ENOTDIR, .Call = "CheckDirectory"});
    }
    if (status.st_uid != owner) {
        return std::unexpected(Errno{.Code = EPERM, .Call = "CheckDirectory"});
    }
    if ((status.st_mode & PERMISSION_BITS) != mode) {
        return std::unexpected(Errno{.Code = EACCES, .Call = "CheckDirectory"});
    }
    return {};
}

IoResult<std::size_t> SendWithFd(int fd, ByteSpan data, int passFd) noexcept {
    if (data.empty()) {
        return std::unexpected(Errno{.Code = EINVAL, .Call = "sendmsg"});
    }
    iovec chunk{// sendmsg does not write through it; the cast is the API's.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                .iov_base = const_cast<std::byte*>(data.data()),
                .iov_len = data.size()};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
    msghdr message{};
    message.msg_iov = &chunk;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    cmsghdr* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &passFd, sizeof(int));
    for (;;) {
        // MSG_NOSIGNAL: a mux whose agent went away hears EPIPE, not SIGPIPE.
        const ssize_t sent = ::sendmsg(fd, &message, MSG_NOSIGNAL);
        if (sent >= 0) {
            return static_cast<std::size_t>(sent);
        }
        if (errno != EINTR) {
            return std::unexpected(LastErrno("sendmsg"));
        }
    }
}

IoResult<ReceivedWithFd> ReceiveWithFd(int fd, MutableByteSpan into) noexcept {
    iovec chunk{.iov_base = into.data(), .iov_len = into.size()};
    alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * RECEIVE_FD_ROOM)>
        control{};
    msghdr message{};
    message.msg_iov = &chunk;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    ssize_t received = -1;
    for (;;) {
        received = ::recvmsg(fd, &message, MSG_CMSG_CLOEXEC);
        if (received >= 0) {
            break;
        }
        if (errno != EINTR) {
            return std::unexpected(LastErrno("recvmsg"));
        }
    }
    const Errno refused{.Code = EBADMSG, .Call = "recvmsg"};
    if ((message.msg_flags & MSG_CTRUNC) != 0) {
        CloseAll(message);
        return std::unexpected(refused);
    }
    ReceivedWithFd result{.Bytes = static_cast<std::size_t>(received),
                          .Fd = {}};
    std::size_t descriptors = 0;
    const unsigned char* first = nullptr;
    for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr;
         header = NextHeader(message, header)) {
        if (header->cmsg_level == SOL_SOCKET &&
            header->cmsg_type == SCM_RIGHTS) {
            descriptors += (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            if (first == nullptr) {
                first = CMSG_DATA(header);
            }
        }
    }
    if (descriptors > 1) {
        CloseAll(message);
        return std::unexpected(refused);
    }
    if (descriptors == 1) {
        int passed = -1;
        std::memcpy(&passed, first, sizeof(int));
        UniqueFd owned{passed};
        if (auto nonBlocking = SetNonBlocking(owned.Get()); !nonBlocking) {
            return std::unexpected(nonBlocking.error());
        }
        result.Fd = std::move(owned);
    }
    return result;
}

}  // namespace zet::io
