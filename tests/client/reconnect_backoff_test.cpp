/// The client's reconnect backoff against an agent that lets the handshake
/// through and then closes the connection, as an agent whose pty queue is full
/// does with input it cannot take (design.md §9). Reaching Live must not
/// restart the backoff: without progress the retries have to slow down.
///
/// Kills: Client::Pump resetting Attempts_ on IsLive() alone; and, in
/// Client::CreditProgress, dropping any one of the progress clauses
/// (ReceiveOffset changed, AckedOffset changed, outlived a keepalive) or the
/// KeepaliveInterval > 0 guard.

#include "client.hpp"
#include "pump_test_api.hpp"

#include <doctest/doctest.h>

#include <poll.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/tcp.hpp"
#include "zet/session/agent_session.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;
using Clock = std::chrono::steady_clock;

crypto::Key MakeKey() {
    crypto::KeyBytes bytes{};
    std::uint8_t v = 91;
    for (auto& b : bytes) {
        b = std::byte{v};
        v = static_cast<std::uint8_t>(v + 7);
    }
    return crypto::Key{bytes};
}

wire::HandshakeNonce MakeNonce(std::uint64_t counter) {
    wire::HandshakeNonce n{};
    std::uint64_t v = counter * 0x9E3779B97F4A7C15ULL + 1;
    for (auto& b : n) {
        b = static_cast<std::byte>(v & 0xFF);
        v = v * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    return n;
}

/// What the agent end does with the input a connection hands over.
enum class EInput { Ignore, Reject, Accept };

/// The agent end of one connection, blocking on nothing.
struct AgentPeer {
    explicit AgentPeer(io::UniqueFd fd)
        : Fd(std::move(fd)),
          Ac{MutableByteSpan{Frames}, io::MonotonicNow(),
             wire::PROTOCOL_VERSION_MAX} {}

    /// One round; false once the connection has failed.
    bool Step(AgentSession& session, std::uint64_t& nonces,
              EInput input = EInput::Ignore) {
        const Timestamp now = io::MonotonicNow();
        std::array<std::byte, 4096> buf{};
        pollfd pfd{.fd = Fd.Get(), .events = POLLIN, .revents = 0};
        while (Ac.ReadRoom() > 0 && ::poll(&pfd, 1, 0) > 0 &&
               (pfd.revents & POLLIN) != 0) {
            const auto n = io::ReadSome(Fd.Get(), MutableByteSpan{buf});
            if (!n || *n == 0 || !Ac.OnBytes(ByteSpan{buf}.first(*n))) {
                return false;
            }
            pfd.revents = 0;
        }
        for (int i = 0; i < 16; ++i) {
            const auto r =
                Ac.Pump(now, MutableByteSpan{Out}, MutableByteSpan{Scratch});
            if (!r) {
                return false;
            }
            if (const auto* sf = std::get_if<SendFrame>(&*r)) {
                std::size_t sent = 0;
                while (sent < sf->Written) {
                    const auto n = io::WriteSome(
                        Fd.Get(),
                        ByteSpan{Out}.subspan(sent, sf->Written - sent));
                    if (!n) {
                        return false;
                    }
                    sent += *n;
                }
            } else if (const auto* deliver = std::get_if<Deliver>(&*r)) {
                // Reject is the agent whose pty queue is full: it takes the
                // bytes back and closes. Accept is the agent that wrote them
                // to the pty and acknowledges them.
                if (input == EInput::Reject) {
                    REQUIRE(session.InputRejected(deliver->Bytes.size()));
                    return false;
                }
                if (input == EInput::Accept) {
                    REQUIRE(session.InputDelivered(deliver->Bytes.size()));
                }
            } else if (std::holds_alternative<NeedsSession>(*r)) {
                Ac.Route(session.Ref(), MakeNonce(++nonces));
            } else if (std::holds_alternative<Idle>(*r)) {
                break;
            }
        }
        return true;
    }

    io::UniqueFd Fd;
    std::array<std::byte, CONNECTION_FRAME_STORAGE> Frames{};
    std::array<std::byte, CONNECTION_OUT_SIZE> Out{};
    std::array<std::byte, CONNECTION_SCRATCH_SIZE> Scratch{};
    AgentConnection Ac;
};

}  // namespace

TEST_CASE(
    "a connection that goes live and is closed at once does not restart the "
    "backoff") {
    REQUIRE(crypto::Init());
    ::signal(SIGPIPE, SIG_IGN);

    const SessionLimits limits{};
    std::vector<std::byte> storage(limits.ResendMax + 256);
    AgentSession session{wire::DeriveSessionSecrets(MakeKey()),
                         SessionStreams{.Storage = MutableByteSpan{storage},
                                        .Now = io::MonotonicNow(),
                                        .Limits = limits}};

    auto listener = io::ListenTcp(0);
    REQUIRE(listener.has_value());
    auto addresses = io::Resolve("127.0.0.1", listener->Port);
    REQUIRE(addresses.has_value());
    auto stdoutPipe = io::MakePipe();
    REQUIRE(stdoutPipe.has_value());
    auto stderrPipe = io::MakePipe();
    REQUIRE(stderrPipe.has_value());

    // Backoff floor is 250 ms and doubles: with the backoff kept, the retries
    // after the first connection come no sooner than 125, 250, 500, 1000 ms
    // (jitter takes at most half), so at most four connections fit in the
    // window. Restarted on every Live they come every 125-250 ms: seven or
    // more.
    client::SessionTestConfig config{
        .Secrets = wire::DeriveSessionSecrets(MakeKey()),
        .Limits = limits,
        .Addresses = *addresses,
        .StdoutFd = stdoutPipe->Write.Get(),
        .StderrFd = stderrPipe->Write.Get(),
        .NoticesInline = false,
        .Ceiling = wire::PROTOCOL_VERSION_MAX,
        .BackoffMax = 4s,
        .GiveUpAfter = 1s,
    };
    auto result =
        std::async(std::launch::async, [config = std::move(config)]() mutable {
            return client::RunSessionForTest(std::move(config));
        });

    std::uint64_t nonces = 0;
    int lives = 0;
    const auto end = Clock::now() + 1600ms;
    while (Clock::now() < end) {
        pollfd lfd{
            .fd = listener->Socket.Get(), .events = POLLIN, .revents = 0};
        if (::poll(&lfd, 1, 50) <= 0) {
            continue;
        }
        auto accepted = io::Accept(listener->Socket.Get());
        REQUIRE(accepted.has_value());
        AgentPeer peer{std::move(*accepted)};
        const auto liveBy = Clock::now() + 2s;
        while (!peer.Ac.IsLive() && Clock::now() < liveBy) {
            if (!peer.Step(session, nonces)) {
                break;
            }
            ::usleep(200);
        }
        if (!peer.Ac.IsLive()) {
            continue;
        }
        ++lives;
        // Long enough for the client to see the connection live.
        const auto hold = Clock::now() + 30ms;
        while (Clock::now() < hold) {
            static_cast<void>(peer.Step(session, nonces));
            ::usleep(200);
        }
        // peer goes out of scope: the connection is closed.
    }
    CHECK(lives >= 2);
    CHECK(lives <= 5);

    stdoutPipe->Read.Reset();
    listener->Socket.Reset();
    REQUIRE(result.wait_for(10s) == std::future_status::ready);
}

namespace {

enum class EProgress { None, Acked, Output, Idle };

/// Four connections go live without progress, so the backoff has grown to
/// 2 s; the fourth then makes `progress` (or none), and the gap before the
/// fifth connection tells whether the backoff was restarted: at most 250 ms
/// when it was, at least 1000 ms (jitter takes half of 2 s) when it was not.
std::chrono::milliseconds GapAfterFourthConnection(EProgress progress,
                                                   bool keepalive) {
    REQUIRE(crypto::Init());
    ::signal(SIGPIPE, SIG_IGN);

    const SessionLimits limits{};
    std::vector<std::byte> storage(limits.ResendMax + 256);
    AgentSession session{wire::DeriveSessionSecrets(MakeKey()),
                         SessionStreams{.Storage = MutableByteSpan{storage},
                                        .Now = io::MonotonicNow(),
                                        .Limits = limits}};

    auto listener = io::ListenTcp(0);
    REQUIRE(listener.has_value());
    auto addresses = io::Resolve("127.0.0.1", listener->Port);
    REQUIRE(addresses.has_value());
    auto stdoutPipe = io::MakePipe();
    REQUIRE(stdoutPipe.has_value());
    auto stderrPipe = io::MakePipe();
    REQUIRE(stderrPipe.has_value());

    SessionLimits clientLimits = limits;
    if (!keepalive) {
        clientLimits.Liveness.KeepaliveInterval = Duration::zero();
        clientLimits.Liveness.PeerSilenceTimeout = Duration::zero();
    } else if (progress == EProgress::Idle) {
        clientLimits.Liveness.KeepaliveInterval = 150ms;
    }
    client::SessionTestConfig config{
        .Secrets = wire::DeriveSessionSecrets(MakeKey()),
        .Limits = clientLimits,
        .Addresses = *addresses,
        .StdoutFd = stdoutPipe->Write.Get(),
        .StderrFd = stderrPipe->Write.Get(),
        .NoticesInline = false,
        .Ceiling = wire::PROTOCOL_VERSION_MAX,
        .BackoffMax = 4s,
        .GiveUpAfter = 20s,
        .InitialInput = progress == EProgress::Acked ? "in" : "",
    };
    auto result =
        std::async(std::launch::async, [config = std::move(config)]() mutable {
            return client::RunSessionForTest(std::move(config));
        });

    std::uint64_t nonces = 0;
    int lives = 0;
    std::optional<Clock::time_point> closedAt;
    std::chrono::milliseconds gap{-1};
    const auto end = Clock::now() + 15s;
    while (lives < 5 && Clock::now() < end) {
        pollfd lfd{
            .fd = listener->Socket.Get(), .events = POLLIN, .revents = 0};
        if (::poll(&lfd, 1, 50) <= 0) {
            continue;
        }
        auto accepted = io::Accept(listener->Socket.Get());
        REQUIRE(accepted.has_value());
        if (lives == 4 && closedAt) {
            gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - *closedAt);
        }
        AgentPeer peer{std::move(*accepted)};
        const bool makes = lives == 3 && progress != EProgress::None;
        // Input is refused on the first three connections and acknowledged on
        // the fourth, from the first frame on, so that nothing is taken in
        // passing before.
        const EInput input = !makes                         ? EInput::Reject
                             : progress == EProgress::Acked ? EInput::Accept
                                                            : EInput::Ignore;
        const auto liveBy = Clock::now() + 2s;
        while (!peer.Ac.IsLive() && Clock::now() < liveBy) {
            if (!peer.Step(session, nonces, input)) {
                break;
            }
            ::usleep(200);
        }
        if (!peer.Ac.IsLive()) {
            continue;
        }
        ++lives;
        if (makes && progress == EProgress::Output) {
            const std::array<std::byte, 3> out{std::byte{'o'}, std::byte{'u'},
                                               std::byte{'t'}};
            REQUIRE(session.Write(ByteSpan{out}).has_value());
        }
        if (lives == 5) {
            // Over: the client ends with the session, and the run is short.
            session.End({.Kind = wire::ESessionEndKind::Exited, .Value = 0});
        }
        // An idle connection must outlive the 150 ms keepalive, an
        // acknowledgement waits 200 ms for a frame to ride on.
        const auto hold =
            Clock::now() + (!makes ? (lives == 5 ? 2s : 30ms)
                            : progress == EProgress::Acked ? 700ms
                            : progress == EProgress::Idle  ? 400ms
                                                           : 150ms);
        while (Clock::now() < hold) {
            if (!peer.Step(session, nonces, input)) {
                break;
            }
            ::usleep(200);
        }
        peer.Fd.Reset();
        closedAt = Clock::now();
    }

    stdoutPipe->Read.Reset();
    listener->Socket.Reset();
    REQUIRE(result.wait_for(30s) == std::future_status::ready);
    REQUIRE(lives == 5);
    return gap;
}

}  // namespace

TEST_CASE("input the agent acknowledged restarts the backoff") {
    CHECK(GapAfterFourthConnection(EProgress::Acked, true) <= 600ms);
}

TEST_CASE("output the client received restarts the backoff") {
    CHECK(GapAfterFourthConnection(EProgress::Output, true) <= 600ms);
}

TEST_CASE("a connection that outlived the keepalive restarts the backoff") {
    CHECK(GapAfterFourthConnection(EProgress::Idle, true) <= 600ms);
}

TEST_CASE("without progress the backoff keeps growing") {
    CHECK(GapAfterFourthConnection(EProgress::None, true) >= 900ms);
}

TEST_CASE("with the keepalive off a live connection is not progress") {
    CHECK(GapAfterFourthConnection(EProgress::None, false) >= 900ms);
}
