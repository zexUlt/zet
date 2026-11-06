/// The real agent's replay of output after a reconnect (design.md §9),
/// driven through pump_test_api.hpp with a real pty and a real
/// ClientConnection over TCP.
///
/// While the client is away the output ring grows past ResendMax, up to
/// DisconnectMax. The agent's per-connection output queue is ResendMax plus a
/// fixed slack, so the replay is larger than the queue. The replay has to go
/// out as the socket drains; a connection that is merely behind is not "too
/// far behind".
///
/// Kills: the send gate in Agent::Pump removed (the whole replay is appended
/// to the connection's queue in one turn, which refuses it and closes the
/// connection, on every reconnect, forever).

#include "../../src/agent/pump_test_api.hpp"

#include "../session/grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <poll.h>
#include <termios.h>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/pty.hpp"
#include "zet/io/tcp.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"

namespace {

using namespace zet::agent;
using namespace std::chrono_literals;

constexpr std::size_t RESEND = 65536;
constexpr std::size_t DISCONNECT = 3U * 1024U * 1024U;
// More than the agent's connection queue holds (RESEND + 1 MiB of slack),
// less than the ring.
constexpr std::size_t OUTPUT = 2U * 1024U * 1024U + 512U * 1024U;

std::vector<std::byte> Sequence(std::size_t count) {
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::byte>(i * 131 % 251);
    }
    return bytes;
}

io::UniqueFd TcpConnect(std::uint16_t port) {
    auto addrs = io::Resolve("127.0.0.1", port);
    if (!addrs || addrs->Count == 0) {
        return io::UniqueFd{};
    }
    auto fd = io::StartConnect(addrs->Items[0]);
    if (!fd) {
        return io::UniqueFd{};
    }
    pollfd pf{.fd = fd->Get(), .events = POLLOUT, .revents = 0};
    if (::poll(&pf, 1, 3000) <= 0 || !io::FinishConnect(fd->Get())) {
        return io::UniqueFd{};
    }
    return std::move(*fd);
}

/// One ClientConnection over TCP; keeps what the agent delivered.
class Peer {
public:
    Peer(session::ClientSession& session, std::uint64_t nonce,
         io::UniqueFd sock)
        : Sock_(std::move(sock)),
          Cc_(session, MakeNonce(nonce), MutableByteSpan{Frames_},
              io::MonotonicNow(),
              /*capable=*/true, wire::PROTOCOL_VERSION_GRID,
              MutableByteSpan{SnapBuf_}) {}

    void Step() {
        pollfd pf{.fd = Sock_.Get(), .events = POLLIN, .revents = 0};
        ::poll(&pf, 1, 5);
        // No more than the frame layer takes: a whole frame is worked through
        // by Pump before the next read.
        const std::size_t room = std::min<std::size_t>(Cc_.ReadRoom(), 65536);
        if (room > 0 && (pf.revents & (POLLIN | POLLHUP))) {
            std::array<std::byte, 65536> buf{};
            auto n =
                io::ReadSome(Sock_.Get(), MutableByteSpan{buf}.first(room));
            if (n && *n > 0) {
                std::ignore = Cc_.OnBytes(ByteSpan{buf}.first(*n));
            } else if ((!n && !n.error().WouldBlock()) || (n && *n == 0)) {
                Closed = true;
            }
        }
        std::vector<std::byte> outgoing;
        for (;;) {
            auto r = Cc_.Pump(io::MonotonicNow(), MutableByteSpan{Out_},
                              MutableByteSpan{Scratch_});
            if (!r) {
                Failed = true;
                break;
            }
            if (const auto* sf = std::get_if<session::SendFrame>(&*r)) {
                outgoing.insert(
                    outgoing.end(), Out_.begin(),
                    Out_.begin() + static_cast<std::ptrdiff_t>(sf->Written));
                continue;
            }
            if (const auto* d = std::get_if<session::Deliver>(&*r)) {
                Got.insert(Got.end(), d->Bytes.begin(), d->Bytes.end());
                continue;
            }
            if (std::holds_alternative<session::Resized>(*r)) {
                continue;
            }
            break;
        }
        ByteSpan bytes{outgoing};
        while (!bytes.empty()) {
            auto w = io::WriteSome(Sock_.Get(), bytes);
            if (!w) {
                if (w.error().WouldBlock()) {
                    pollfd wpf{
                        .fd = Sock_.Get(), .events = POLLOUT, .revents = 0};
                    ::poll(&wpf, 1, 10);
                    continue;
                }
                Closed = true;
                return;
            }
            bytes = bytes.subspan(*w);
        }
    }

    [[nodiscard]] bool Ended() const { return Closed || Failed; }

    bool Closed{false};
    bool Failed{false};
    std::vector<std::byte> Got;

private:
    io::UniqueFd Sock_;
    std::array<std::byte, session::CONNECTION_FRAME_STORAGE> Frames_{};
    std::array<std::byte, session::CONNECTION_OUT_SIZE> Out_{};
    std::array<std::byte, session::CONNECTION_SCRATCH_SIZE> Scratch_{};
    std::vector<std::byte> SnapBuf_ =
        std::vector<std::byte>(wire::SNAPSHOT_MAX_BYTES);
    session::ClientConnection Cc_;
};

}  // namespace

// The shell writes more than the connection queue holds while no client is
// attached; the client then connects once. Every byte arrives, in order, on
// that one connection.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a replay larger than the connection's output queue "
                  "is paced by the socket") {
    auto limits = TinyLimits();
    limits.ResendMax = RESEND;
    limits.DisconnectMax = DISCONNECT;

    auto pty = io::OpenPty();
    REQUIRE(pty.has_value());
    termios t{};
    REQUIRE(::tcgetattr(pty->Slave.Get(), &t) == 0);
    ::cfmakeraw(&t);
    REQUIRE(::tcsetattr(pty->Slave.Get(), TCSANOW, &t) == 0);
    auto listener = io::ListenTcp(0);
    REQUIRE(listener.has_value());
    const std::uint16_t port = listener->Port;

    std::atomic<bool> stop{false};
    std::thread agent{
        [&, l = std::move(*listener), m = std::move(pty->Master)]() mutable {
            std::ignore = RunAgentForTest(AgentTestConfig{
                .Secrets = DeriveSessionSecrets(MakeKey(92)),
                .Limits = limits,
                .Listen = std::move(l),
                .PtyReadFd = std::move(m),
                .Ceiling = wire::PROTOCOL_VERSION_GRID,
                .GiveUpAfter = std::chrono::seconds{120},
                .Inspect = nullptr,
                .Stop = &stop,
            });
        }};

    std::vector<std::byte> clientStorage(DISCONNECT + 256);
    session::ClientSession client{
        DeriveSessionSecrets(MakeKey(92)),
        session::SessionStreams{.Storage = MutableByteSpan{clientStorage},
                                .Now = io::MonotonicNow(),
                                .Limits = limits}};

    const auto output = Sequence(OUTPUT);
    ByteSpan rest{output};
    while (!rest.empty()) {
        auto w = io::WriteSome(pty->Slave.Get(), rest);
        if (w) {
            rest = rest.subspan(*w);
        } else {
            REQUIRE(w.error().WouldBlock());
            std::this_thread::sleep_for(1ms);
        }
    }
    std::this_thread::sleep_for(500ms);  // the agent has read it all

    int connections = 1;
    auto peer = std::make_unique<Peer>(client, 1, TcpConnect(port));
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    std::vector<std::byte> got;
    while (got.size() + peer->Got.size() < output.size() &&
           std::chrono::steady_clock::now() < deadline) {
        if (peer->Ended()) {
            got.insert(got.end(), peer->Got.begin(), peer->Got.end());
            ++connections;
            peer =
                std::make_unique<Peer>(client, connections, TcpConnect(port));
        }
        peer->Step();
    }
    got.insert(got.end(), peer->Got.begin(), peer->Got.end());

    stop = true;
    std::ignore = TcpConnect(port);
    agent.join();

    CHECK(connections == 1);
    CHECK(got.size() == output.size());
    CHECK(got == output);
}
