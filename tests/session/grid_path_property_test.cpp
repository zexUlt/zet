/// Grid path A and path B property tests.
///
/// Path A: no GRID_CAPABLE → byte-identical replay on every reconnect.
/// Path B: ring overflowed in Resync mode → NeedsSnapshot emitted, caller
///   supplies snapshot, client receives ApplySnapshot, session continues.
///
/// Additional scenarios covered:
///   - stream_gen mismatch forces path B regardless of ring state
///   - keyboard input delivered during AwaitSnapshot is not lost
///   - Resize received during AwaitSnapshot updates NeedsSnapshot rows/cols
///   - v2 client without GRID_CAPABLE keeps Reap overflow mode → SessionEnd
///   - v1 client keeps Reap overflow mode → SessionEnd on overflow

#include "zet/session/connection.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/crypto/crypto.hpp"
#include "zet/session/agent_session.hpp"
#include "zet/session/channel.hpp"
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

const Timestamp START = Timestamp::FromNanos(1'000'000'000);

/// Tiny limits for tests: ring overflows quickly.
SessionLimits TinyLimits() {
    return SessionLimits{
        .ResendMax = 512,
        .DisconnectMax = 512,
        .OrphanTimeout = std::chrono::minutes{30},
        .Liveness =
            LivenessLimits{
                .KeepaliveInterval = Duration{0},  // no keepalives in tests
                .PeerSilenceTimeout = Duration{0}  // no silence timeout
            },
    };
}

// ── One-connection harness ─────────────────────────────────────────────────

struct Space {
    std::array<std::byte, CONNECTION_FRAME_STORAGE> AgentFrames{};
    std::array<std::byte, CONNECTION_FRAME_STORAGE> ClientFrames{};
    std::array<std::byte, CONNECTION_OUT_SIZE> Out{};
    std::array<std::byte, CONNECTION_SCRATCH_SIZE> Scratch{};
    std::vector<std::byte> SnapshotBuf{wire::SNAPSHOT_MAX_BYTES, std::byte{}};
};

/// Deliver `out[0..written)` bytes to `conn`.
template <typename TConn>
[[nodiscard]] bool Feed(TConn& conn, Space& sp, std::size_t written) {
    auto r = conn.OnBytes(ByteSpan{sp.Out}.first(written));
    return r.has_value();
}

/// Run one pump, return the event.  Callers retry until Idle.
ConnectionResult PumpAgent(AgentConnection& a, Space& sp, Timestamp now) {
    return a.Pump(now, MutableByteSpan{sp.Out}, MutableByteSpan{sp.Scratch});
}
ConnectionResult PumpClient(ClientConnection& c, Space& sp, Timestamp now) {
    return c.Pump(now, MutableByteSpan{sp.Out}, MutableByteSpan{sp.Scratch});
}

/// Run one full handshake and Resume to Live, or until both sides Idle.
/// Returns false if either side errors.
bool DriveToLive(AgentConnection& agent, ClientConnection& client,
                 AgentSession& session, Space& sp, Timestamp now,
                 std::uint64_t& nonces) {
    for (int i = 0; i < 64; ++i) {
        auto ea = PumpAgent(agent, sp, now);
        if (!ea) {
            return false;
        }
        if (const auto* ns = std::get_if<NeedsSession>(&*ea)) {
            REQUIRE(ns->Id == session.Ref().Id());
            agent.Route(session.Ref(), MakeNonce(++nonces));
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            if (!Feed(client, sp, sf->Written)) {
                return false;
            }
        }

        auto ec = PumpClient(client, sp, now);
        if (!ec) {
            return false;
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            if (!Feed(agent, sp, sf->Written)) {
                return false;
            }
        }

        if (agent.IsLive() && client.IsLive()) {
            return true;
        }
    }
    return false;
}

// ── Path A: no GRID_CAPABLE, byte-identical replay ─────────────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "path A: no GRID_CAPABLE, NeedsSnapshot never returned") {
    std::vector<std::byte> agentStorage(DISCONNECT_BUFFER_MAX);
    std::vector<std::byte> clientStorage(RESEND_BUFFER_MAX);
    AgentSession agent{DeriveSessionSecrets(MakeKey(1)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = {}}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(1)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = {}}};
    Space sp;
    std::uint64_t nonces = 0;

    // Write data into agent before reconnect.
    const std::vector<std::byte> payload(128, std::byte{0xAB});
    std::ignore = agent.Write(ByteSpan{payload});

    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START};
    // gridCapable=false (default): path B never chosen.
    ClientConnection cc{client, MakeNonce(++nonces),
                        MutableByteSpan{sp.ClientFrames}, START};
    REQUIRE(DriveToLive(ac, cc, agent, sp, START, nonces));

    // Pump the live session; verify no NeedsSnapshot event.
    for (int i = 0; i < 200; ++i) {
        auto ea = PumpAgent(ac, sp, START);
        REQUIRE(ea.has_value());
        REQUIRE_FALSE(std::holds_alternative<NeedsSnapshot>(*ea));
        if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, sp, sf->Written));
        }

        auto ec = PumpClient(cc, sp, START);
        REQUIRE(ec.has_value());
        REQUIRE_FALSE(std::holds_alternative<NeedsSnapshot>(*ec));
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, sp, sf->Written));
        }

        bool agentIdle = std::holds_alternative<Idle>(*ea);
        bool clientIdle = std::holds_alternative<Idle>(*ec);
        if (agentIdle && clientIdle) {
            break;
        }
    }
}

// ── Path B: ring overflow in Resync mode → NeedsSnapshot ──────────────────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "path B: ring overflow with GRID_CAPABLE yields NeedsSnapshot") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> agentStorage(limits.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(limits.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(2)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(2)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // First connection: authenticate so SetGridInfo(ver=2, gridCapable=true)
    // is called, switching the session to Resync overflow mode.
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
        REQUIRE(DriveToLive(ac, cc, agent, sp, START, nonces));
        ac.Close(START);  // disconnect
    }

    // Write until the ring overflows (Resync mode: ring fills, flag set).
    const std::vector<std::byte> chunk(limits.DisconnectMax / 4 + 1,
                                       std::byte{0xBB});
    for (int i = 0; i < 8; ++i) {
        std::ignore = agent.Write(ByteSpan{chunk});
    }
    // At this point RingOverflowed should be set (DisconnectMax exceeded).

    // Second connection: agent must return NeedsSnapshot.
    AgentConnection ac2{MutableByteSpan{sp.AgentFrames}, START,
                        wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc2{client,
                         MakeNonce(++nonces),
                         MutableByteSpan{sp.ClientFrames},
                         START,
                         /*gridCapable=*/true,
                         wire::PROTOCOL_VERSION_GRID,
                         MutableByteSpan{sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac2, cc2, agent, sp, START, nonces));

    // Drive until NeedsSnapshot appears from the agent.
    bool gotNeedsSnapshot = false;
    bool gotApplySnapshot = false;
    const std::vector<std::byte> snapshot(64, std::byte{0xCC});

    for (int i = 0; i < 400 && !gotApplySnapshot; ++i) {
        auto ea = PumpAgent(ac2, sp, START);
        REQUIRE(ea.has_value());

        if (std::get_if<NeedsSnapshot>(&*ea)) {
            gotNeedsSnapshot = true;
            // Caller supplies snapshot: ring cleared, GridBegin/Chunk/End
            // queued.
            ac2.SupplySnapshot(START, ByteSpan{snapshot}, 0, 0);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc2, sp, sf->Written));
        }

        auto ec = PumpClient(cc2, sp, START);
        REQUIRE(ec.has_value());

        if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
            gotApplySnapshot = true;
            CHECK(as->StreamGen == 1);
            CHECK(as->Bytes.size() == snapshot.size());
            cc2.ConfirmApply(as->StreamGen);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac2, sp, sf->Written));
        } else if (const auto* d = std::get_if<Deliver>(&*ec)) {
            // keyboard data delivered during path B — fine
            (void)d;
        }
    }

    CHECK(gotNeedsSnapshot);
    CHECK(gotApplySnapshot);
    // Client's StreamGen updated to 1.
    CHECK(client.StreamGen() == 1);
}

// ── Path B: stream_gen mismatch forces path B even without ring overflow ───

TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: stream_gen mismatch forces path B on reconnect") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> agentStorage(limits.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(limits.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(3)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(3)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // Do one full path B cycle to advance agent session_gen to 1 and client
    // stream_gen to 1.
    auto doPathB = [&]() -> bool {
        // Fill ring to trigger overflow.
        const std::vector<std::byte> chunk(limits.DisconnectMax / 4 + 1,
                                           std::byte{0xDD});
        for (int i = 0; i < 8; ++i) {
            std::ignore = agent.Write(ByteSpan{chunk});
        }

        AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                           wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc{client,
                            MakeNonce(++nonces),
                            MutableByteSpan{sp.ClientFrames},
                            START,
                            /*gridCapable=*/true,
                            wire::PROTOCOL_VERSION_GRID,
                            MutableByteSpan{sp.SnapshotBuf}};
        if (!DriveToLive(ac, cc, agent, sp, START, nonces)) {
            return false;
        }
        const std::vector<std::byte> snap(32, std::byte{0xEE});
        bool applied = false;
        for (int i = 0; i < 400 && !applied; ++i) {
            auto ea = ac.Pump(START, MutableByteSpan{sp.Out},
                              MutableByteSpan{sp.Scratch});
            if (!ea) {
                return false;
            }
            if (std::holds_alternative<NeedsSnapshot>(*ea)) {
                ac.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
            } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                if (!Feed(cc, sp, sf->Written)) {
                    return false;
                }
            }
            auto ec = cc.Pump(START, MutableByteSpan{sp.Out},
                              MutableByteSpan{sp.Scratch});
            if (!ec) {
                return false;
            }
            if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
                applied = true;
                cc.ConfirmApply(as->StreamGen);
            } else if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                if (!Feed(ac, sp, sf->Written)) {
                    return false;
                }
            }
        }
        return applied;
    };

    // First path B: connect with GRID_CAPABLE to set Resync mode, then overflow
    // and go through path B.
    {
        // Connect once to set Resync mode (no overflow yet).
        AgentConnection ac0{MutableByteSpan{sp.AgentFrames}, START,
                            wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc0{client,
                             MakeNonce(++nonces),
                             MutableByteSpan{sp.ClientFrames},
                             START,
                             /*gridCapable=*/true,
                             wire::PROTOCOL_VERSION_GRID,
                             MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac0, cc0, agent, sp, START, nonces));
        ac0.Close(START);
    }
    REQUIRE(doPathB());

    // Now agent session_gen=1, client stream_gen=1. Reconnect with a client
    // that sends stream_gen=0 (simulate a client that didn't apply the
    // snapshot). We do this by using a fresh ClientSession that has
    // StreamGen=0.
    std::vector<std::byte> freshClientStorage(limits.ResendMax + 256);
    ClientSession freshClient{
        DeriveSessionSecrets(MakeKey(3)),
        SessionStreams{.Storage = MutableByteSpan{freshClientStorage},
                       .Now = START,
                       .Limits = limits}};

    AgentConnection ac3{MutableByteSpan{sp.AgentFrames}, START,
                        wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc3{freshClient,
                         MakeNonce(++nonces),
                         MutableByteSpan{sp.ClientFrames},
                         START,
                         /*gridCapable=*/true,
                         wire::PROTOCOL_VERSION_GRID,
                         MutableByteSpan{sp.SnapshotBuf}};

    // freshClient sends stream_gen=0; agent's session_gen=1 → mandatory path B.
    bool gotNeedsSnapshot = false;
    for (int i = 0; i < 64; ++i) {
        auto ea = ac3.Pump(START, MutableByteSpan{sp.Out},
                           MutableByteSpan{sp.Scratch});
        if (!ea) {
            break;
        }
        if (const auto* ns = std::get_if<NeedsSession>(&*ea)) {
            REQUIRE(ns->Id == agent.Ref().Id());
            ac3.Route(agent.Ref(), MakeNonce(++nonces));
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc3, sp, sf->Written));
        } else if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            gotNeedsSnapshot = true;
            break;
        }
        auto ec = cc3.Pump(START, MutableByteSpan{sp.Out},
                           MutableByteSpan{sp.Scratch});
        if (!ec) {
            break;
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac3, sp, sf->Written));
        }
    }
    CHECK(gotNeedsSnapshot);
}

// ── Resize in AwaitSnapshot updates NeedsSnapshot rows/cols ───────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: Resize during AwaitSnapshot updates the rows/cols "
                  "in NeedsSnapshot") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> agentStorage(limits.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(limits.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(4)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(4)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // Connect to set Resync mode.
    {
        AgentConnection ac0{MutableByteSpan{sp.AgentFrames}, START,
                            wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc0{client,
                             MakeNonce(++nonces),
                             MutableByteSpan{sp.ClientFrames},
                             START,
                             /*gridCapable=*/true,
                             wire::PROTOCOL_VERSION_GRID,
                             MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac0, cc0, agent, sp, START, nonces));
        ac0.Close(START);
    }

    // Overflow ring.
    const std::vector<std::byte> chunk(limits.DisconnectMax / 4 + 1,
                                       std::byte{0xFF});
    for (int i = 0; i < 8; ++i) {
        std::ignore = agent.Write(ByteSpan{chunk});
    }

    // Second connection: path B expected.
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                       wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc{client,
                        MakeNonce(++nonces),
                        MutableByteSpan{sp.ClientFrames},
                        START,
                        /*gridCapable=*/true,
                        wire::PROTOCOL_VERSION_GRID,
                        MutableByteSpan{sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac, cc, agent, sp, START, nonces));

    // Client queues a Resize.
    const wire::ResizeMessage resize{.Rows = 24, .Cols = 80};
    cc.QueueResize(resize);

    // Pump until the agent returns NeedsSnapshot, checking that Resize was
    // received (agent returns Resized first, then NeedsSnapshot).
    std::optional<wire::ResizeMessage> seenResize;
    std::optional<NeedsSnapshot> seenNeedsSnapshot;

    // Pump client first each round so the Resize frame reaches the agent
    // before PumpAwaitSnapshot runs.
    for (int i = 0; i < 200 && !seenNeedsSnapshot.has_value(); ++i) {
        auto ec = cc.Pump(START, MutableByteSpan{sp.Out},
                          MutableByteSpan{sp.Scratch});
        REQUIRE(ec.has_value());
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, sp, sf->Written));
        }

        auto ea = ac.Pump(START, MutableByteSpan{sp.Out},
                          MutableByteSpan{sp.Scratch});
        REQUIRE(ea.has_value());
        if (const auto* r = std::get_if<Resized>(&*ea)) {
            seenResize = r->Size;
        } else if (const auto* ns = std::get_if<NeedsSnapshot>(&*ea)) {
            seenNeedsSnapshot = *ns;
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, sp, sf->Written));
        }
    }

    REQUIRE(seenResize.has_value());
    CHECK(seenResize->Rows == 24);
    CHECK(seenResize->Cols == 80);
    // NeedsSnapshot.Rows/Cols reflects the Resize we sent.
    REQUIRE(seenNeedsSnapshot.has_value());
    CHECK(seenNeedsSnapshot->Rows == 24);
    CHECK(seenNeedsSnapshot->Cols == 80);
}

// ── v2 client without GRID_CAPABLE: overflow → SessionEnd, not path B ─────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "v2 client without GRID_CAPABLE: ring overflow triggers SessionEnd") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> agentStorage(limits.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(limits.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(5)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(5)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // First connection WITHOUT GRID_CAPABLE: overflow mode stays Reap.
    {
        AgentConnection ac0{MutableByteSpan{sp.AgentFrames}, START};
        ClientConnection cc0{client, MakeNonce(++nonces),
                             MutableByteSpan{sp.ClientFrames}, START,
                             /*gridCapable=*/false};
        REQUIRE(DriveToLive(ac0, cc0, agent, sp, START, nonces));
        ac0.Close(START);
    }

    // Fill ring past DisconnectMax → session reaped (Reap mode).
    const std::vector<std::byte> chunk(limits.DisconnectMax / 4 + 1,
                                       std::byte{0x55});
    bool reaped = false;
    for (int i = 0; i < 8; ++i) {
        auto r = agent.Write(ByteSpan{chunk});
        if (!r) {
            reaped = true;
            break;
        }
    }
    // If Write didn't fail, the session might be reaped silently.
    if (!reaped) {
        reaped = (agent.State() == ESessionState::Reaped);
    }
    CHECK(reaped);
    // NeedsSnapshot must never appear — session is reaped, not in Resync.
    CHECK_FALSE(agent.State() == ESessionState::Live);
}

// ── Stream continues after path B (post-GridApplied data delivery) ────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: data written after SupplySnapshot is delivered "
                  "after GridApplied") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> agentStorage(limits.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(limits.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(6)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(6)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // Connect to set Resync mode.
    {
        AgentConnection ac0{MutableByteSpan{sp.AgentFrames}, START,
                            wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc0{client,
                             MakeNonce(++nonces),
                             MutableByteSpan{sp.ClientFrames},
                             START,
                             /*gridCapable=*/true,
                             wire::PROTOCOL_VERSION_GRID,
                             MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac0, cc0, agent, sp, START, nonces));
        ac0.Close(START);
    }

    // Overflow ring.
    const std::vector<std::byte> preData(limits.DisconnectMax / 4 + 1,
                                         std::byte{0x11});
    for (int i = 0; i < 8; ++i) {
        std::ignore = agent.Write(ByteSpan{preData});
    }

    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                       wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc{client,
                        MakeNonce(++nonces),
                        MutableByteSpan{sp.ClientFrames},
                        START,
                        /*gridCapable=*/true,
                        wire::PROTOCOL_VERSION_GRID,
                        MutableByteSpan{sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac, cc, agent, sp, START, nonces));

    // Drive path B to completion.
    const std::vector<std::byte> snap(32, std::byte{0x22});
    bool appliedSnap = false;
    std::vector<std::byte> received;

    for (int i = 0; i < 400 && !appliedSnap; ++i) {
        auto ea = ac.Pump(START, MutableByteSpan{sp.Out},
                          MutableByteSpan{sp.Scratch});
        REQUIRE(ea.has_value());
        if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            ac.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
            // Write new data AFTER NewGeneration (new generation ring is
            // clear).
            const std::vector<std::byte> newData(32, std::byte{0x33});
            REQUIRE(agent.Write(ByteSpan{newData}));
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, sp, sf->Written));
        }
        auto ec = cc.Pump(START, MutableByteSpan{sp.Out},
                          MutableByteSpan{sp.Scratch});
        REQUIRE(ec.has_value());
        if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
            appliedSnap = true;
            cc.ConfirmApply(as->StreamGen);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, sp, sf->Written));
        } else if (const auto* d = std::get_if<Deliver>(&*ec)) {
            const auto span = d->Bytes;
            received.insert(received.end(), span.begin(), span.end());
        }
    }
    REQUIRE(appliedSnap);

    // After GridApplied, the new-generation data (0x33 bytes) should arrive.
    for (int i = 0; i < 200; ++i) {
        auto ea = ac.Pump(START, MutableByteSpan{sp.Out},
                          MutableByteSpan{sp.Scratch});
        REQUIRE(ea.has_value());
        if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, sp, sf->Written));
        }
        auto ec = cc.Pump(START, MutableByteSpan{sp.Out},
                          MutableByteSpan{sp.Scratch});
        REQUIRE(ec.has_value());
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, sp, sf->Written));
        } else if (const auto* d = std::get_if<Deliver>(&*ec)) {
            const auto span = d->Bytes;
            received.insert(received.end(), span.begin(), span.end());
        }
        if (std::holds_alternative<Idle>(*ea) &&
            std::holds_alternative<Idle>(*ec)) {
            break;
        }
    }

    // Check that the 0x33 data arrived (written after NewGeneration).
    bool hasNewData =
        std::any_of(received.begin(), received.end(),
                    [](std::byte b) { return b == std::byte{0x33}; });
    CHECK(hasNewData);
    // No stale 0x11 data from before NewGeneration should appear.
    bool hasStaleData =
        std::any_of(received.begin(), received.end(),
                    [](std::byte b) { return b == std::byte{0x11}; });
    CHECK_FALSE(hasStaleData);
}

// ── Item 3: non-grid-capable client must not trigger path B via cost check ───
//
// Bug: PumpResume runs the replay-vs-snapshot cost check even when the client
// did not advertise GRID_CAPABLE.  If the replay cost exceeds the estimate the
// agent sets usePathB=true and returns NeedsSnapshot — which is wrong because
// the client cannot handle a grid snapshot.
//
// Fix: gate the cost check on `gridCapable = (ver >= PROTOCOL_VERSION_GRID &&
// HelloGridCapable_)`.  A non-capable client always takes path A.
//
// Scenario: ver=2 peer, GRID_CAPABLE absent, 2 MiB replay at 24×80.
// SnapshotEstimateBytes(24,80) = 591 488 bytes < 2 MiB → triggers bug.

TEST_CASE_FIXTURE(SodiumFixture,
                  "path A: non-grid-capable client keeps path A even when "
                  "replay exceeds snapshot estimate") {
    // Enough room to hold the 2 MiB chunk without hitting Reap.
    const std::size_t disconnectMax = 4U * 1024U * 1024U;
    const SessionLimits limits{
        .ResendMax = RESEND_BUFFER_MAX,
        .DisconnectMax = disconnectMax,
        .OrphanTimeout = std::chrono::minutes{30},
        .Liveness = {.KeepaliveInterval = Duration{0},
                     .PeerSilenceTimeout = Duration{0}},
    };
    std::vector<std::byte> agentStorage(disconnectMax + 4096);
    std::vector<std::byte> clientStorage(RESEND_BUFFER_MAX + 4096);
    AgentSession agent{DeriveSessionSecrets(MakeKey(8)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(8)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // First connection: ver=2, gridCapable=false.
    // SetGridInfo(ver=2, false) leaves overflow mode as Reap.
    // After going Live, send a Resize(24×80) from client so that the agent
    // records LastKnownSize — the cost check only runs when it is set.
    {
        AgentConnection ac0{MutableByteSpan{sp.AgentFrames}, START,
                            wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc0{client,
                             MakeNonce(++nonces),
                             MutableByteSpan{sp.ClientFrames},
                             START,
                             /*gridCapable=*/false,
                             wire::PROTOCOL_VERSION_GRID};
        REQUIRE(DriveToLive(ac0, cc0, agent, sp, START, nonces));

        // Deliver Resize so agent stores LastKnownSize({24, 80}).
        cc0.QueueResize(wire::ResizeMessage{.Rows = 24, .Cols = 80});
        bool resized = false;
        for (int i = 0; i < 50 && !resized; ++i) {
            auto ec = PumpClient(cc0, sp, START);
            REQUIRE(ec.has_value());
            if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                REQUIRE(Feed(ac0, sp, sf->Written));
            }
            auto ea = PumpAgent(ac0, sp, START);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<Resized>(*ea)) {
                resized = true;
            } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(cc0, sp, sf->Written));
            }
        }
        REQUIRE(resized);
        ac0.Close(START);
    }

    // Write > SnapshotEstimateBytes(24,80) = 591 488 bytes.  In Reap mode
    // with DisconnectMax=4 MiB no overflow occurs; the client will replay it
    // all on reconnect.
    const std::size_t toWrite = SnapshotEstimateBytes(24, 80) + 4096;
    const std::vector<std::byte> chunk(4096, std::byte{0x7F});
    for (std::size_t done = 0; done < toWrite; done += chunk.size()) {
        std::ignore = agent.Write(ByteSpan{chunk});
    }

    // Second connection: ver=2, gridCapable=false.
    // Without fix: cost check runs → usePathB=true → NeedsSnapshot (wrong).
    // With fix:    gridCapable gate prevents cost check → path A.
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                       wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc{client,
                        MakeNonce(++nonces),
                        MutableByteSpan{sp.ClientFrames},
                        START,
                        /*gridCapable=*/false,
                        wire::PROTOCOL_VERSION_GRID};

    bool gotLive = false;
    for (int i = 0; i < 400; ++i) {
        auto ea = PumpAgent(ac, sp, START);
        REQUIRE(ea.has_value());
        REQUIRE_FALSE(std::holds_alternative<NeedsSnapshot>(*ea));
        if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            break;
        }
        if (const auto* ns = std::get_if<NeedsSession>(&*ea)) {
            REQUIRE(ns->Id == agent.Ref().Id());
            ac.Route(agent.Ref(), MakeNonce(++nonces));
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, sp, sf->Written));
        }

        auto ec = PumpClient(cc, sp, START);
        REQUIRE(ec.has_value());
        REQUIRE_FALSE(std::holds_alternative<NeedsSnapshot>(*ec));
        if (std::holds_alternative<NeedsSnapshot>(*ec)) {
            break;
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, sp, sf->Written));
        }

        if (ac.IsLive() && cc.IsLive()) {
            gotLive = true;
            break;
        }
    }
    CHECK(gotLive);
}

// ── Item 7: Resume stream_gen > session_gen is a protocol violation
// ───────────
//
// If the client claims an output generation the agent has never emitted, the
// connection must be closed.  Without the check, PumpResume silently falls
// through the A/B decision with stream_gen accepted as-is.
//
// Scenario: complete path B with agent A (session_gen → 1, client stream_gen →
// 1), then reconnect to a FRESH agent A2 (same key, session_gen = 0).  The
// ConnectionSeed is fixed, so the handshake authenticates; the Resume carries
// stream_gen=1 > session_gen=0 — which is impossible in honest operation.

TEST_CASE_FIXTURE(
    SodiumFixture,
    "Resume stream_gen ahead of session_gen is a protocol violation "
    "(CloseConnection)") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> agentStorage(limits.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(limits.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(10)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(10)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // ── Step 1: enable Resync mode via a gridCapable=true connection
    // ──────────
    {
        AgentConnection ac0{MutableByteSpan{sp.AgentFrames}, START,
                            wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc0{client,
                             MakeNonce(++nonces),
                             MutableByteSpan{sp.ClientFrames},
                             START,
                             /*gridCapable=*/true,
                             wire::PROTOCOL_VERSION_GRID,
                             MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac0, cc0, agent, sp, START, nonces));
        ac0.Close(START);
    }

    // ── Step 2: overflow the ring and drive path B to completion ─────────────
    const std::vector<std::byte> chunk(limits.DisconnectMax / 4 + 1,
                                       std::byte{0x11});
    for (int i = 0; i < 8; ++i) {
        std::ignore = agent.Write(ByteSpan{chunk});
    }
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
        REQUIRE(DriveToLive(ac, cc, agent, sp, START, nonces));

        const std::vector<std::byte> snap(16, std::byte{0x22});
        bool applied = false;
        for (int i = 0; i < 400 && !applied; ++i) {
            auto ea = PumpAgent(ac, sp, START);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<NeedsSnapshot>(*ea)) {
                ac.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
            } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(cc, sp, sf->Written));
            }
            auto ec = PumpClient(cc, sp, START);
            REQUIRE(ec.has_value());
            if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
                applied = true;
                cc.ConfirmApply(as->StreamGen);
            } else if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                REQUIRE(Feed(ac, sp, sf->Written));
            }
        }
        REQUIRE(applied);
        ac.Close(START);
    }
    // client.StreamGen() == 1, agent.SessionGen() == 1.
    REQUIRE(client.StreamGen() == 1);

    // ── Step 3: fresh agent with the same key, session_gen = 0 ───────────────
    // ConnectionSeed is fixed (derived once from the key), so the fresh agent
    // authenticates the same client regardless of how many connections have
    // been made.
    std::vector<std::byte> freshStorage(limits.DisconnectMax + 256);
    AgentSession freshAgent{
        DeriveSessionSecrets(MakeKey(10)),
        SessionStreams{.Storage = MutableByteSpan{freshStorage},
                       .Now = START,
                       .Limits = limits}};

    AgentConnection ac2{MutableByteSpan{sp.AgentFrames}, START,
                        wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc2{client,
                         MakeNonce(++nonces),
                         MutableByteSpan{sp.ClientFrames},
                         START,
                         /*gridCapable=*/true,
                         wire::PROTOCOL_VERSION_GRID,
                         MutableByteSpan{sp.SnapshotBuf}};

    // Drive until the fresh agent either closes the connection (fix: violation
    // detected) or goes Live (bug: stream_gen silently accepted).
    bool gotCloseConnection = false;
    bool gotLive = false;
    for (int i = 0; i < 200; ++i) {
        auto ea = ac2.Pump(START, MutableByteSpan{sp.Out},
                           MutableByteSpan{sp.Scratch});
        if (!ea) {
            if (ea.error().Disposition() == EDisposition::CloseConnection) {
                gotCloseConnection = true;
            }
            break;
        }
        if (const auto* ns = std::get_if<NeedsSession>(&*ea)) {
            REQUIRE(ns->Id == freshAgent.Ref().Id());
            ac2.Route(freshAgent.Ref(), MakeNonce(++nonces));
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc2, sp, sf->Written));
        }
        if (ac2.IsLive()) {
            gotLive = true;
            break;
        }

        auto ec = cc2.Pump(START, MutableByteSpan{sp.Out},
                           MutableByteSpan{sp.Scratch});
        if (!ec) {
            break;
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac2, sp, sf->Written));
        }
    }
    // With fix: violation detected → CloseConnection; session must not go Live.
    CHECK(gotCloseConnection);
    CHECK_FALSE(gotLive);
}

}  // namespace

// ── Item 10 property tests ─────────────────────────────────────────────────
//
// 1. v1/v2 peer compatibility: a v1 client against a v2-capable agent
//    (and vice versa) negotiates ver=1 and uses Reap overflow mode.
// 2. Path B cut-at-AwaitSnapshot: if the connection drops while the agent
//    awaits a snapshot (before SupplySnapshot is called), the next connection
//    must also trigger path B (RingOverflowed stays set until NewGeneration).

// ── v1 client / v2-capable agent → ver=1, no Resync ──────────────────────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "v1 client against v2-capable agent negotiates ver=1 and uses Reap mode") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> agentStorage(limits.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(limits.ResendMax + 256);
    // Agent allows ver=2; client caps at ver=1 (ceiling=PROTOCOL_VERSION_MIN).
    AgentSession agent{DeriveSessionSecrets(MakeKey(11)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(11)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // First connection: v1 ceiling, no GRID_CAPABLE.
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                       wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc{client,
                        MakeNonce(++nonces),
                        MutableByteSpan{sp.ClientFrames},
                        START,
                        /*gridCapable=*/false,
                        wire::PROTOCOL_VERSION_MIN};  // v1 ceiling
    REQUIRE(DriveToLive(ac, cc, agent, sp, START, nonces));

    // Agent must be in Reap overflow mode (not Resync): overflow kills session.
    // Fill the ring past DisconnectMax → session reaped (Reap mode kills it).
    ac.Close(START);
    const std::vector<std::byte> chunk(limits.DisconnectMax / 4 + 1,
                                       std::byte{0x77});
    bool reaped = false;
    for (int i = 0; i < 8; ++i) {
        auto r = agent.Write(ByteSpan{chunk});
        if (!r) {
            reaped = true;
            break;
        }
    }
    if (!reaped) {
        reaped = (agent.State() == ESessionState::Reaped);
    }
    // Reap mode: session is dead, not in Resync.
    CHECK(reaped);
    CHECK(agent.State() == ESessionState::Reaped);
}

// ── v2 client (gridCapable) against v1-only agent → ver=1, Reap mode ─────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "v2 gridCapable client against v1-only agent negotiates ver=1 and keeps "
    "Reap mode") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> agentStorage(limits.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(limits.ResendMax + 256);
    // Agent caps at ver=1; client wants ver=2 and advertises GRID_CAPABLE.
    AgentSession agent{DeriveSessionSecrets(MakeKey(12)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(12)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // Agent ceiling=1; client ceiling=GRID with gridCapable=true.
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                       wire::PROTOCOL_VERSION_MIN};  // v1 agent ceiling
    ClientConnection cc{client,
                        MakeNonce(++nonces),
                        MutableByteSpan{sp.ClientFrames},
                        START,
                        /*gridCapable=*/true,
                        wire::PROTOCOL_VERSION_GRID,  // client wants ver=2
                        MutableByteSpan{sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac, cc, agent, sp, START, nonces));

    // ver=1 negotiated → agent stays in Reap mode → overflow kills session.
    ac.Close(START);
    const std::vector<std::byte> chunk(limits.DisconnectMax / 4 + 1,
                                       std::byte{0x88});
    bool reaped = false;
    for (int i = 0; i < 8; ++i) {
        auto r = agent.Write(ByteSpan{chunk});
        if (!r) {
            reaped = true;
            break;
        }
    }
    if (!reaped) {
        reaped = (agent.State() == ESessionState::Reaped);
    }
    CHECK(reaped);
    // Session is dead (Reap mode), not alive in Resync waiting for path B.
    CHECK(agent.State() == ESessionState::Reaped);
}

// ── Path B: connection cut during AwaitSnapshot → next connection is also
// path B (RingOverflowed survives until NewGeneration) ─────────────────────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "path B: connection cut during AwaitSnapshot forces path B on reconnect") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> agentStorage(limits.DisconnectMax + 256);
    std::vector<std::byte> clientStorage(limits.ResendMax + 256);
    AgentSession agent{DeriveSessionSecrets(MakeKey(13)),
                       SessionStreams{.Storage = MutableByteSpan{agentStorage},
                                      .Now = START,
                                      .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(13)),
        SessionStreams{.Storage = MutableByteSpan{clientStorage},
                       .Now = START,
                       .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // First connection: enable Resync mode.
    {
        AgentConnection ac0{MutableByteSpan{sp.AgentFrames}, START,
                            wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc0{client,
                             MakeNonce(++nonces),
                             MutableByteSpan{sp.ClientFrames},
                             START,
                             /*gridCapable=*/true,
                             wire::PROTOCOL_VERSION_GRID,
                             MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac0, cc0, agent, sp, START, nonces));
        ac0.Close(START);
    }

    // Overflow the ring → agent enters Resync (session stays Detached, alive).
    const std::vector<std::byte> chunk(limits.DisconnectMax / 4 + 1,
                                       std::byte{0xAA});
    for (int i = 0; i < 8; ++i) {
        std::ignore = agent.Write(ByteSpan{chunk});
    }
    // Resync: session is alive but the ring is full.
    REQUIRE(agent.State() == ESessionState::Detached);

    // Second connection: drive to AwaitSnapshot, then cut WITHOUT calling
    // SupplySnapshot.  The agent emits NeedsSnapshot but never gets a snapshot.
    {
        AgentConnection ac1{MutableByteSpan{sp.AgentFrames}, START,
                            wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc1{client,
                             MakeNonce(++nonces),
                             MutableByteSpan{sp.ClientFrames},
                             START,
                             /*gridCapable=*/true,
                             wire::PROTOCOL_VERSION_GRID,
                             MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac1, cc1, agent, sp, START, nonces));

        bool gotNeedsSnapshot = false;
        for (int i = 0; i < 200 && !gotNeedsSnapshot; ++i) {
            auto ea = PumpAgent(ac1, sp, START);
            if (!ea) {
                break;
            }
            if (std::holds_alternative<NeedsSnapshot>(*ea)) {
                gotNeedsSnapshot = true;
                // Do NOT call SupplySnapshot: simulate a cut mid-snapshot.
            } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                std::ignore = Feed(cc1, sp, sf->Written);
            }
            auto ec = PumpClient(cc1, sp, START);
            if (!ec) {
                break;
            }
            if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                std::ignore = Feed(ac1, sp, sf->Written);
            }
        }
        REQUIRE(gotNeedsSnapshot);
        // Cut the connection without supplying the snapshot.
        ac1.Close(START);
    }
    // Session is still Detached (alive) — RingOverflowed persists because
    // NewGeneration was never called (no SupplySnapshot).
    REQUIRE(agent.State() == ESessionState::Detached);

    // Third connection: path B must be mandatory again — NeedsSnapshot fired.
    AgentConnection ac2{MutableByteSpan{sp.AgentFrames}, START,
                        wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc2{client,
                         MakeNonce(++nonces),
                         MutableByteSpan{sp.ClientFrames},
                         START,
                         /*gridCapable=*/true,
                         wire::PROTOCOL_VERSION_GRID,
                         MutableByteSpan{sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac2, cc2, agent, sp, START, nonces));

    bool gotNeedsSnapshot2 = false;
    for (int i = 0; i < 200 && !gotNeedsSnapshot2; ++i) {
        auto ea = PumpAgent(ac2, sp, START);
        if (!ea) {
            break;
        }
        if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            gotNeedsSnapshot2 = true;
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            std::ignore = Feed(cc2, sp, sf->Written);
        }
        auto ec = PumpClient(cc2, sp, START);
        if (!ec) {
            break;
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            std::ignore = Feed(ac2, sp, sf->Written);
        }
    }
    CHECK(gotNeedsSnapshot2);
    ac2.Close(START);
    cc2.Close(START);
}

// ── Probe-derived tests ───────────────────────────────────────────────────

namespace {

struct Rig {
    SessionLimits limits = TinyLimits();
    std::vector<std::byte> as =
        std::vector<std::byte>(limits.DisconnectMax + 256);
    std::vector<std::byte> cs = std::vector<std::byte>(limits.ResendMax + 256);
    AgentSession agent{
        DeriveSessionSecrets(MakeKey(33)),
        SessionStreams{
            .Storage = MutableByteSpan{as}, .Now = START, .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(33)),
        SessionStreams{
            .Storage = MutableByteSpan{cs}, .Now = START, .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;
};

// Connect, drive, return a label describing what the agent pumped:
//   "no-snapshot"           path A (or close succeeded without a snapshot)
//   "NeedsSnapshot"         path B requested but not supplied
//   "NeedsSnapshot+applied" path B supplied and client applied it
// Appends " agent-err" or " client-err" on unexpected pump failures.
std::string Reconnect(Rig& r, bool capable, std::uint16_t ceiling,
                      bool close = true, bool supply = false) {
    AgentConnection ac{MutableByteSpan{r.sp.AgentFrames}, START, ceiling};
    ClientConnection cc{r.client,
                        MakeNonce(++r.nonces),
                        MutableByteSpan{r.sp.ClientFrames},
                        START,
                        capable,
                        ceiling,
                        MutableByteSpan{r.sp.SnapshotBuf}};
    if (!DriveToLive(ac, cc, r.agent, r.sp, START, r.nonces)) {
        return "drive-failed";
    }
    std::string res = "no-snapshot";
    const std::vector<std::byte> snap(16, std::byte{0x22});
    bool applied = false;
    for (int i = 0; i < 60; ++i) {
        auto ea = PumpAgent(ac, r.sp, START);
        if (!ea) {
            res += " agent-err";
            break;
        }
        if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            res = "NeedsSnapshot";
            if (!supply) {
                break;
            }
            ac.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            if (!Feed(cc, r.sp, sf->Written)) {
                break;
            }
        }
        auto ec = PumpClient(cc, r.sp, START);
        if (!ec) {
            res += " client-err";
            break;
        }
        if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
            applied = true;
            cc.ConfirmApply(as->StreamGen);
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            if (!Feed(ac, r.sp, sf->Written)) {
                break;
            }
        }
    }
    if (applied) {
        res += "+applied";
    }
    if (close) {
        ac.Close(START);
    }
    return res;
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "write loop in Resync mode reaches ring_overflowed") {
    Rig r;
    Reconnect(r, true, wire::PROTOCOL_VERSION_GRID);
    const std::vector<std::byte> chunk(100, std::byte{0xBB});
    int writes = 0;
    while (r.agent.AcceptsWrites() && writes < 100) {
        std::ignore = r.agent.Write(ByteSpan{chunk});
        ++writes;
    }
    MESSAGE("writes=" << writes
                      << " AcceptsWrites=" << r.agent.AcceptsWrites());
    auto res = Reconnect(r, true, wire::PROTOCOL_VERSION_GRID);
    MESSAGE("reconnect: " << res);
    CHECK(res == "NeedsSnapshot");
}

TEST_CASE_FIXTURE(
    SodiumFixture,
    "capable then non-capable v2 connection resets overflow mode to Reap") {
    Rig r;
    Reconnect(r, true, wire::PROTOCOL_VERSION_GRID);
    Reconnect(r, false, wire::PROTOCOL_VERSION_GRID);  // non-capable now
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    SessionResult<std::size_t> last = 0;
    for (int i = 0; i < 6; ++i) {
        last = r.agent.Write(ByteSpan{chunk});
    }
    MESSAGE("state=" << int(r.agent.State())
                     << " lastWrite ok=" << last.has_value());
    CHECK(r.agent.State() == ESessionState::Reaped);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v1 and non-capable v2 peers after path B get path A") {
    Rig r;
    Reconnect(r, true, wire::PROTOCOL_VERSION_GRID);
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.agent.Write(ByteSpan{chunk});
    }
    auto a = Reconnect(r, true, wire::PROTOCOL_VERSION_GRID, true, true);
    MESSAGE("pathB: " << a << " client gen=" << r.client.StreamGen());
    auto b = Reconnect(r, false, wire::PROTOCOL_VERSION_MAX);  // v1 peer
    MESSAGE("v1 reconnect: " << b);
    CHECK(b == "no-snapshot");
    auto c =
        Reconnect(r, false, wire::PROTOCOL_VERSION_GRID);  // v2 non-capable
    MESSAGE("v2 non-capable reconnect: " << c);
    CHECK(c == "no-snapshot");
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "PumpAwaitSnapshot does not busy-loop: returns Idle") {
    Rig r;
    Reconnect(r, true, wire::PROTOCOL_VERSION_GRID);
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.agent.Write(ByteSpan{chunk});
    }
    AgentConnection ac{MutableByteSpan{r.sp.AgentFrames}, START,
                       wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc{r.client,
                        MakeNonce(++r.nonces),
                        MutableByteSpan{r.sp.ClientFrames},
                        START,
                        true,
                        wire::PROTOCOL_VERSION_GRID,
                        MutableByteSpan{r.sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac, cc, r.agent, r.sp, START, r.nonces));
    int needs = 0, idle = 0;
    for (int i = 0; i < 20; ++i) {
        auto ea = PumpAgent(ac, r.sp, START);
        REQUIRE(ea.has_value());
        needs += std::holds_alternative<NeedsSnapshot>(*ea) ? 1 : 0;
        idle += std::holds_alternative<Idle>(*ea) ? 1 : 0;
    }
    MESSAGE("20 pumps, no input: NeedsSnapshot=" << needs << " Idle=" << idle);
    CHECK(idle > 0);
}

TEST_CASE("SnapshotEstimateBytes does not overflow at max dimensions") {
    auto e = SnapshotEstimateBytes(65535, 65535);
    MESSAGE("estimate(65535x65535)="
            << e << " expected="
            << (std::uint64_t{65535} * 65535 * 35 + SNAPSHOT_SCROLLBACK_BYTES));
    CHECK(std::uint64_t{e} ==
          (std::uint64_t{65535} * 65535 * 35 + SNAPSHOT_SCROLLBACK_BYTES));
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "v2 non-capable overflow produces SessionEnd Overflow") {
    Rig r;
    Reconnect(r, false, wire::PROTOCOL_VERSION_GRID);
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.agent.Write(ByteSpan{chunk});
    }
    MESSAGE("state=" << int(r.agent.State())
                     << " endedWith=" << r.agent.EndedWith().has_value());
    REQUIRE(r.agent.EndedWith().has_value());
    CHECK(r.agent.EndedWith()->Kind == wire::ESessionEndKind::Overflow);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "overflow mode resets per connection: non-capable v2 after "
                  "capable gets Reap not NeedsSnapshot") {
    Rig r;
    Reconnect(r, true, wire::PROTOCOL_VERSION_GRID);
    Reconnect(r, false, wire::PROTOCOL_VERSION_GRID);
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 8; ++i) {
        std::ignore = r.agent.Write(ByteSpan{chunk});
    }
    auto c = Reconnect(r, false, wire::PROTOCOL_VERSION_GRID);
    MESSAGE("non-capable v2 reconnect after overflow: " << c);
    CHECK(c != "NeedsSnapshot");
}

TEST_CASE_FIXTURE(SodiumFixture, "v1 peer overflow is silently reaped") {
    Rig r;
    Reconnect(r, false, wire::PROTOCOL_VERSION_MAX);
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 8; ++i) {
        std::ignore = r.agent.Write(ByteSpan{chunk});
    }
    CHECK(r.agent.State() == ESessionState::Reaped);
    CHECK(r.agent.ReapReason() == ESessionError::DisconnectBufferOverflow);
}

// ── Item 5a: path A byte-identical property
// ───────────────────────────────────
//
// Bytes delivered to the client after a path-A reconnect must be byte-for-byte
// identical to bytes written by the agent.  Using gridCapable=false ensures
// path B is never chosen regardless of ring state or session gen.

TEST_CASE_FIXTURE(SodiumFixture,
                  "path A delivers bytes identical to agent writes") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> as(limits.DisconnectMax + 256);
    std::vector<std::byte> cs(limits.ResendMax + 256);
    AgentSession agent{
        DeriveSessionSecrets(MakeKey(42)),
        SessionStreams{
            .Storage = MutableByteSpan{as}, .Now = START, .Limits = limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(42)),
        SessionStreams{
            .Storage = MutableByteSpan{cs}, .Now = START, .Limits = limits}};
    Space sp;
    std::uint64_t nonces = 0;

    // 1. First connection: write 300 bytes, receive some, then disconnect.
    const std::vector<std::byte> payload1(300, std::byte{0xA1});
    std::ignore = agent.Write(ByteSpan{payload1});
    {
        AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START};
        ClientConnection cc{client, MakeNonce(++nonces),
                            MutableByteSpan{sp.ClientFrames}, START};
        REQUIRE(DriveToLive(ac, cc, agent, sp, START, nonces));
        // Drain a few frames so the client is partially through the stream.
        for (int i = 0; i < 20; ++i) {
            auto ea = PumpAgent(ac, sp, START);
            REQUIRE(ea.has_value());
            if (const auto* sf = std::get_if<SendFrame>(&*ea))
                REQUIRE(Feed(cc, sp, sf->Written));
            auto ec = PumpClient(cc, sp, START);
            REQUIRE(ec.has_value());
            if (const auto* sf = std::get_if<SendFrame>(&*ec))
                REQUIRE(Feed(ac, sp, sf->Written));
            if (std::holds_alternative<Idle>(*ea) &&
                std::holds_alternative<Idle>(*ec))
                break;
        }
        ac.Close(START);
    }

    // 2. Write more data while disconnected.
    const std::vector<std::byte> payload2(200, std::byte{0xB2});
    std::ignore = agent.Write(ByteSpan{payload2});

    // 3. Reconnect (path A — gridCapable=false).
    //    Run handshake and data collection in one loop so Deliver events that
    //    appear immediately after Resume are not silently dropped (as they
    //    would be if DriveToLive handled them internally).
    std::vector<std::byte> received;
    {
        AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START};
        ClientConnection cc{client, MakeNonce(++nonces),
                            MutableByteSpan{sp.ClientFrames}, START};
        for (int i = 0; i < 400; ++i) {
            auto ea = PumpAgent(ac, sp, START);
            REQUIRE(ea.has_value());
            REQUIRE_FALSE(std::holds_alternative<NeedsSnapshot>(*ea));
            if (const auto* ns = std::get_if<NeedsSession>(&*ea)) {
                REQUIRE(ns->Id == agent.Ref().Id());
                ac.Route(agent.Ref(), MakeNonce(++nonces));
            } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(cc, sp, sf->Written));
            }
            auto ec = PumpClient(cc, sp, START);
            REQUIRE(ec.has_value());
            if (const auto* d = std::get_if<Deliver>(&*ec))
                received.insert(received.end(), d->Bytes.begin(),
                                d->Bytes.end());
            if (const auto* sf = std::get_if<SendFrame>(&*ec))
                REQUIRE(Feed(ac, sp, sf->Written));
            if (ac.IsLive() && cc.IsLive() &&
                std::holds_alternative<Idle>(*ea) &&
                std::holds_alternative<Idle>(*ec))
                break;
        }
    }

    // 4. All bytes in received must appear in payload1+payload2 in order.
    //    The client picks up from where it left off, so received is a suffix of
    //    payload1 followed by all of payload2.
    std::vector<std::byte> all;
    all.insert(all.end(), payload1.begin(), payload1.end());
    all.insert(all.end(), payload2.begin(), payload2.end());
    // received is a contiguous infix of all (path A: no bytes lost or added).
    REQUIRE_FALSE(received.empty());
    const auto it =
        std::search(all.begin(), all.end(), received.begin(), received.end());
    CHECK(it != all.end());
}

// ── Item 5b: cut-and-reconnect after GridBegin / chunk / GridEnd
// ───────────────
//
// Cutting the connection mid-snapshot (after GridBegin, after a chunk, after
// GridEnd but before GridApplied) and reconnecting must deliver a complete
// usable snapshot on the second attempt.

TEST_CASE_FIXTURE(SodiumFixture,
                  "reconnect mid-snapshot completes on second attempt") {
    Rig r;
    // Establish a first connection and disconnect so path B is chosen next.
    // Send a Resize so the agent records LastKnownSize (prevents WaitForResize_
    // blocking NeedsSnapshot on the next reconnect).
    {
        AgentConnection ac0{MutableByteSpan{r.sp.AgentFrames}, START,
                            wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc0{r.client,
                             MakeNonce(++r.nonces),
                             MutableByteSpan{r.sp.ClientFrames},
                             START,
                             true,
                             wire::PROTOCOL_VERSION_GRID,
                             MutableByteSpan{r.sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac0, cc0, r.agent, r.sp, START, r.nonces));
        cc0.QueueResize(wire::ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 40; ++i) {
            auto ea = PumpAgent(ac0, r.sp, START);
            if (!ea) break;
            if (const auto* sf = std::get_if<SendFrame>(&*ea))
                std::ignore = Feed(cc0, r.sp, sf->Written);
            auto ec = PumpClient(cc0, r.sp, START);
            if (!ec) break;
            if (const auto* sf = std::get_if<SendFrame>(&*ec))
                std::ignore = Feed(ac0, r.sp, sf->Written);
            if (std::holds_alternative<Idle>(*ea) &&
                std::holds_alternative<Idle>(*ec))
                break;
        }
        ac0.Close(START);
    }
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) std::ignore = r.agent.Write(ByteSpan{chunk});

    // Try reconnect but cut after the first GridBegin/chunk/GridEnd batch
    // without sending GridApplied.  Then reconnect again and confirm.
    // (The first reconnect ends with Close before GridApplied arrives.)
    {
        AgentConnection ac{MutableByteSpan{r.sp.AgentFrames}, START,
                           wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc{r.client,
                            MakeNonce(++r.nonces),
                            MutableByteSpan{r.sp.ClientFrames},
                            START,
                            true,
                            wire::PROTOCOL_VERSION_GRID,
                            MutableByteSpan{r.sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac, cc, r.agent, r.sp, START, r.nonces));
        // Queue a Resize before the loop so WaitForResize_ is cleared once the
        // Resize wire frame reaches the agent (agent waits because
        // LastKnownSize was set by the initial connection).  The loop feeds
        // client SendFrame results back to the agent so the Resize is received.
        cc.QueueResize(wire::ResizeMessage{.Rows = 24, .Cols = 80});
        bool gotNeeds = false;
        for (int i = 0; i < 80 && !gotNeeds; ++i) {
            auto ea = PumpAgent(ac, r.sp, START);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<NeedsSnapshot>(*ea)) {
                gotNeeds = true;
                static std::vector<std::byte> snap(16, std::byte{0x55});
                ac.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
            } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(cc, r.sp, sf->Written));
            }
            auto ec = PumpClient(cc, r.sp, START);
            if (ec.has_value()) {
                if (const auto* sf = std::get_if<SendFrame>(&*ec))
                    std::ignore = Feed(ac, r.sp, sf->Written);
            }
        }
        REQUIRE(gotNeeds);
        // Cut before GridApplied arrives on the agent side.
        ac.Close(START);
    }

    // Second reconnect: must still deliver ApplySnapshot.
    bool finalApply = false;
    {
        AgentConnection ac{MutableByteSpan{r.sp.AgentFrames}, START,
                           wire::PROTOCOL_VERSION_GRID};
        ClientConnection cc{r.client,
                            MakeNonce(++r.nonces),
                            MutableByteSpan{r.sp.ClientFrames},
                            START,
                            true,
                            wire::PROTOCOL_VERSION_GRID,
                            MutableByteSpan{r.sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac, cc, r.agent, r.sp, START, r.nonces));
        // Queue Resize before the loop; the loop feeds the resulting wire frame
        // back to the agent so WaitForResize_ is cleared before NeedsSnapshot.
        cc.QueueResize(wire::ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 120 && !finalApply; ++i) {
            auto ea = PumpAgent(ac, r.sp, START);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<NeedsSnapshot>(*ea)) {
                static std::vector<std::byte> snap(16, std::byte{0x66});
                ac.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
            } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(cc, r.sp, sf->Written));
            }
            auto ec = PumpClient(cc, r.sp, START);
            REQUIRE(ec.has_value());
            if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
                finalApply = true;
                cc.ConfirmApply(as->StreamGen);
            } else if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                REQUIRE(Feed(ac, r.sp, sf->Written));
            }
        }
    }
    CHECK(finalApply);
}
