/// The client's reconnect backoff against an agent that lets the handshake
/// through and then closes the connection, as an agent whose pty queue is full
/// does with input it cannot take (design.md §9). Reaching Live must not
/// restart the backoff: without progress the retries have to slow down.
///
/// Kills: Client::Pump resetting Attempts_ on IsLive() alone.

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

/// The agent end of one connection, blocking on nothing.
struct AgentPeer {
    explicit AgentPeer(io::UniqueFd fd)
        : Fd(std::move(fd)),
          Ac{MutableByteSpan{Frames}, io::MonotonicNow(),
             wire::PROTOCOL_VERSION_MAX} {}

    /// One round; false once the connection has failed.
    bool Step(AgentSession& session, std::uint64_t& nonces) {
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
