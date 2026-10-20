#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <variant>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/message.hpp"

/// What zet-muxd decides, sans-I/O (design.md §6): which agent registered
/// which sid, and where each new TCP connection goes once its Hello is in.
///
/// The I/O layer owns the sockets and numbers them; the router sees the
/// numbers, the bytes it is fed and the time it is told, and answers with
/// decisions. It never sees a key of a session: a registration's R is hashed
/// to its sid on the way in and not kept, and of a connection it reads only
/// what PeekHello reads.
namespace zet::session {

/// An agent's connection to mux.sock, as the I/O layer numbers it.
using MuxAgentId = std::uint64_t;
/// A TCP connection to the mux, as the I/O layer numbers it.
using MuxConnectionId = std::uint64_t;
/// The uid SO_PEERCRED gave for an agent's connection.
using Uid = std::uint32_t;
/// A TCP peer's address: IPv6, or IPv4 mapped into it. What the per-address
/// limit counts by.
using PeerAddress = std::array<std::byte, 16>;

/// Sessions on one machine at once: well past the users of a shared host.
inline constexpr std::size_t MAX_MUX_REGISTRATIONS = 1024;
/// Sessions of one user at once. Enough for the terminals anyone keeps open,
/// and what keeps one user from filling the table for everybody.
inline constexpr std::size_t MAX_REGISTRATIONS_PER_UID = 32;
/// Connections waiting for their Hello, all peers together. Each holds at
/// most a handshake frame, so this bounds the router's buffers too.
inline constexpr std::size_t MAX_PENDING_CONNECTIONS = 256;
/// Of those, from one address: a peer opening connections it never speaks on
/// fills its own share and nobody else's.
inline constexpr std::size_t MAX_PENDING_PER_ADDRESS = 16;
/// What one waiting connection may have buffered: its Hello frame, whole.
inline constexpr std::size_t MUX_PENDING_BUFFER = wire::MAX_HANDOFF_PREFIX;
/// The storage MuxRouter borrows for the waiting connections.
inline constexpr std::size_t MUX_PENDING_STORAGE =
    MAX_PENDING_CONNECTIONS * MUX_PENDING_BUFFER;

enum class EMuxClose : std::uint8_t {
    /// Not a cleartext Hello, or longer than a handshake frame may be.
    Malformed,
    /// No Hello within HANDSHAKE_TIMEOUT.
    Expired,
    /// A pending-connection limit is reached.
    Limit,
    /// A number the router does not know. A defect of the caller.
    Unknown,
};

/// Give the connection to `Agent`: send `Prefix` and the descriptor in one
/// MuxHandoff, then close the mux's copy.
struct MuxHandoff {
    MuxConnectionId Connection{0};
    MuxAgentId Agent{0};
    ByteSpan Prefix;
};

/// No live registration has the sid: run the decoy handshake of §5 rule two
/// on the connection, fed `Prefix` first.
struct MuxDecoy {
    MuxConnectionId Connection{0};
    ByteSpan Prefix;
};

struct MuxClose {
    MuxConnectionId Connection{0};
    EMuxClose Reason{EMuxClose::Malformed};
};

/// The Hello is not all in yet.
struct MuxNeedMore {};

/// What Feed decided. Every alternative but MuxNeedMore ends the router's
/// interest in the connection: it is no longer pending.
using MuxDecision = std::variant<MuxNeedMore, MuxHandoff, MuxDecoy, MuxClose>;

class MuxRouter {
public:
    /// `pendingStorage` holds MUX_PENDING_STORAGE bytes and is borrowed for
    /// the router's life; less is fewer connections pending at once. The router
    /// itself is tens of kilobytes of fixed tables: give it a home off the
    /// stack.
    explicit MuxRouter(MutableByteSpan pendingStorage) noexcept;

    MuxRouter(const MuxRouter&) = delete;
    MuxRouter& operator=(const MuxRouter&) = delete;
    MuxRouter(MuxRouter&&) = delete;
    MuxRouter& operator=(MuxRouter&&) = delete;
    ~MuxRouter() = default;

    /// Claims the sid `registration` hashes to for `agent`, as uid `uid`.
    ///
    /// The same sid again from the same uid moves the claim to `agent`: an
    /// agent that reconnected to the mux before the mux noticed its old
    /// connection go. From another uid it is Taken while the first lives.
    [[nodiscard]] std::expected<void, wire::EMuxRefusal> Register(
        MuxAgentId agent, Uid uid, const crypto::Key& registration) noexcept;

    /// The agent's connection is gone: every sid it held is free again.
    void Unregister(MuxAgentId agent) noexcept;

    /// The agent holding `id`, if any. What the router routes by.
    [[nodiscard]] std::optional<MuxAgentId> Find(
        const wire::SessionId& id) const noexcept;

    [[nodiscard]] std::size_t Registrations() const noexcept {
        return RegistrationCount_;
    }

    /// A TCP connection was accepted. Refused, and to be closed at once, when
    /// a pending limit is reached.
    [[nodiscard]] std::expected<void, EMuxClose> Accept(
        MuxConnectionId connection, const PeerAddress& from,
        Timestamp now) noexcept;

    /// How many more bytes Feed takes for `connection`: read at most this.
    [[nodiscard]] std::size_t Room(MuxConnectionId connection) const noexcept;

    /// Bytes read from `connection`. A decision's Prefix points into the
    /// router's storage and is good until the next Accept.
    [[nodiscard]] MuxDecision Feed(MuxConnectionId connection,
                                   ByteSpan bytes) noexcept;

    /// The peer went away before its Hello was in.
    void Closed(MuxConnectionId connection) noexcept;

    /// The earliest deadline of a pending connection; unset with none.
    [[nodiscard]] Deadline NextDeadline() const noexcept;

    /// One pending connection whose deadline has passed, now closed by the
    /// router; nullopt when there is none. Call until nullopt.
    [[nodiscard]] std::optional<MuxClose> Expire(Timestamp now) noexcept;

    [[nodiscard]] std::size_t Pending() const noexcept { return PendingCount_; }

private:
    struct Registration {
        wire::SessionId Id{};
        MuxAgentId Agent{0};
        Uid Owner{0};
        bool Live{false};
    };

    struct Waiting {
        MuxConnectionId Connection{0};
        PeerAddress From{};
        Timestamp Until;
        std::size_t Length{0};
        bool Live{false};
    };

    [[nodiscard]] Waiting* FindWaiting(MuxConnectionId connection) noexcept;
    [[nodiscard]] const Waiting* FindWaiting(
        MuxConnectionId connection) const noexcept;
    [[nodiscard]] MutableByteSpan BufferOf(const Waiting& waiting) noexcept;
    void Release(Waiting& waiting) noexcept;

    MutableByteSpan Storage_;
    std::size_t Slots_;
    std::array<Registration, MAX_MUX_REGISTRATIONS> Registrations_{};
    std::size_t RegistrationCount_{0};
    std::array<Waiting, MAX_PENDING_CONNECTIONS> Waiting_{};
    std::size_t PendingCount_{0};
};

}  // namespace zet::session
