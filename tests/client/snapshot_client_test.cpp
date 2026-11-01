/// Tests for the client-side snapshot wiring (§9 path B, ADR-035):
///   • TryWriteSnapshot writes bytes correctly and detects errors.
///   • ClientConnection at ceiling=2 with gridCapable=true produces an
///     ApplySnapshot event; ConfirmApply causes GridApplied; DenyApply does
///     not.
///   • A non-capable client at ceiling=2 receives SessionOverflowed when the
///     ring overflows — the Run loop maps this to OVERFLOW_STATUS with no
///     reconnect attempt.
///
/// Tests are in the ZET_HAVE_IO section of CMakeLists.txt because
/// TryWriteSnapshot needs real I/O (pipes), and zet_client_loop is an
/// IO-dependent library.

#include "client.hpp"

#include <doctest/doctest.h>

#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cstddef>
#include <cstdint>
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

// ── Helpers ───────────────────────────────────────────────────────────────

const Timestamp START = Timestamp::FromNanos(1'000'000'000);

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

template <typename TConn>
[[nodiscard]] bool Feed(TConn& conn, Space& sp, std::size_t written) {
    auto r = conn.OnBytes(ByteSpan{sp.Out}.first(written));
    return r.has_value();
}

ConnectionResult PumpAgent(AgentConnection& a, Space& sp, Timestamp now) {
    return a.Pump(now, MutableByteSpan{sp.Out}, MutableByteSpan{sp.Scratch});
}

ConnectionResult PumpClient(ClientConnection& c, Space& sp, Timestamp now) {
    return c.Pump(now, MutableByteSpan{sp.Out}, MutableByteSpan{sp.Scratch});
}

/// Drive both ends to Live (handshake complete).
bool DriveToLive(AgentConnection& agent, ClientConnection& client,
                 AgentSession& session, Space& sp, std::uint64_t& nonces) {
    for (int i = 0; i < 64; ++i) {
        auto ea = PumpAgent(agent, sp, START);
        if (!ea) return false;
        if (std::get_if<NeedsSession>(&*ea) != nullptr)
            agent.Route(session.Ref(), MakeNonce(++nonces));
        else if (const auto* sf = std::get_if<SendFrame>(&*ea))
            if (!Feed(client, sp, sf->Written)) return false;
        auto ec = PumpClient(client, sp, START);
        if (!ec) return false;
        if (const auto* sf = std::get_if<SendFrame>(&*ec))
            if (!Feed(agent, sp, sf->Written)) return false;
        if (agent.IsLive() && client.IsLive()) return true;
    }
    return false;
}

// ── TryWriteSnapshot unit tests ───────────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "TryWriteSnapshot writes bytes to a pipe in order") {
    // Arrange: a non-blocking pipe.
    const auto pipe = io::MakePipe();
    REQUIRE(pipe.has_value());

    // Content: 64 bytes of a known pattern so order is verifiable.
    std::vector<std::byte> payload(64);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(static_cast<std::uint8_t>(i * 3));
    }
    // Deadline uses the real clock because TryWriteSnapshot calls MonotonicNow.
    const Timestamp until = io::MonotonicNow() + 5s;

    // Act.
    REQUIRE(
        client::TryWriteSnapshot(pipe->Write.Get(), ByteSpan{payload}, until));

    // Assert: every byte arrives in order.
    std::vector<std::byte> received(payload.size());
    const auto n = ::read(pipe->Read.Get(), received.data(), received.size());
    REQUIRE(n == static_cast<ssize_t>(payload.size()));
    CHECK(received == payload);

    // Break check: if TryWriteSnapshot wrote bytes in a different order the
    // equality check above would fail.
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "TryWriteSnapshot returns false on write error") {
    // Arrange: close the read end so any write raises EPIPE.
    const auto pipe = io::MakePipe();
    REQUIRE(pipe.has_value());
    const int wfd = pipe->Write.Get();
    // Close the read end to make the pipe broken.
    // Release() returns the fd but we deliberately do not use it (we just
    // closed it above; the UniqueFd destructor must not double-close).
    ::close(pipe->Read.Get());
    static_cast<void>(const_cast<io::UniqueFd&>(pipe->Read).Release());

    const std::vector<std::byte> payload(32, std::byte{0xAB});
    const Timestamp until = io::MonotonicNow() + 5s;

    // Act + Assert: must return false; SIGPIPE is already ignored by Main
    // but in this test context we must not die on it.
    ::signal(SIGPIPE, SIG_IGN);
    CHECK_FALSE(client::TryWriteSnapshot(wfd, ByteSpan{payload}, until));

    // Break check: if the function ignores write errors it would return true
    // and this CHECK_FALSE would fail.
}

// ── Connection-layer wiring tests ─────────────────────────────────────────
//
// These tests exercise the session/connection API directly, mirroring what
// Client::Pump does after FinishConnecting advertises gridCapable=true.

TEST_CASE_FIXTURE(SodiumFixture,
                  "ApplySnapshot bytes match snapshot; "
                  "ConfirmApply sends GridApplied (stream gen advances)") {
    // Session pair with tiny limits so a short write overflows the ring.
    const SessionLimits lim = TinyLimits();
    std::vector<std::byte> agentStorage(lim.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(lim.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(11)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = lim}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(11)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = lim}};
    Space sp;
    std::uint64_t nonces = 0;

    // First connection: bring up, then drop.
    {
        AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                           wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc{client,
                            MakeNonce(++nonces),
                            MutableByteSpan{sp.ClientFrames},
                            START,
                            /*gridCapable=*/true,
                            wire::PROTOCOL_VERSION_GRID,
                            MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac, cc, agent, sp, nonces));
        ac.Close(START);
    }

    // Write enough data to force path B.
    const std::vector<std::byte> overflow(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) std::ignore = agent.Write(ByteSpan{overflow});

    const std::uint32_t genBefore = client.StreamGen();

    // Second connection: gridCapable=true, ceiling=2.
    AgentConnection ac2{MutableByteSpan{sp.AgentFrames}, START,
                        wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc2{client,
                         MakeNonce(++nonces),
                         MutableByteSpan{sp.ClientFrames},
                         START,
                         /*gridCapable=*/true,
                         wire::PROTOCOL_VERSION_GRID,
                         MutableByteSpan{sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac2, cc2, agent, sp, nonces));

    // Drive until ApplySnapshot arrives at the client.
    const std::vector<std::byte> snap(16, std::byte{0x77});
    bool gotApply = false;
    std::uint32_t applyGen = 0;
    for (int i = 0; i < 120 && !gotApply; ++i) {
        auto ea = PumpAgent(ac2, sp, START);
        REQUIRE(ea.has_value());
        if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            ac2.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc2, sp, sf->Written));
        }
        auto ec = PumpClient(cc2, sp, START);
        REQUIRE(ec.has_value());
        if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
            gotApply = true;
            // Verify the bytes match the snapshot the agent supplied.
            REQUIRE(as->Bytes.size() == snap.size());
            CHECK(std::equal(as->Bytes.begin(), as->Bytes.end(), snap.begin()));
            applyGen = as->StreamGen;
            // Confirm: this is what Client::Pump does on a successful write.
            cc2.ConfirmApply(applyGen);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac2, sp, sf->Written));
        }
    }
    REQUIRE(gotApply);

    // Drain so GridApplied reaches the agent (several more pump cycles).
    for (int i = 0; i < 40; ++i) {
        auto ea = PumpAgent(ac2, sp, START);
        if (!ea) break;
        if (const auto* sf = std::get_if<SendFrame>(&*ea))
            REQUIRE(Feed(cc2, sp, sf->Written));
        auto ec = PumpClient(cc2, sp, START);
        if (!ec) break;
        if (const auto* sf = std::get_if<SendFrame>(&*ec))
            REQUIRE(Feed(ac2, sp, sf->Written));
    }

    // GridApplied advanced StreamGen — proof that ConfirmApply was called.
    CHECK(client.StreamGen() > genBefore);

    // Break check: remove the cc2.ConfirmApply(applyGen) call above →
    // GridApplied is never sent → StreamGen stays at genBefore → CHECK fails.
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "DenyApply does not advance StreamGen and sends no "
                  "GridApplied") {
    const SessionLimits lim = TinyLimits();
    std::vector<std::byte> agentStorage(lim.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(lim.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(22)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = lim}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(22)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = lim}};
    Space sp;
    std::uint64_t nonces = 0;

    {
        AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                           wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc{client,
                            MakeNonce(++nonces),
                            MutableByteSpan{sp.ClientFrames},
                            START,
                            /*gridCapable=*/true,
                            wire::PROTOCOL_VERSION_GRID,
                            MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac, cc, agent, sp, nonces));
        ac.Close(START);
    }

    const std::vector<std::byte> overflow(200, std::byte{0xCC});
    for (int i = 0; i < 6; ++i) std::ignore = agent.Write(ByteSpan{overflow});

    const std::uint32_t genBefore = client.StreamGen();

    AgentConnection ac2{MutableByteSpan{sp.AgentFrames}, START,
                        wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc2{client,
                         MakeNonce(++nonces),
                         MutableByteSpan{sp.ClientFrames},
                         START,
                         /*gridCapable=*/true,
                         wire::PROTOCOL_VERSION_GRID,
                         MutableByteSpan{sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac2, cc2, agent, sp, nonces));

    const std::vector<std::byte> snap(16, std::byte{0x55});
    bool gotApply = false;
    for (int i = 0; i < 120 && !gotApply; ++i) {
        auto ea = PumpAgent(ac2, sp, START);
        REQUIRE(ea.has_value());
        if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            ac2.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc2, sp, sf->Written));
        }
        auto ec = PumpClient(cc2, sp, START);
        REQUIRE(ec.has_value());
        if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
            gotApply = true;
            // Deny: simulates a write failure, as Client::Pump does.
            cc2.DenyApply(as->StreamGen);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac2, sp, sf->Written));
        }
    }
    REQUIRE(gotApply);

    // Pump a few more cycles; GridApplied must NOT have been sent.
    for (int i = 0; i < 40; ++i) {
        auto ea = PumpAgent(ac2, sp, START);
        if (!ea) break;
        if (const auto* sf = std::get_if<SendFrame>(&*ea))
            REQUIRE(Feed(cc2, sp, sf->Written));
        auto ec = PumpClient(cc2, sp, START);
        if (!ec) break;
        if (const auto* sf = std::get_if<SendFrame>(&*ec))
            REQUIRE(Feed(ac2, sp, sf->Written));
    }

    // StreamGen must be unchanged: DenyApply does not advance it.
    CHECK(client.StreamGen() == genBefore);

    // Break check: replace cc2.DenyApply with cc2.ConfirmApply →
    // StreamGen advances → CHECK(== genBefore) fails.
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "Non-capable client at ceiling=2 receives SessionOverflowed "
                  "when the ring overflows; no reconnect") {
    // This mirrors what Client::Run does: it exits with OVERFLOW_STATUS and
    // never calls StartConnecting again.  Here we verify the session layer
    // produces SessionOverflowed so the Run loop can act on it.
    const SessionLimits lim = TinyLimits();
    std::vector<std::byte> agentStorage(lim.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(lim.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(33)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = lim}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(33)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = lim}};
    Space sp;
    std::uint64_t nonces = 0;

    // First connection without GRID_CAPABLE.
    {
        AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                           wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc{client,
                            MakeNonce(++nonces),
                            MutableByteSpan{sp.ClientFrames},
                            START,
                            /*gridCapable=*/false,
                            wire::PROTOCOL_VERSION_GRID,
                            MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac, cc, agent, sp, nonces));
        ac.Close(START);
    }

    // Overflow the ring.
    const std::vector<std::byte> flood(200, std::byte{0xDD});
    for (int i = 0; i < 6; ++i) std::ignore = agent.Write(ByteSpan{flood});

    // Reconnect without GRID_CAPABLE at ceiling=2.
    AgentConnection ac2{MutableByteSpan{sp.AgentFrames}, START,
                        wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc2{client,
                         MakeNonce(++nonces),
                         MutableByteSpan{sp.ClientFrames},
                         START,
                         /*gridCapable=*/false,
                         wire::PROTOCOL_VERSION_GRID,
                         MutableByteSpan{sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac2, cc2, agent, sp, nonces));

    bool overflowed = false;
    for (int i = 0; i < 60; ++i) {
        auto ea = PumpAgent(ac2, sp, START);
        if (!ea) break;
        if (const auto* sf = std::get_if<SendFrame>(&*ea))
            REQUIRE(Feed(cc2, sp, sf->Written));
        auto ec = PumpClient(cc2, sp, START);
        if (!ec) break;
        if (std::holds_alternative<SessionOverflowed>(*ec)) {
            overflowed = true;
            break;
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec))
            REQUIRE(Feed(ac2, sp, sf->Written));
    }

    CHECK(overflowed);

    // Break check: remove the SessionOverflowed variant from the result
    // type or from Client::Pump's event handling → overflowed stays false →
    // CHECK fails.
}

}  // namespace
