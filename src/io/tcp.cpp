#include "zet/io/tcp.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace zet::io {

namespace {

constexpr int ENABLED = 1;
constexpr int DISABLED = 0;

/// Interactive traffic is keystrokes: Nagle would hold each one back for
/// the ack of the previous.
IoResult<void> SetNoDelay(int fd) noexcept {
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &ENABLED, sizeof(ENABLED)) !=
        0) {
        return std::unexpected(LastErrno("setsockopt"));
    }
    return {};
}

IoResult<UniqueFd> OpenStream(int family) noexcept {
    UniqueFd fd{
        ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return std::unexpected(LastErrno("socket"));
    }
    return fd;
}

IoResult<Listener> ListenOn(int family, std::uint16_t port) noexcept {
    auto fd = OpenStream(family);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    // A restarted agent can bind its old port while the previous
    // connections sit in TIME_WAIT.
    if (::setsockopt(fd->Get(), SOL_SOCKET, SO_REUSEADDR, &ENABLED,
                     sizeof(ENABLED)) != 0) {
        return std::unexpected(LastErrno("setsockopt"));
    }
    sockaddr_storage storage{};
    socklen_t length = 0;
    if (family == AF_INET6) {
        // Dual stack whatever net.ipv6.bindv6only says.
        if (::setsockopt(fd->Get(), IPPROTO_IPV6, IPV6_V6ONLY, &DISABLED,
                         sizeof(DISABLED)) != 0) {
            return std::unexpected(LastErrno("setsockopt"));
        }
        sockaddr_in6 any{};
        any.sin6_family = AF_INET6;
        any.sin6_addr = in6addr_any;
        any.sin6_port = htons(port);
        std::memcpy(&storage, &any, sizeof(any));
        length = sizeof(any);
    } else {
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port = htons(port);
        std::memcpy(&storage, &any, sizeof(any));
        length = sizeof(any);
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::bind(fd->Get(), reinterpret_cast<const sockaddr*>(&storage),
               length) != 0) {
        return std::unexpected(LastErrno("bind"));
    }
    if (::listen(fd->Get(), LISTEN_BACKLOG) != 0) {
        return std::unexpected(LastErrno("listen"));
    }
    SocketAddress bound{};
    bound.Length = sizeof(bound.Storage);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    if (::getsockname(fd->Get(), reinterpret_cast<sockaddr*>(&bound.Storage),
                      &bound.Length) != 0) {
        return std::unexpected(LastErrno("getsockname"));
    }
    return Listener{.Socket = std::move(*fd), .Port = bound.Port()};
}

}  // namespace

std::uint16_t SocketAddress::Port() const noexcept {
    if (Storage.ss_family == AF_INET6) {
        sockaddr_in6 v6{};
        std::memcpy(&v6, &Storage, sizeof(v6));
        return ntohs(v6.sin6_port);
    }
    if (Storage.ss_family == AF_INET) {
        sockaddr_in v4{};
        std::memcpy(&v4, &Storage, sizeof(v4));
        return ntohs(v4.sin_port);
    }
    return 0;
}

IoResult<Listener> ListenTcp(std::uint16_t port) noexcept {
    auto dual = ListenOn(AF_INET6, port);
    if (dual) {
        return dual;
    }
    // A kernel built without IPv6, or one with it disabled at boot.
    if (dual.error().Code == EAFNOSUPPORT ||
        dual.error().Code == EADDRNOTAVAIL) {
        return ListenOn(AF_INET, port);
    }
    return dual;
}

IoResult<UniqueFd> Accept(int listener) noexcept {
    for (;;) {
        UniqueFd fd{::accept4(listener, nullptr, nullptr,
                              SOCK_NONBLOCK | SOCK_CLOEXEC)};
        if (fd) {
            if (auto noDelay = SetNoDelay(fd.Get()); !noDelay) {
                return std::unexpected(noDelay.error());
            }
            return fd;
        }
        // A connection that was reset while it waited in the queue is not the
        // listener's failure: the next one may be fine.
        if (errno != EINTR && errno != ECONNABORTED) {
            return std::unexpected(LastErrno("accept4"));
        }
    }
}

IoResult<AddressList> Resolve(const char* host, std::uint16_t port) noexcept {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    // Only families this host can actually reach.
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo* found = nullptr;
    const int status = ::getaddrinfo(host, nullptr, &hints, &found);
    if (status != 0) {
        if (status == EAI_SYSTEM) {
            return std::unexpected(LastErrno("getaddrinfo"));
        }
        return std::unexpected(Errno{.Code = status, .Call = "getaddrinfo"});
    }
    AddressList list;
    for (const addrinfo* entry = found;
         entry != nullptr && list.Count < MAX_RESOLVED_ADDRESSES;
         entry = entry->ai_next) {
        if (entry->ai_addrlen > sizeof(sockaddr_storage)) {
            continue;
        }
        SocketAddress& address = list.Items[list.Count];
        std::memcpy(&address.Storage, entry->ai_addr, entry->ai_addrlen);
        address.Length = entry->ai_addrlen;
        if (entry->ai_family == AF_INET6) {
            sockaddr_in6 v6{};
            std::memcpy(&v6, &address.Storage, sizeof(v6));
            v6.sin6_port = htons(port);
            std::memcpy(&address.Storage, &v6, sizeof(v6));
        } else if (entry->ai_family == AF_INET) {
            sockaddr_in v4{};
            std::memcpy(&v4, &address.Storage, sizeof(v4));
            v4.sin_port = htons(port);
            std::memcpy(&address.Storage, &v4, sizeof(v4));
        } else {
            continue;
        }
        ++list.Count;
    }
    ::freeaddrinfo(found);
    if (list.Count == 0) {
        return std::unexpected(
            Errno{.Code = EAI_NONAME, .Call = "getaddrinfo"});
    }
    return list;
}

IoResult<UniqueFd> StartConnect(const SocketAddress& address) noexcept {
    auto fd = OpenStream(address.Storage.ss_family);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    if (auto noDelay = SetNoDelay(fd->Get()); !noDelay) {
        return std::unexpected(noDelay.error());
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* target = reinterpret_cast<const sockaddr*>(&address.Storage);
    // An interrupted connect carries on in the background, the same as one
    // that returned EINPROGRESS; calling it again would only say EALREADY.
    if (::connect(fd->Get(), target, address.Length) == 0 ||
        errno == EINPROGRESS || errno == EINTR) {
        return std::move(*fd);
    }
    return std::unexpected(LastErrno("connect"));
}

IoResult<void> FinishConnect(int socket) noexcept {
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return std::unexpected(LastErrno("getsockopt"));
    }
    if (error != 0) {
        return std::unexpected(Errno{.Code = error, .Call = "connect"});
    }
    return {};
}

}  // namespace zet::io
