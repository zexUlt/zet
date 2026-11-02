/// The client's read gate against the silence timer (§9), through the real
/// client: while the terminal does not drain, the client stops reading the
/// socket, and the agent's silence in return must not be taken for a dead
/// path. The test plays the agent through AgentConnection over loopback TCP.

#include "client.hpp"
#include "pump_test_api.hpp"

#include <doctest/doctest.h>

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
#include "zet/wire/message.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;
using Clock = std::chrono::steady_clock;

constexpr std::string_view LOST_PREFIX = "zet: connection lost";
constexpr std::uint8_t KEY_SEED = 77;

// Short enough to run in seconds; the stall below is longer than three of them.
constexpr Duration KEEPALIVE = 250ms;
constexpr Duration SILENCE = 1000ms;
constexpr auto STALL = 3500ms;

// More than the client's stdout queue, the pty-sized pipe and both ends of the
// loopback socket can hold, so that the agent is left with bytes it cannot
// send.
constexpr std::size_t STREAM_BYTES = 32 * 1024 * 1024;

/// Runs `Fn` when it goes out of scope.
template <typename TFn>
struct Cleanup {
    explicit Cleanup(TFn fn) : Fn(std::move(fn)) {}
    Cleanup(const Cleanup&) = delete;
    Cleanup& operator=(const Cleanup&) = delete;
    Cleanup(Cleanup&&) = delete;
    Cleanup& operator=(Cleanup&&) = delete;
    ~Cleanup() { Fn(); }
    TFn Fn;
};

struct SodiumFixture {
    SodiumFixture() {
        REQUIRE(crypto::Init());
        ::signal(SIGPIPE, SIG_IGN);
    }
};

crypto::Key MakeKey(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    std::uint8_t v = seed;
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

std::vector<std::byte> Pattern(std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 31 + (i >> 8)) & 0xFF);
    }
    return bytes;
}

SessionLimits ShortLiveness() {
    return SessionLimits{
        .ResendMax = STREAM_BYTES + 4096,
        .DisconnectMax = 4096,
        .OrphanTimeout = 30min,
        .Liveness = LivenessLimits{.KeepaliveInterval = KEEPALIVE,
                                   .PeerSilenceTimeout = SILENCE},
    };
}

/// The agent end of the one connection, never blocking: what the socket does
/// not take stays in `Pending`, and every Step still works through what the
/// client sent, so that a full socket does not look like a silent client.
struct AgentPeer {
    explicit AgentPeer(io::UniqueFd fd)
        : Fd(std::move(fd)),
          Ac{MutableByteSpan{Frames}, io::MonotonicNow(),
             wire::PROTOCOL_VERSION_MAX} {}

    /// One round: take what the client sent, flush, seal what is due.
    /// False once the connection has failed.
    bool Step(AgentSession& session, std::uint64_t& nonces) {
        const Timestamp now = io::MonotonicNow();
        std::array<std::byte, 4096> buf{};
        pollfd pfd{.fd = Fd.Get(), .events = POLLIN, .revents = 0};
        while (Ac.ReadRoom() > 0 && ::poll(&pfd, 1, 0) > 0 &&
               (pfd.revents & POLLIN) != 0) {
            const auto n =
                io::ReadSome(Fd.Get(), MutableByteSpan{buf}.first(std::min(
                                           buf.size(), Ac.ReadRoom())));
            if (!n || *n == 0 || !Ac.OnBytes(ByteSpan{buf}.first(*n))) {
                return false;
            }
            pfd.revents = 0;
        }
        Flush();
        for (int i = 0; i < 16; ++i) {
            const auto r =
                Ac.Pump(now, MutableByteSpan{Out}, MutableByteSpan{Scratch});
            if (!r) {
                return false;
            }
            if (const auto* sf = std::get_if<SendFrame>(&*r)) {
                Pending.insert(
                    Pending.end(), Out.begin(),
                    Out.begin() + static_cast<std::ptrdiff_t>(sf->Written));
                Flush();
            } else if (std::holds_alternative<NeedsSession>(*r)) {
                Ac.Route(session.Ref(), MakeNonce(++nonces));
            } else if (std::holds_alternative<Idle>(*r)) {
                break;
            }
        }
        return true;
    }

    /// Whether frames are waiting for a socket that is full.
    [[nodiscard]] bool Backlogged() const { return Sent < Pending.size(); }

    io::UniqueFd Fd;
    std::array<std::byte, CONNECTION_FRAME_STORAGE> Frames{};
    std::array<std::byte, CONNECTION_OUT_SIZE> Out{};
    std::array<std::byte, CONNECTION_SCRATCH_SIZE> Scratch{};
    AgentConnection Ac;
    std::vector<std::byte> Pending;
    std::size_t Sent = 0;

private:
    void Flush() {
        while (Sent < Pending.size()) {
            const auto n =
                io::WriteSome(Fd.Get(), ByteSpan{Pending}.subspan(Sent));
            if (!n || *n == 0) {
                return;
            }
            Sent += *n;
        }
        Pending.clear();
        Sent = 0;
    }
};

}  // namespace

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a client that stops reading for its terminal is not cut for silence") {
    const SessionLimits limits = ShortLiveness();
    // The test's own agent end does not judge the client's silence: a loaded
    // runner may starve the client for longer than the short timeout, and
    // that is not what is under test.
    SessionLimits agentLimits = limits;
    agentLimits.Liveness.PeerSilenceTimeout = Duration::zero();
    std::vector<std::byte> storage(limits.ResendMax + 256);
    AgentSession session{wire::DeriveSessionSecrets(MakeKey(KEY_SEED)),
                         SessionStreams{.Storage = MutableByteSpan{storage},
                                        .Now = io::MonotonicNow(),
                                        .Limits = agentLimits}};
    std::uint64_t nonces = 0;

    auto listener = io::ListenTcp(0);
    REQUIRE(listener.has_value());
    auto addresses = io::Resolve("127.0.0.1", listener->Port);
    REQUIRE(addresses.has_value());
    auto out = io::MakePipe();
    REQUIRE(out.has_value());
    auto err = io::MakePipe();
    REQUIRE(err.has_value());

    client::SessionTestConfig config{
        .Secrets = wire::DeriveSessionSecrets(MakeKey(KEY_SEED)),
        .Limits = limits,
        .Addresses = *addresses,
        .StdoutFd = out->Write.Get(),
        .StderrFd = err->Write.Get(),
        .NoticesInline = false,
        .Ceiling = wire::PROTOCOL_VERSION_MAX,
        .GiveUpAfter = 5s,
    };
    auto result =
        std::async(std::launch::async, [config = std::move(config)]() mutable {
            return client::RunSessionForTest(std::move(config));
        });

    // A failed REQUIRE must not leave the client waiting on a socket nobody
    // answers: closing every way it could wait lets the future finish.
    std::optional<AgentPeer> peer;
    const Cleanup cleanup{[&]() {
        out->Read.Reset();
        listener->Socket.Reset();
        if (peer) {
            peer->Fd.Reset();
        }
        if (result.valid()) {
            result.wait();
        }
    }};

    pollfd lfd{.fd = listener->Socket.Get(), .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&lfd, 1, 3000) > 0);
    auto accepted = io::Accept(listener->Socket.Get());
    REQUIRE(accepted.has_value());
    peer.emplace(std::move(*accepted));

    const auto liveBy = Clock::now() + 3s;
    while (!peer->Ac.IsLive() && Clock::now() < liveBy) {
        REQUIRE(peer->Step(session, nonces));
        ::usleep(200);
    }
    REQUIRE(peer->Ac.IsLive());

    // The terminal does not drain: nobody reads the stdout pipe.
    const auto stream = Pattern(STREAM_BYTES);
    const auto written = session.Write(ByteSpan{stream});
    REQUIRE(written.has_value());
    REQUIRE(*written == stream.size());

    bool alive = true;
    const auto stallEnd = Clock::now() + STALL;
    while (Clock::now() < stallEnd && alive) {
        alive = peer->Step(session, nonces);
        ::usleep(200);
    }
    REQUIRE(alive);
    // The stall is real: the client is not taking bytes off the socket.
    CHECK(peer->Backlogged());
    // The path was never cut and the client did not look for a new one.
    CHECK(::poll(&lfd, 1, 0) == 0);

    // The terminal comes back.
    std::vector<std::byte> received;
    received.reserve(stream.size());
    std::array<std::byte, 65536> buf{};
    const auto until = Clock::now() + 60s;
    while (received.size() < stream.size() && Clock::now() < until) {
        REQUIRE(peer->Step(session, nonces));
        const auto n = io::ReadSome(out->Read.Get(), MutableByteSpan{buf});
        if (n && *n > 0) {
            received.insert(received.end(), buf.begin(),
                            buf.begin() + static_cast<std::ptrdiff_t>(*n));
        } else {
            ::usleep(200);
        }
    }
    CHECK(received.size() == stream.size());
    CHECK(received == stream);

    session.End({.Kind = wire::ESessionEndKind::Exited, .Value = 0});
    const auto endBy = Clock::now() + 5s;
    while (!peer->Ac.DeliveredEnd() && Clock::now() < endBy) {
        static_cast<void>(peer->Step(session, nonces));
        ::usleep(200);
    }
    REQUIRE(result.wait_for(5s) == std::future_status::ready);
    CHECK(result.get().Status == 0);

    std::string notices;
    std::array<char, 4096> text{};
    out->Write.Reset();
    err->Write.Reset();
    for (;;) {
        const auto n = ::read(err->Read.Get(), text.data(), text.size());
        if (n <= 0) {
            break;
        }
        notices.append(text.data(), static_cast<std::size_t>(n));
    }
    CHECK(notices.find(LOST_PREFIX) == std::string::npos);
}
