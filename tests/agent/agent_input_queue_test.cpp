/// The real agent's queue of input for the pty (PtyInput_ in agent.cpp,
/// design.md §9), driven through pump_test_api.hpp with a real pty and real
/// ClientConnections.
///
/// tests/session/agent_input_overrun_test.cpp covers the session arithmetic
/// with a model of the queue; these tests cover the agent's own lines, each
/// aimed at one mutation checked by hand:
///
///   Session_.InputRejected(...) removed from Agent::Pump
///       "Agent: a refused input frame is resent and reaches the pty once"
///   PtyInput_ sized ResendMax instead of max(ResendMax, DisconnectMax)
///       "Agent: a replay up to DisconnectMax fits the queue while the pty
///        does not drain"
///
/// The pty is not read while the client replays. The kernel keeps some of what
/// the agent writes (measured here, it differs between kernels); the rest
/// waits in the agent's queue, which is what is under test.

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

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

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

std::vector<std::byte> Sequence(std::size_t count) {
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::byte>(i * 131 % 251);
    }
    return bytes;
}

void MakeRaw(int slave) {
    termios t{};
    REQUIRE(::tcgetattr(slave, &t) == 0);
    ::cfmakeraw(&t);
    REQUIRE(::tcsetattr(slave, TCSANOW, &t) == 0);
}

/// How much a pty takes from its master while nobody reads the slave.
std::size_t PtyCapacity() {
    auto pty = io::OpenPty();
    REQUIRE(pty.has_value());
    MakeRaw(pty->Slave.Get());
    REQUIRE(io::SetNonBlocking(pty->Master.Get()).has_value());
    const std::vector<std::byte> block(4096, std::byte{'x'});
    std::size_t total = 0;
    for (;;) {
        auto w = io::WriteSome(pty->Master.Get(), block);
        if (!w) {
            REQUIRE(w.error().WouldBlock());
            return total;
        }
        total += *w;
    }
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

/// One ClientConnection over TCP to the agent; ends when the agent closes it.
class Peer {
public:
    Peer(session::ClientSession& session, std::uint64_t nonce,
         io::UniqueFd sock)
        : Sock_(std::move(sock)),
          Cc_(session, MakeNonce(nonce), MutableByteSpan{Frames_},
              io::MonotonicNow(),
              /*capable=*/true, wire::PROTOCOL_VERSION_GRID,
              MutableByteSpan{SnapBuf_}) {}

    /// One turn: read what the agent sent, write what the session has.
    void Step() {
        pollfd pf{.fd = Sock_.Get(), .events = POLLIN, .revents = 0};
        ::poll(&pf, 1, 5);
        if (pf.revents & (POLLIN | POLLHUP)) {
            std::array<std::byte, 65536> buf{};
            auto n = io::ReadSome(Sock_.Get(), MutableByteSpan{buf});
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
            if (const auto* as = std::get_if<session::ApplySnapshot>(&*r)) {
                Cc_.ConfirmApply(as->StreamGen);
                continue;
            }
            if (std::holds_alternative<session::Deliver>(*r)) {
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

private:
    io::UniqueFd Sock_;
    std::array<std::byte, session::CONNECTION_FRAME_STORAGE> Frames_{};
    std::array<std::byte, session::CONNECTION_OUT_SIZE> Out_{};
    std::array<std::byte, session::CONNECTION_SCRATCH_SIZE> Scratch_{};
    std::vector<std::byte> SnapBuf_ =
        std::vector<std::byte>(wire::SNAPSHOT_MAX_BYTES);
    session::ClientConnection Cc_;
};

/// The real agent, a client session with its own limits, and the slave end of
/// the pty the agent writes to. The test is the shell: it reads the slave when
/// it wants the pty to drain.
class AgentRig {
public:
    AgentRig(const session::SessionLimits& agentLimits,
             const session::SessionLimits& clientLimits)
        : ClientStorage_(
              std::max(clientLimits.ResendMax, clientLimits.DisconnectMax) +
              256),
          Client_(DeriveSessionSecrets(MakeKey(91)),
                  session::SessionStreams{
                      .Storage = MutableByteSpan{ClientStorage_},
                      .Now = io::MonotonicNow(),
                      .Limits = clientLimits}) {
        auto pty = io::OpenPty();
        REQUIRE(pty.has_value());
        Slave_ = std::move(pty->Slave);
        MakeRaw(Slave_.Get());
        REQUIRE(io::SetNonBlocking(Slave_.Get()).has_value());
        auto listener = io::ListenTcp(0);
        REQUIRE(listener.has_value());
        Port_ = listener->Port;
        Thread_ = std::thread([this, agentLimits, l = std::move(*listener),
                               m = std::move(pty->Master)]() mutable {
            std::ignore = RunAgentForTest(AgentTestConfig{
                .Secrets = DeriveSessionSecrets(MakeKey(91)),
                .Limits = agentLimits,
                .Listen = std::move(l),
                .PtyReadFd = std::move(m),
                .Ceiling = wire::PROTOCOL_VERSION_GRID,
                .GiveUpAfter = std::chrono::seconds{120},
                .Inspect = nullptr,
                .Stop = &Stop_,
                .InputToPty = true,
            });
        });
    }

    AgentRig(const AgentRig&) = delete;
    AgentRig& operator=(const AgentRig&) = delete;

    ~AgentRig() {
        Stop_ = true;
        std::ignore = TcpConnect(Port_);
        Thread_.join();
    }

    session::ClientSession& Client() { return Client_; }

    std::unique_ptr<Peer> Connect() {
        auto sock = TcpConnect(Port_);
        REQUIRE(sock.Get() != -1);
        return std::make_unique<Peer>(Client_, ++Nonces_, std::move(sock));
    }

    /// What the shell has read since the last call.
    void DrainPty(std::vector<std::byte>& into) {
        std::array<std::byte, 65536> buf{};
        for (;;) {
            auto n = io::ReadSome(Slave_.Get(), MutableByteSpan{buf});
            if (!n || *n == 0) {
                return;
            }
            into.insert(into.end(), buf.begin(),
                        buf.begin() + static_cast<std::ptrdiff_t>(*n));
        }
    }

private:
    std::vector<std::byte> ClientStorage_;
    session::ClientSession Client_;
    io::UniqueFd Slave_;
    std::uint16_t Port_{0};
    std::uint64_t Nonces_{0};
    std::atomic<bool> Stop_{false};
    std::thread Thread_;
};

session::SessionLimits Limits(std::size_t resend, std::size_t disconnect) {
    auto l = TinyLimits();
    l.ResendMax = resend;
    l.DisconnectMax = disconnect;
    return l;
}

/// Runs `peer` for `ms`, or until it ends.
void RunFor(Peer& peer, int ms) {
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{ms};
    while (!peer.Ended() && std::chrono::steady_clock::now() < end) {
        peer.Step();
    }
}

/// The shell starts reading: reconnects whenever the agent closes the
/// connection, and returns what reached the pty once `want` bytes did, or
/// when the deadline passes.
std::vector<std::byte> DrainWithReconnects(AgentRig& rig, std::size_t want,
                                           int& reconnects) {
    std::vector<std::byte> got;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    auto peer = rig.Connect();
    while (got.size() < want && std::chrono::steady_clock::now() < deadline) {
        if (peer->Ended()) {
            ++reconnects;
            peer = rig.Connect();
        }
        peer->Step();
        rig.DrainPty(got);
    }
    // What is still on its way would show as extra bytes: let it arrive.
    const auto settle = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < settle) {
        if (peer->Ended()) {
            peer = rig.Connect();
        }
        peer->Step();
        rig.DrainPty(got);
    }
    return got;
}

}  // namespace

// The client replays more than the agent's queue and the pty's own buffer hold
// (its limits are larger than the agent's). The queue refuses a frame and the
// agent closes the connection; the frame has to count as not received, or the
// client's resend is dropped as a duplicate and the bytes are lost. Once the
// shell reads, every byte arrives once and in order.
//
// Kills: Session_.InputRejected(...) removed from Agent::Pump (the refused
// bytes are acknowledged as received and never reach the pty).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a refused input frame is resent and reaches the pty "
                  "once") {
    constexpr std::size_t agentLimit = 300000;
    const std::size_t total = PtyCapacity() + agentLimit + 600000;
    AgentRig rig{Limits(agentLimit, agentLimit), Limits(total, total)};
    const auto typed = Sequence(total);
    REQUIRE(rig.Client().Write(typed) == typed.size());  // detached

    // The pty does not drain: the pty and the queue fill, the next frame is
    // refused, the connection closes.
    auto peer = rig.Connect();
    const auto end = std::chrono::steady_clock::now() + 20s;
    while (!peer->Ended() && std::chrono::steady_clock::now() < end) {
        peer->Step();
    }
    REQUIRE(peer->Ended());

    int reconnects = 0;
    const auto got = DrainWithReconnects(rig, typed.size(), reconnects);
    CHECK(got.size() == typed.size());
    CHECK(got == typed);
}

// After an outage the client holds more than ResendMax unacknowledged, up to
// its DisconnectMax, and replays it all at once. The agent's queue is sized
// for that (max(ResendMax, DisconnectMax)), so while the pty does not drain
// the agent takes the whole replay and keeps the connection: nothing is
// refused. With the queue sized ResendMax the agent would refuse and close.
//
// Kills: PtyInput_ sized ResendMax instead of max(ResendMax, DisconnectMax).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a replay up to DisconnectMax fits the queue while "
                  "the pty does not drain") {
    constexpr std::size_t resend = 65536;
    const std::size_t total = PtyCapacity() + resend + 600000;
    AgentRig rig{Limits(resend, total), Limits(resend, total)};
    const auto typed = Sequence(total);
    REQUIRE(rig.Client().Write(typed) == typed.size());  // detached

    auto peer = rig.Connect();
    RunFor(*peer, 3000);
    CHECK_FALSE(peer->Ended());  // the agent took it all and kept the link

    int reconnects = 0;
    const auto got = DrainWithReconnects(rig, typed.size(), reconnects);
    CHECK(reconnects == 0);
    CHECK(got.size() == typed.size());
    CHECK(got == typed);
}
