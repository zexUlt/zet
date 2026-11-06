/// What the real agent does when the grid cannot take the client's Resize
/// (agent.cpp, the Resized handler): the pty keeps its size, and the frames
/// read together with the Resize are still taken, so that a later Resize in
/// the same read is applied without waiting for another wake.
///
/// The grid fails a Resize only when its new buffers cannot be allocated. The
/// test makes that happen through the allocation hook of the grid tests.
///
/// Kills: the pty resized after a failed grid resize.
/// Kills: the Resized handler returning from Pump after a failed grid resize
/// (the frames behind it stay unread).

#include "../../src/agent/pump_test_api.hpp"

#include "../session/grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/ioctl.h>
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

/// Defined with the OOM injection in tests/grid/probe_test.cpp, which
/// replaces the nothrow array new for this whole binary: blocks of at least
/// this many bytes are refused while it is below SIZE_MAX. Process-wide, so it
/// is armed only around the Resize under test.
extern std::atomic<std::size_t> G_REFUSE_NOTHROW_ARRAYS_FROM;

namespace {

using namespace zet::agent;
using namespace std::chrono_literals;

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

/// One ClientConnection over TCP to the agent.
class Peer {
public:
    Peer(session::ClientSession& session, std::uint64_t nonce,
         io::UniqueFd sock)
        : Sock_(std::move(sock)),
          Cc_(session, MakeNonce(nonce), MutableByteSpan{Frames_},
              io::MonotonicNow(),
              /*capable=*/true, wire::PROTOCOL_VERSION_GRID,
              MutableByteSpan{SnapBuf_}) {}

    /// Queues a Resize and seals its frame now, but holds it back: the next
    /// Step writes it together with whatever follows, in one go.
    void HoldResize(std::uint16_t rows, std::uint16_t cols) {
        Cc_.QueueResize(wire::ResizeMessage{.Rows = rows, .Cols = cols});
        auto r = Cc_.Pump(io::MonotonicNow(), MutableByteSpan{Out_},
                          MutableByteSpan{Scratch_});
        REQUIRE(r.has_value());
        const auto* sf = std::get_if<session::SendFrame>(&*r);
        REQUIRE(sf != nullptr);
        Held_.insert(Held_.end(), Out_.begin(),
                     Out_.begin() + static_cast<std::ptrdiff_t>(sf->Written));
    }

    void Queue(std::uint16_t rows, std::uint16_t cols) {
        Cc_.QueueResize(wire::ResizeMessage{.Rows = rows, .Cols = cols});
    }

    [[nodiscard]] bool IsLive() const { return Cc_.IsLive(); }

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
        std::vector<std::byte> outgoing = std::move(Held_);
        Held_.clear();
        for (;;) {
            auto r = Cc_.Pump(io::MonotonicNow(), MutableByteSpan{Out_},
                              MutableByteSpan{Scratch_});
            if (!r) {
                Closed = true;
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
        // The whole turn in one write, so that the agent reads the Resume,
        // the Resize and the keystrokes together.
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

    bool Closed{false};

private:
    io::UniqueFd Sock_;
    std::vector<std::byte> Held_;
    std::array<std::byte, session::CONNECTION_FRAME_STORAGE> Frames_{};
    std::array<std::byte, session::CONNECTION_OUT_SIZE> Out_{};
    std::array<std::byte, session::CONNECTION_SCRATCH_SIZE> Scratch_{};
    std::vector<std::byte> SnapBuf_ =
        std::vector<std::byte>(wire::SNAPSHOT_MAX_BYTES);
    session::ClientConnection Cc_;
};

/// The real agent on a real pty, whose slave end the test reads as the shell.
class AgentRig {
public:
    AgentRig()
        : ClientStorage_(4096 + 256),
          Client_(DeriveSessionSecrets(MakeKey(91)),
                  session::SessionStreams{
                      .Storage = MutableByteSpan{ClientStorage_},
                      .Now = io::MonotonicNow(),
                      .Limits = Limits()}) {
        auto pty = io::OpenPty();
        REQUIRE(pty.has_value());
        Slave_ = std::move(pty->Slave);
        termios t{};
        REQUIRE(::tcgetattr(Slave_.Get(), &t) == 0);
        ::cfmakeraw(&t);
        REQUIRE(::tcsetattr(Slave_.Get(), TCSANOW, &t) == 0);
        REQUIRE(io::SetNonBlocking(Slave_.Get()).has_value());
        auto listener = io::ListenTcp(0);
        REQUIRE(listener.has_value());
        Port_ = listener->Port;
        Thread_ = std::thread([this, l = std::move(*listener),
                               m = std::move(pty->Master)]() mutable {
            std::ignore = RunAgentForTest(AgentTestConfig{
                .Secrets = DeriveSessionSecrets(MakeKey(91)),
                .Limits = Limits(),
                .Listen = std::move(l),
                .PtyReadFd = std::move(m),
                .Ceiling = wire::PROTOCOL_VERSION_GRID,
                .GiveUpAfter = std::chrono::seconds{60},
                .Inspect = nullptr,
                .Stop = &Stop_,
                .InputToPty = true,
            });
        });
    }

    AgentRig(const AgentRig&) = delete;
    AgentRig& operator=(const AgentRig&) = delete;

    ~AgentRig() {
        G_REFUSE_NOTHROW_ARRAYS_FROM = SIZE_MAX;
        Stop_ = true;
        std::ignore = TcpConnect(Port_);
        Thread_.join();
    }

    static session::SessionLimits Limits() {
        auto l = TinyLimits();
        l.ResendMax = 4096;
        l.DisconnectMax = 4096;
        return l;
    }

    session::ClientSession& Client() { return Client_; }

    std::unique_ptr<Peer> Connect() {
        auto sock = TcpConnect(Port_);
        REQUIRE(sock.Get() != -1);
        return std::make_unique<Peer>(Client_, ++Nonces_, std::move(sock));
    }

    /// What the shell has read since the last call.
    void DrainPty(std::vector<std::byte>& into) {
        std::array<std::byte, 4096> buf{};
        for (;;) {
            auto n = io::ReadSome(Slave_.Get(), MutableByteSpan{buf});
            if (!n || *n == 0) {
                return;
            }
            into.insert(into.end(), buf.begin(),
                        buf.begin() + static_cast<std::ptrdiff_t>(*n));
        }
    }

    /// The window size the pty has, as the shell sees it.
    [[nodiscard]] winsize PtySize() const {
        winsize ws{};
        REQUIRE(::ioctl(Slave_.Get(), TIOCGWINSZ, &ws) == 0);
        return ws;
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

}  // namespace

namespace {

/// A connection that is live, with the agent up and idle behind it.
std::unique_ptr<Peer> ConnectLive(AgentRig& rig) {
    auto peer = rig.Connect();
    const auto liveBy = std::chrono::steady_clock::now() + 5s;
    while (!peer->IsLive() && !peer->Closed &&
           std::chrono::steady_clock::now() < liveBy) {
        peer->Step();
    }
    REQUIRE(peer->IsLive());
    for (int i = 0; i < 20; ++i) {
        peer->Step();  // the agent has the Resume and has nothing more to say
    }
    return peer;
}

/// 200x50 needs a 160000-byte screen (and a new scrollback), 80x30 one of
/// 38400 bytes with the scrollback kept; the 80x24 grid
/// held 30720. Armed only around the Resize under test, and only once the
/// agent is up.
constexpr std::size_t REFUSE_FROM = 100000;

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a failed grid resize leaves the pty's size alone") {
    AgentRig rig;
    auto peer = ConnectLive(rig);
    const winsize before = rig.PtySize();

    G_REFUSE_NOTHROW_ARRAYS_FROM = REFUSE_FROM;
    peer->Queue(50, 200);
    const auto end = std::chrono::steady_clock::now() + 500ms;
    while (std::chrono::steady_clock::now() < end) {
        peer->Step();
    }
    G_REFUSE_NOTHROW_ARRAYS_FROM = SIZE_MAX;

    const winsize after = rig.PtySize();
    CHECK(after.ws_row == before.ws_row);
    CHECK(after.ws_col == before.ws_col);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: the frames read with a failed grid resize are still "
                  "taken") {
    AgentRig rig;
    auto peer = ConnectLive(rig);

    // The first Resize fails; the second, behind it in the same read, fits.
    // Then the client is silent, so that nothing but those bytes wakes the
    // agent.
    G_REFUSE_NOTHROW_ARRAYS_FROM = REFUSE_FROM;
    peer->HoldResize(50, 200);
    peer->HoldResize(30, 80);
    peer->Step();

    winsize size = rig.PtySize();
    const auto end = std::chrono::steady_clock::now() + 1s;
    while ((size.ws_row != 30 || size.ws_col != 80) &&
           std::chrono::steady_clock::now() < end) {
        ::usleep(1000);
        size = rig.PtySize();
    }
    G_REFUSE_NOTHROW_ARRAYS_FROM = SIZE_MAX;

    CHECK(size.ws_row == 30);
    CHECK(size.ws_col == 80);
}
