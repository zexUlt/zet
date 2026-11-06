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
             wire::PROTOCOL_VERSION_MIN} {}

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

/// The client under test on its own thread, against a loopback listener, with
/// the agent session behind it. Nobody reads the stdout pipe until the test
/// does. Every way the client could be waiting is closed on destruction, so a
/// failed REQUIRE cannot hang the suite.
class Rig {
public:
    Rig()
        : Limits{ShortLiveness()},
          Storage(Limits.ResendMax + 256),
          Session{wire::DeriveSessionSecrets(MakeKey(KEY_SEED)),
                  SessionStreams{.Storage = MutableByteSpan{Storage},
                                 .Now = io::MonotonicNow(),
                                 .Limits = AgentLimits(Limits)}} {
        auto listener = io::ListenTcp(0);
        REQUIRE(listener.has_value());
        Listener = std::move(*listener);
        auto addresses = io::Resolve("127.0.0.1", Listener.Port);
        REQUIRE(addresses.has_value());
        auto stdoutPipe = io::MakePipe();
        REQUIRE(stdoutPipe.has_value());
        Out = std::move(*stdoutPipe);
        auto stderrPipe = io::MakePipe();
        REQUIRE(stderrPipe.has_value());
        Err = std::move(*stderrPipe);

        client::SessionTestConfig config{
            .Secrets = wire::DeriveSessionSecrets(MakeKey(KEY_SEED)),
            .Limits = Limits,
            .Addresses = *addresses,
            .StdoutFd = Out.Write.Get(),
            .StderrFd = Err.Write.Get(),
            .NoticesInline = false,
            .Ceiling = wire::PROTOCOL_VERSION_MIN,
            .GiveUpAfter = 5s,
        };
        Result = std::async(
            std::launch::async, [config = std::move(config)]() mutable {
                return client::RunSessionForTest(std::move(config));
            });
    }

    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    Rig(Rig&&) = delete;
    Rig& operator=(Rig&&) = delete;

    ~Rig() {
        Out.Read.Reset();
        Listener.Socket.Reset();
        if (Peer) {
            Peer->Fd.Reset();
        }
        if (Result.valid()) {
            Result.wait();
        }
    }

    /// The test's own agent end does not judge the client's silence: a loaded
    /// runner may starve the client for longer than the short timeout, and
    /// that is not what is under test.
    static SessionLimits AgentLimits(SessionLimits limits) {
        limits.Liveness.PeerSilenceTimeout = Duration::zero();
        return limits;
    }

    /// The client's next connection, accepted into `Peer`.
    void Accept() {
        pollfd lfd{.fd = Listener.Socket.Get(), .events = POLLIN, .revents = 0};
        REQUIRE(::poll(&lfd, 1, 3000) > 0);
        auto accepted = io::Accept(Listener.Socket.Get());
        REQUIRE(accepted.has_value());
        Peer.emplace(std::move(*accepted));
    }

    /// Whether the client has made a connection nobody accepted yet.
    [[nodiscard]] bool Knocking(int waitMs = 0) {
        pollfd lfd{.fd = Listener.Socket.Get(), .events = POLLIN, .revents = 0};
        return ::poll(&lfd, 1, waitMs) > 0;
    }

    /// Steps the peer until it is live, in at most `within`.
    [[nodiscard]] bool UntilLive(std::chrono::milliseconds within) {
        const auto by = Clock::now() + within;
        while (!Peer->Ac.IsLive() && Clock::now() < by) {
            if (!Peer->Step(Session, Nonces)) {
                return false;
            }
            ::usleep(200);
        }
        return Peer->Ac.IsLive();
    }

    /// Steps the peer for `how long`; false if the connection failed.
    [[nodiscard]] bool Stall(std::chrono::milliseconds howLong) {
        const auto end = Clock::now() + howLong;
        while (Clock::now() < end) {
            if (!Peer->Step(Session, Nonces)) {
                return false;
            }
            ::usleep(200);
        }
        return true;
    }

    /// Reads the stdout pipe, stepping the peer, until `want` bytes are in
    /// `Received` or a minute is up.
    void Drain(std::size_t want) {
        std::array<std::byte, 65536> buf{};
        const auto until = Clock::now() + 60s;
        while (Received.size() < want && Clock::now() < until) {
            REQUIRE(Peer->Step(Session, Nonces));
            const auto n = io::ReadSome(Out.Read.Get(), MutableByteSpan{buf});
            if (n && *n > 0) {
                Received.insert(Received.end(), buf.begin(),
                                buf.begin() + static_cast<std::ptrdiff_t>(*n));
            } else {
                ::usleep(200);
            }
        }
    }

    /// Ends the session, waits for the client to exit and returns its status.
    int Finish() {
        Session.End({.Kind = wire::ESessionEndKind::Exited, .Value = 0});
        const auto endBy = Clock::now() + 5s;
        while (!Peer->Ac.DeliveredEnd() && Clock::now() < endBy) {
            static_cast<void>(Peer->Step(Session, Nonces));
            ::usleep(200);
        }
        REQUIRE(Result.wait_for(5s) == std::future_status::ready);
        return Result.get().Status;
    }

    /// What the client wrote to stderr, once it has exited.
    std::string Notices() {
        std::string notices;
        std::array<char, 4096> text{};
        Err.Write.Reset();
        for (;;) {
            const auto n = ::read(Err.Read.Get(), text.data(), text.size());
            if (n <= 0) {
                break;
            }
            notices.append(text.data(), static_cast<std::size_t>(n));
        }
        return notices;
    }

    SessionLimits Limits;
    std::vector<std::byte> Storage;
    AgentSession Session;
    std::uint64_t Nonces = 0;
    io::Listener Listener;
    io::Pipe Out;
    io::Pipe Err;
    std::future<client::SessionTestResult> Result;
    std::optional<AgentPeer> Peer;
    std::vector<std::byte> Received;
};

std::size_t CountOf(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (auto at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

}  // namespace

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a client that stops reading for its terminal is not cut for silence") {
    Rig rig;
    rig.Accept();
    REQUIRE(rig.UntilLive(3s));

    // The terminal does not drain: nobody reads the stdout pipe.
    const auto stream = Pattern(STREAM_BYTES);
    const auto written = rig.Session.Write(ByteSpan{stream});
    REQUIRE(written.has_value());
    REQUIRE(*written == stream.size());

    REQUIRE(rig.Stall(STALL));
    // The stall is real: the client is not taking bytes off the socket.
    CHECK(rig.Peer->Backlogged());
    // The path was never cut and the client did not look for a new one.
    CHECK_FALSE(rig.Knocking());

    // The terminal comes back.
    rig.Drain(stream.size());
    CHECK(rig.Received.size() == stream.size());
    CHECK(rig.Received == stream);

    CHECK(rig.Finish() == 0);
    CHECK(rig.Notices().find(LOST_PREFIX) == std::string::npos);
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "a reconnect completes its handshake while the terminal is full") {
    Rig rig;
    rig.Accept();
    REQUIRE(rig.UntilLive(3s));
    const auto stream = Pattern(STREAM_BYTES);
    REQUIRE(rig.Session.Write(ByteSpan{stream}).has_value());
    REQUIRE(rig.Stall(STALL));
    REQUIRE(rig.Peer->Backlogged());

    // The path dies under a client that is not reading. It finds out by
    // writing: a keepalive into a closed socket is refused.
    rig.Peer->Fd.Reset();
    rig.Peer.reset();
    REQUIRE(rig.Knocking(5000));
    rig.Accept();
    // The terminal is still full; the handshake must not wait for it.
    REQUIRE(rig.UntilLive(3s));

    rig.Drain(stream.size());
    CHECK(rig.Received == stream);
    CHECK(rig.Finish() == 0);
    CHECK(CountOf(rig.Notices(), LOST_PREFIX) == 1);
}
