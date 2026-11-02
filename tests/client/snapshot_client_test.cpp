/// Tests for the client-side snapshot wiring (§9 path B, ADR-035):
///   24. TryWriteSnapshot writes bytes correctly.
///   25. TryWriteSnapshot returns false on write error.
///   26. Client::Pump: snapshot bytes reach the stdout pipe.
///   27. Client::Pump: ConfirmApply deferred until stdout drains.
///   28. Client::Pump: DenyApply + drop when snapshot too large.
///   29. Client::Pump: SNAPSHOT_RESTORE_NOTICE on stderr after success.
///   30. Client::Pump: SessionEnd{Overflow} exits with OVERFLOW_STATUS.
///
/// Tests 26-30 drive the real Client via RunSessionForTest over a socketpair
/// whose other end is an in-process AgentConnection.

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
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/session/agent_session.hpp"
#include "zet/session/client_session.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"
#include "zet/wire/key_schedule.hpp"
#include "zet/wire/limits.hpp"
#include "zet/wire/message.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;
using zet::wire::DeriveSessionSecrets;
using zet::wire::HandshakeNonce;

struct SodiumFixture {
    SodiumFixture() { REQUIRE(crypto::Init()); }
};

// Matches the constant in client.cpp.
constexpr std::size_t STDOUT_QUEUE_MAX = 1024 * 1024;
constexpr int OVERFLOW_STATUS = 70;

crypto::Key MakeKey(std::uint8_t seed) {
    crypto::KeyBytes bytes{};
    std::uint8_t v = seed;
    for (auto& b : bytes) {
        b = std::byte{v};
        v = static_cast<std::uint8_t>(v + 7);
    }
    return crypto::Key{bytes};
}

HandshakeNonce MakeNonce(std::uint64_t counter) {
    HandshakeNonce n{};
    std::uint64_t v = counter * 0x9E3779B97F4A7C15ULL + 1;
    for (auto& b : n) {
        b = static_cast<std::byte>(v & 0xFF);
        v = v * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    return n;
}

/// Tiny limits so the ring overflows after a small write.
SessionLimits TinyLimits() {
    return SessionLimits{
        .ResendMax = 512,
        .DisconnectMax = 512,
        .OrphanTimeout = std::chrono::minutes{30},
        .Liveness = LivenessLimits{.KeepaliveInterval = Duration{0},
                                   .PeerSilenceTimeout = Duration{0}},
    };
}

struct Space {
    std::array<std::byte, CONNECTION_FRAME_STORAGE> AgentFrames{};
    std::array<std::byte, CONNECTION_FRAME_STORAGE> ClientFrames{};
    std::array<std::byte, CONNECTION_OUT_SIZE> Out{};
    std::array<std::byte, CONNECTION_SCRATCH_SIZE> Scratch{};
    std::vector<std::byte> SnapshotBuf{wire::SNAPSHOT_MAX_BYTES, std::byte{}};
};

// ── Agent harness ─────────────────────────────────────────────────────────

/// The socket is non-blocking and a frame dropped halfway desynchronises the
/// stream, so wait for room instead of discarding what did not fit.
void WriteAll(int fd, ByteSpan bytes) {
    while (!bytes.empty()) {
        auto n = io::WriteSome(fd, bytes);
        if (n) {
            bytes = bytes.subspan(*n);
            continue;
        }
        REQUIRE(n.error().WouldBlock());
        pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
        REQUIRE(::poll(&pfd, 1, 3000) > 0);
    }
}

/// Feed available socket bytes into the agent, drain its pump writing every
/// SendFrame back, and return the first non-SendFrame, non-Idle event.
std::optional<ConnectionEvent> AgentTick(AgentConnection& ac, Space& sp,
                                         int agentFd, Timestamp now) {
    std::array<std::byte, 4096> buf{};
    pollfd pfd{.fd = agentFd, .events = POLLIN, .revents = 0};
    while (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        auto n = io::ReadSome(agentFd, MutableByteSpan{buf});
        if (!n || *n == 0) break;
        if (!ac.OnBytes(ByteSpan{buf}.first(*n))) break;
        pfd.revents = 0;
    }
    for (int i = 0; i < 64; ++i) {
        auto r =
            ac.Pump(now, MutableByteSpan{sp.Out}, MutableByteSpan{sp.Scratch});
        if (!r) return std::nullopt;
        if (const auto* sf = std::get_if<SendFrame>(&*r)) {
            WriteAll(agentFd, ByteSpan{sp.Out}.first(sf->Written));
            continue;
        }
        if (std::holds_alternative<Idle>(*r)) return std::nullopt;
        return *r;
    }
    return std::nullopt;
}

bool DriveAgentToLive(AgentConnection& ac, AgentSession& session, Space& sp,
                      int agentFd, std::uint64_t& nonces,
                      std::chrono::milliseconds timeout = 3000ms) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until) {
        auto ev = AgentTick(ac, sp, agentFd, io::MonotonicNow());
        if (ev && std::holds_alternative<NeedsSession>(*ev)) {
            ac.Route(session.Ref(), MakeNonce(++nonces));
        }
        if (ac.IsLive()) return true;
        ::usleep(500);
    }
    return false;
}

/// Path B: answer NeedsSnapshot with `snap`; true once the agent is live.
bool DriveAgentSupplySnapshot(AgentConnection& ac, Space& sp, int agentFd,
                              ByteSpan snap,
                              std::chrono::milliseconds timeout = 3000ms) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    bool supplied = false;
    while (std::chrono::steady_clock::now() < until) {
        const Timestamp now = io::MonotonicNow();
        auto ev = AgentTick(ac, sp, agentFd, now);
        if (ev && std::holds_alternative<NeedsSnapshot>(*ev)) {
            ac.SupplySnapshot(now, snap, 0, 0);
            supplied = true;
        }
        if (supplied && ac.IsLive()) return true;
        ::usleep(500);
    }
    return false;
}

bool DriveAgentToEnd(AgentConnection& ac, Space& sp, int agentFd,
                     std::chrono::milliseconds timeout = 3000ms) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until && !ac.DeliveredEnd()) {
        static_cast<void>(AgentTick(ac, sp, agentFd, io::MonotonicNow()));
        ::usleep(500);
    }
    return ac.DeliveredEnd();
}

std::vector<std::byte> DrainFd(int fd,
                               std::chrono::milliseconds timeout = 3000ms) {
    std::vector<std::byte> result;
    std::array<std::byte, 4096> buf{};
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            until - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0) break;
        auto n = io::ReadSome(fd, MutableByteSpan{buf});
        if (!n || *n == 0) break;
        result.insert(result.end(), buf.begin(), buf.begin() + *n);
    }
    return result;
}

/// AgentSession starts in Reap mode, so overflowing a fresh session reaps it.
/// One in-memory connection runs SetGridInfo (capable → Resync), then
/// detaches, leaving the session ready for an overflow that forces path B.
void PrimeAgentSession(AgentSession& agentSession, std::uint8_t keySeed,
                       const SessionLimits& lim, std::uint64_t& nonces) {
    const Timestamp now = io::MonotonicNow();
    Space sp;
    std::vector<std::byte> clientStorage(lim.ResendMax + 256);
    ClientSession clientSession{
        DeriveSessionSecrets(MakeKey(keySeed)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = now,
                       .Limits = lim}};
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, now,
                       wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc{clientSession,
                        MakeNonce(++nonces),
                        MutableByteSpan{sp.ClientFrames},
                        now,
                        true,
                        wire::PROTOCOL_VERSION_GRID,
                        MutableByteSpan{sp.SnapshotBuf}};
    for (int i = 0; i < 64 && !ac.IsLive(); ++i) {
        auto ea =
            ac.Pump(now, MutableByteSpan{sp.Out}, MutableByteSpan{sp.Scratch});
        REQUIRE(ea.has_value());
        if (std::holds_alternative<NeedsSession>(*ea)) {
            ac.Route(agentSession.Ref(), MakeNonce(++nonces));
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(
                cc.OnBytes(ByteSpan{sp.Out}.first(sf->Written)).has_value());
        }
        auto ec =
            cc.Pump(now, MutableByteSpan{sp.Out}, MutableByteSpan{sp.Scratch});
        REQUIRE(ec.has_value());
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(
                ac.OnBytes(ByteSpan{sp.Out}.first(sf->Written)).has_value());
        }
    }
    REQUIRE(ac.IsLive());
    ac.Close(now);
}

/// An agent session whose ring overflowed while detached in Resync mode, so
/// the next connection takes path B.
struct OverflowedSession {
    SessionLimits Limits = TinyLimits();
    std::uint8_t KeySeed;
    std::vector<std::byte> Storage;
    AgentSession Session;
    std::uint64_t Nonces = 0;

    explicit OverflowedSession(std::uint8_t keySeed)
        : KeySeed{keySeed},
          Storage(Limits.DisconnectMax + 256),
          Session{DeriveSessionSecrets(MakeKey(keySeed)),
                  SessionStreams{.Storage = MutableByteSpan{Storage},
                                 .Now = io::MonotonicNow(),
                                 .Limits = Limits}} {
        PrimeAgentSession(Session, KeySeed, Limits, Nonces);
        const std::vector<std::byte> fill(200, std::byte{0xBB});
        for (int i = 0; i < 6; ++i) std::ignore = Session.Write(ByteSpan{fill});
        REQUIRE_FALSE(Session.HasEnded());
    }
};

// ── TryWriteSnapshot unit tests (24-25) ───────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "TryWriteSnapshot writes bytes to a pipe in order") {
    const auto pipe = io::MakePipe();
    REQUIRE(pipe.has_value());

    std::vector<std::byte> payload(64);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(static_cast<std::uint8_t>(i * 3));
    }
    const Timestamp until = io::MonotonicNow() + 5s;

    REQUIRE(
        client::TryWriteSnapshot(pipe->Write.Get(), ByteSpan{payload}, until));

    std::vector<std::byte> received(payload.size());
    const auto n = ::read(pipe->Read.Get(), received.data(), received.size());
    REQUIRE(n == static_cast<ssize_t>(payload.size()));
    CHECK(received == payload);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "TryWriteSnapshot returns false on write error") {
    auto pipe = io::MakePipe();
    REQUIRE(pipe.has_value());
    pipe->Read.Reset();

    const std::vector<std::byte> payload(32, std::byte{0xAB});
    const Timestamp until = io::MonotonicNow() + 5s;

    ::signal(SIGPIPE, SIG_IGN);
    CHECK_FALSE(
        client::TryWriteSnapshot(pipe->Write.Get(), ByteSpan{payload}, until));
}

// ── Client::Pump integration tests (26-30) ────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "snapshot bytes reach the stdout pipe (Client::Pump)") {
    OverflowedSession s{41};
    auto pair = io::MakeSocketPair();
    REQUIRE(pair.has_value());
    auto stdoutPipe = io::MakePipe();
    REQUIRE(stdoutPipe.has_value());
    const int agentFd = pair->First.Get();

    Space sp;
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, io::MonotonicNow(),
                       wire::PROTOCOL_VERSION_GRID};
    const std::vector<std::byte> snap(64, std::byte{0xAB});

    auto fut = std::async(std::launch::async, [&] {
        return client::RunSessionForTest(client::SessionTestConfig{
            .Secrets = DeriveSessionSecrets(MakeKey(s.KeySeed)),
            .Limits = s.Limits,
            .Socket = std::move(pair->Second),
            .StdoutFd = stdoutPipe->Write.Get(),
            .GiveUpAfter = 2s,
        });
    });

    REQUIRE(DriveAgentToLive(ac, s.Session, sp, agentFd, s.Nonces));
    REQUIRE(DriveAgentSupplySnapshot(ac, sp, agentFd, ByteSpan{snap}));
    s.Session.End({.Kind = wire::ESessionEndKind::Exited, .Value = 0});
    CHECK(DriveAgentToEnd(ac, sp, agentFd));

    const int status = fut.get();
    stdoutPipe->Write.Reset();
    const std::vector<std::byte> received = DrainFd(stdoutPipe->Read.Get());

    CHECK(status == 0);
    CHECK(received == snap);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "ConfirmApply deferred until stdout drains (Client::Pump)") {
    // Agent Data is held while the grid awaits GridApplied, and SessionEnd
    // waits for every byte to be acknowledged.  With stdout full the snapshot
    // cannot drain, so ConfirmApply, and with it SessionEnd, must wait.
    OverflowedSession s{42};
    auto pair = io::MakeSocketPair();
    REQUIRE(pair.has_value());
    auto stdoutPipe = io::MakePipe();
    REQUIRE(stdoutPipe.has_value());
    const int agentFd = pair->First.Get();

    std::size_t prefilled = 0;
    {
        const std::array<std::byte, 4096> junk{};
        while (true) {
            auto n = io::WriteSome(stdoutPipe->Write.Get(), ByteSpan{junk});
            if (!n || *n == 0) break;
            prefilled += *n;
        }
    }
    REQUIRE(prefilled > 0);

    Space sp;
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, io::MonotonicNow(),
                       wire::PROTOCOL_VERSION_GRID};
    const std::vector<std::byte> snap(64, std::byte{0xDD});
    const std::vector<std::byte> tail(1, std::byte{0x7A});

    auto fut = std::async(std::launch::async, [&] {
        return client::RunSessionForTest(client::SessionTestConfig{
            .Secrets = DeriveSessionSecrets(MakeKey(s.KeySeed)),
            .Limits = s.Limits,
            .Socket = std::move(pair->Second),
            .StdoutFd = stdoutPipe->Write.Get(),
            .GiveUpAfter = 2s,
        });
    });

    REQUIRE(DriveAgentToLive(ac, s.Session, sp, agentFd, s.Nonces));
    REQUIRE(DriveAgentSupplySnapshot(ac, sp, agentFd, ByteSpan{snap}));
    REQUIRE(s.Session.Write(ByteSpan{tail}).has_value());
    s.Session.End({.Kind = wire::ESessionEndKind::Exited, .Value = 0});

    CHECK_FALSE(DriveAgentToEnd(ac, sp, agentFd, 300ms));

    std::vector<std::byte> drained;
    const auto until = std::chrono::steady_clock::now() + 3s;
    while (!ac.DeliveredEnd() && std::chrono::steady_clock::now() < until) {
        const auto chunk = DrainFd(stdoutPipe->Read.Get(), 5ms);
        drained.insert(drained.end(), chunk.begin(), chunk.end());
        static_cast<void>(AgentTick(ac, sp, agentFd, io::MonotonicNow()));
    }
    CHECK(ac.DeliveredEnd());
    CHECK(fut.get() == 0);
    stdoutPipe->Write.Reset();
    const auto rest = DrainFd(stdoutPipe->Read.Get(), 500ms);
    drained.insert(drained.end(), rest.begin(), rest.end());

    std::vector<std::byte> expected(prefilled, std::byte{0});
    expected.insert(expected.end(), snap.begin(), snap.end());
    expected.insert(expected.end(), tail.begin(), tail.end());
    CHECK(drained == expected);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "DenyApply and drop when snapshot too large (Client::Pump)") {
    OverflowedSession s{43};
    auto pair = io::MakeSocketPair();
    REQUIRE(pair.has_value());
    auto stdoutPipe = io::MakePipe();
    REQUIRE(stdoutPipe.has_value());
    const int agentFd = pair->First.Get();

    Space sp;
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, io::MonotonicNow(),
                       wire::PROTOCOL_VERSION_GRID};
    const std::vector<std::byte> bigSnap(STDOUT_QUEUE_MAX + 1, std::byte{0xFF});

    auto fut = std::async(std::launch::async, [&] {
        return client::RunSessionForTest(client::SessionTestConfig{
            .Secrets = DeriveSessionSecrets(MakeKey(s.KeySeed)),
            .Limits = s.Limits,
            .Socket = std::move(pair->Second),
            .StdoutFd = stdoutPipe->Write.Get(),
            .GiveUpAfter = 1s,
        });
    });

    REQUIRE(DriveAgentToLive(ac, s.Session, sp, agentFd, s.Nonces));
    REQUIRE(DriveAgentSupplySnapshot(ac, sp, agentFd, ByteSpan{bigSnap}));
    // Keep emitting the grid until the client gives up on the link.
    while (fut.wait_for(1ms) != std::future_status::ready) {
        static_cast<void>(AgentTick(ac, sp, agentFd, io::MonotonicNow()));
    }

    const int status = fut.get();
    stdoutPipe->Write.Reset();
    const std::vector<std::byte> received =
        DrainFd(stdoutPipe->Read.Get(), 500ms);

    CHECK(status != 0);
    CHECK(received.empty());
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "SNAPSHOT_RESTORE_NOTICE written to stderr (Client::Pump)") {
    OverflowedSession s{44};
    auto pair = io::MakeSocketPair();
    REQUIRE(pair.has_value());
    auto stdoutPipe = io::MakePipe();
    REQUIRE(stdoutPipe.has_value());
    auto stderrPipe = io::MakePipe();
    REQUIRE(stderrPipe.has_value());
    const int agentFd = pair->First.Get();

    Space sp;
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, io::MonotonicNow(),
                       wire::PROTOCOL_VERSION_GRID};
    const std::vector<std::byte> snap(32, std::byte{0x11});

    auto fut = std::async(std::launch::async, [&] {
        return client::RunSessionForTest(client::SessionTestConfig{
            .Secrets = DeriveSessionSecrets(MakeKey(s.KeySeed)),
            .Limits = s.Limits,
            .Socket = std::move(pair->Second),
            .StdoutFd = stdoutPipe->Write.Get(),
            .StderrFd = stderrPipe->Write.Get(),
            .NoticesInline = false,
            .GiveUpAfter = 2s,
        });
    });

    REQUIRE(DriveAgentToLive(ac, s.Session, sp, agentFd, s.Nonces));
    REQUIRE(DriveAgentSupplySnapshot(ac, sp, agentFd, ByteSpan{snap}));
    s.Session.End({.Kind = wire::ESessionEndKind::Exited, .Value = 0});
    CHECK(DriveAgentToEnd(ac, sp, agentFd));
    CHECK(fut.get() == 0);

    stderrPipe->Write.Reset();
    const std::vector<std::byte> stderrBytes =
        DrainFd(stderrPipe->Read.Get(), 500ms);
    const std::string stderrStr{
        reinterpret_cast<const char*>(stderrBytes.data()), stderrBytes.size()};
    CHECK(stderrStr.find("zet: screen restored") != std::string::npos);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "SessionEnd{Overflow} exits OVERFLOW_STATUS (Client::Pump)") {
    // The overflow verdict reaches the client over the live channel.
    const SessionLimits lim = TinyLimits();
    constexpr std::uint8_t KEY_SEED = 45;
    std::vector<std::byte> agentStorage(lim.DisconnectMax + 256);
    AgentSession agentSession{
        DeriveSessionSecrets(MakeKey(KEY_SEED)),
        SessionStreams{.Storage = MutableByteSpan{agentStorage},
                       .Now = io::MonotonicNow(),
                       .Limits = lim}};

    auto pair = io::MakeSocketPair();
    REQUIRE(pair.has_value());
    auto stdoutPipe = io::MakePipe();
    REQUIRE(stdoutPipe.has_value());
    const int agentFd = pair->First.Get();

    Space sp;
    std::uint64_t nonces = 0;
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, io::MonotonicNow(),
                       wire::PROTOCOL_VERSION_GRID};

    auto fut = std::async(std::launch::async, [&] {
        return client::RunSessionForTest(client::SessionTestConfig{
            .Secrets = DeriveSessionSecrets(MakeKey(KEY_SEED)),
            .Limits = lim,
            .Socket = std::move(pair->Second),
            .StdoutFd = stdoutPipe->Write.Get(),
            .GiveUpAfter = 2s,
        });
    });

    REQUIRE(DriveAgentToLive(ac, agentSession, sp, agentFd, nonces));
    agentSession.End({.Kind = wire::ESessionEndKind::Overflow, .Value = 0});
    CHECK(DriveAgentToEnd(ac, sp, agentFd));
    CHECK(fut.get() == OVERFLOW_STATUS);
}

}  // namespace
