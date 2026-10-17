#pragma once

#include <sys/socket.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "zet/io/errno.hpp"
#include "zet/io/fd.hpp"

/// TCP for the agent's listener and the client's connection, per
/// docs/design.md §10. The only place in zet that opens a network socket:
/// tools/check_no_telemetry.sh allows outbound primitives in zet_io alone.
namespace zet::io {

/// How many connections the kernel queues before the agent accepts them.
/// The agent carries one session (§3) and its client reconnects one at a
/// time; the margin is for half-open attempts a stranger leaves behind.
inline constexpr int LISTEN_BACKLOG = 16;

/// How many addresses Resolve keeps. A host with more is reached through
/// the first ones; nobody needs the eighth AAAA record of a laptop's peer.
inline constexpr std::size_t MAX_RESOLVED_ADDRESSES = 8;

/// One socket address of either family, as getaddrinfo and accept return it.
struct SocketAddress {
    sockaddr_storage Storage{};
    socklen_t Length{0};

    /// The port, in host order; 0 for a family without one.
    [[nodiscard]] std::uint16_t Port() const noexcept;
};

/// A listening socket and the port the kernel gave it.
struct Listener {
    UniqueFd Socket;
    std::uint16_t Port{0};
};

/// Listens on `port` of every address, 0 for one the kernel picks.
///
/// One dual-stack IPv6 socket with IPv4 mapped into it, so a client reaches
/// the agent over either family on the same port; a host without IPv6 gets
/// a plain IPv4 socket instead. Non-blocking and close-on-exec, like
/// everything the loop watches.
[[nodiscard]] IoResult<Listener> ListenTcp(std::uint16_t port = 0) noexcept;

/// One pending connection, non-blocking, close-on-exec and with TCP_NODELAY.
/// None pending fails with WouldBlock().
[[nodiscard]] IoResult<UniqueFd> Accept(int listener) noexcept;

/// What Resolve found, in the order getaddrinfo gave it.
struct AddressList {
    std::array<SocketAddress, MAX_RESOLVED_ADDRESSES> Items{};
    std::size_t Count{0};

    [[nodiscard]] std::span<const SocketAddress> View() const noexcept {
        return std::span{Items}.first(Count);
    }
};

/// The stream addresses of `host`, each with `port` filled in.
///
/// Blocking: getaddrinfo has no non-blocking form. Called before the event
/// loop starts, never from it; a resolver worker would take it over if a
/// reconnect ever needed a fresh lookup (§10). A failure carries the EAI_*
/// code with Call "getaddrinfo", and Description() says what it means.
[[nodiscard]] IoResult<AddressList> Resolve(const char* host,
                                            std::uint16_t port) noexcept;

/// Starts connecting to `address` without waiting. The socket comes back at
/// once, non-blocking and with TCP_NODELAY; watch it for writing and call
/// FinishConnect when it is ready.
[[nodiscard]] IoResult<UniqueFd> StartConnect(
    const SocketAddress& address) noexcept;

/// Whether the connect StartConnect began went through. A refusal or a
/// timeout comes back as an Errno with Call "connect".
[[nodiscard]] IoResult<void> FinishConnect(int socket) noexcept;

}  // namespace zet::io
