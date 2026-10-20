#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zet/app/byte_queue.hpp"
#include "zet/app/mux_frames.hpp"
#include "zet/app/registration.hpp"
#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/poller.hpp"

namespace zet::agent {

/// Where zet-muxd takes registrations unless ZET_MUX_SOCKET says otherwise.
inline constexpr std::string_view DEFAULT_MUX_SOCKET = "/run/zet/mux.sock";

/// ZET_MUX_SOCKET, or the default.
[[nodiscard]] std::string MuxSocketPath();

/// Whether there is a socket at `path`: what makes the agent try the mux at
/// all (§6). Without one it listens itself, as in M4.
[[nodiscard]] bool MuxSocketExists(const std::string& path) noexcept;

/// A client connection the mux has passed over, with the bytes it read off
/// the connection before deciding — they come before anything on the socket.
struct HandedOver {
    io::UniqueFd Socket;
    std::vector<std::byte> Prefix;
};

/// The agent's registration with zet-muxd (§6): one unix connection that
/// carries MuxRegister out and MuxRegistered and handoffs in.
///
/// What the agent trusts: the process on the other end of the socket at the
/// path, provided that it runs as the uid that owns the socket file, and
/// that the directory holding it can be written by nobody but that uid and
/// root. Whoever can plant a socket there could be the mux anyway; whoever
/// cannot is refused. Given that, the mux learns R — which claims the sid
/// and nothing else — and is trusted to route; the handshake on each
/// handed-off connection is still the agent's own.
///
/// When the connection is lost the link comes back with the client's
/// backoff (§9) and registers again. The connections it handed over do not
/// depend on it.
class MuxLink {
public:
    MuxLink(std::string path, crypto::Key registration,
            std::uint64_t token) noexcept;

    /// Connects, checks the peer, registers, and waits for the answer,
    /// blocking until `until`: the bootstrap, before the loop exists, where
    /// the Accept cannot go out without the port. Returns the mux's port.
    [[nodiscard]] std::expected<std::uint16_t, std::string_view> RegisterNow(
        Timestamp until) noexcept;

    /// The port the mux announced at bootstrap.
    [[nodiscard]] std::uint16_t Port() const noexcept { return Port_; }

    /// Reads or writes what the poller says is ready; handoffs land in `out`.
    void OnEvent(const io::PollEvent& event, io::Poller& poller, Timestamp now,
                 std::vector<HandedOver>& out) noexcept;

    /// Reconnects when the backoff says so.
    void OnTimer(io::Poller& poller, Timestamp now) noexcept;

    [[nodiscard]] bool UpdateInterest(io::Poller& poller) noexcept;

    [[nodiscard]] Deadline NextDeadline() const noexcept;

    /// Stops watching and closes, for good.
    void Close(io::Poller& poller) noexcept;

private:
    [[nodiscard]] std::expected<void, std::string_view> Connect() noexcept;
    void Lose(io::Poller& poller, Timestamp now, std::string_view why) noexcept;
    void Read(io::Poller& poller, Timestamp now,
              std::vector<HandedOver>& out) noexcept;
    void Write(io::Poller& poller, Timestamp now) noexcept;
    /// Takes one message; false if it breaks the protocol.
    [[nodiscard]] bool Take(ByteSpan body,
                            std::vector<HandedOver>& out) noexcept;
    [[nodiscard]] Duration Backoff() noexcept;

    std::string Path_;
    crypto::Key Registration_;
    std::uint64_t Token_;
    std::uint16_t Port_{0};

    io::UniqueFd Socket_;
    std::optional<app::Registration> Watch_;
    app::MuxFrameReader Reader_;
    app::ByteQueue Output_;
    /// Descriptors received ahead of the MuxHandoff they go with.
    std::deque<io::UniqueFd> Descriptors_;
    bool Registered_{false};

    Deadline RetryAt_;
    unsigned Attempts_{0};
};

}  // namespace zet::agent
