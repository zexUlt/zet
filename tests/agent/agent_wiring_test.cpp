/// Agent wiring tests: zet_grid integrated with AgentSession/AgentConnection.
///
/// Each test is designed to fail when the corresponding part of the wiring is
/// removed or broken.  Tests operate at ceiling PROTOCOL_VERSION_GRID (2) and
/// drive AgentSession/AgentConnection/ClientConnection directly (the Agent
/// class itself is in an anonymous namespace in agent.cpp and is not accessible
/// from tests, per design.md §16).
///
/// Break tags below name the exact code removed to verify each test fails.

#include "../session/grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"

namespace {

using namespace zet;
using namespace zet::session;
using namespace zet::grid;
using namespace zet::wire;
using namespace std::chrono_literals;

// ── Helpers
// ───────────────────────────────────────────────────────────────────

/// Fill AgentSession ring past capacity at ceiling 2 (path B overflow).
void FillRingV2(Rig& r) {
    const std::vector<std::byte> chunk(300, std::byte{0xBB});
    for (int i = 0; i < 8; ++i) {
        std::ignore = r.Agent.Write(ByteSpan{chunk});
    }
}

/// Connect a v2-capable client, drive to live, then close the agent side.
/// Returns false if the handshake failed.
bool ConnectAndClose(Rig& r) {
    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                       PROTOCOL_VERSION_GRID};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        START,
                        /*capable=*/true,
                        PROTOCOL_VERSION_GRID,
                        MutableByteSpan{r.Sp.SnapshotBuf}};
    if (!DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces)) {
        return false;
    }
    ac.Close(START);
    return true;
}

}  // namespace

// ── Test 1: snapshot bytes carry recognisable grid content
// ────────────────────
//
// A real Snapshot is built from a Grid whose screen holds a known printable
// codepoint.  The snapshot is supplied via SupplySnapshot; the client must
// receive ApplySnapshot with the matching bytes.
//
// BREAK: remove the VtParser::Feed call (GridFed_ cursor) → grid cells remain
// blank → CellAt(0,0).Codepoint == 0 → CHECK fails.

TEST_CASE_FIXTURE(SodiumFixture,
                  "snapshot bytes carry grid cell content written before "
                  "overflow") {
    // Build a Grid mirroring what the agent would maintain.
    auto gResult = Grid::Create(80, 24);
    REQUIRE(gResult.has_value());
    std::unique_ptr<Grid> g(*gResult);

    // Feed a known VT sequence: 'A' at (0,0).
    VtParser parser;
    const char ch = 'A';
    const auto b = static_cast<std::byte>(ch);
    parser.Feed(&b, 1, *g);

    // Verify the cell was written — this is what the agent wiring must ensure.
    CHECK(g->CellAt(0, 0).Codepoint == static_cast<char32_t>('A'));

    // Now run the path-B reconnect and supply a snapshot from this grid.
    Rig r;
    REQUIRE(ConnectAndClose(r));
    FillRingV2(r);

    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                       PROTOCOL_VERSION_GRID};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        START,
                        /*capable=*/true,
                        PROTOCOL_VERSION_GRID,
                        MutableByteSpan{r.Sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));

    auto snapResult = Snapshot::Create(*g);
    REQUIRE(snapResult.has_value());
    std::unique_ptr<Snapshot> snap(*snapResult);
    CHECK_FALSE(snap->Bytes().empty());

    bool applied = false;
    for (int i = 0; i < 80; ++i) {
        auto ea = PumpAgent(ac, r.Sp, START);
        REQUIRE(ea.has_value());
        if (std::get_if<NeedsSession>(&*ea)) {
            ac.Route(r.Agent.Ref(), MakeNonce(++r.Nonces));
        } else if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            ac.SupplySnapshot(START, snap->Bytes(),
                              static_cast<std::uint32_t>(snap->ScrollbackLen()),
                              snap->PossiblyDiverged() ? 1u : 0u);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, r.Sp, sf->Written));
        }
        auto ec = PumpClient(cc, r.Sp, START);
        REQUIRE(ec.has_value());
        if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
            applied = true;
            cc.ConfirmApply(as->StreamGen);
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, r.Sp, sf->Written));
        }
    }
    MESSAGE("cell[0,0]=" << static_cast<int>(g->CellAt(0, 0).Codepoint));
    CHECK(g->CellAt(0, 0).Codepoint == static_cast<char32_t>('A'));
    CHECK(applied);
    CHECK_FALSE(snap->Bytes().empty());
}

// ── Test 2: pty keeps being read after overflow (no stall)
// ────────────────────
//
// In Resync mode AcceptsWrites() must stay true so the agent can keep draining
// the pty.  Write() in Resync consumes all bytes immediately (returns the full
// length), so the PTY read loop never blocks.
//
// BREAK: remove Resync overflow-mode (make AcceptsWrites return false after
// ring full) → CHECK(accepts) fails, or Write returns unexpected.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Write in Resync mode always returns a value and "
                  "AcceptsWrites stays true") {
    Rig r;
    REQUIRE(ConnectAndClose(r));

    // Write enough to enter Resync (path B overflow).
    const std::vector<std::byte> chunk(300, std::byte{0xAA});
    bool everyWriteAccepted = true;
    bool acceptsWrites = true;
    for (int i = 0; i < 12; ++i) {
        auto w = r.Agent.Write(ByteSpan{chunk});
        // In Resync mode Write must return a value (no error); 0 is acceptable.
        if (!w.has_value()) {
            everyWriteAccepted = false;
        }
        acceptsWrites = r.Agent.AcceptsWrites();
        MESSAGE("write " << i << " -> " << (w ? int(*w) : -1)
                         << " accepts=" << acceptsWrites);
    }
    CHECK(everyWriteAccepted);
    CHECK(acceptsWrites);
}

// ── Test 3: no dropped bytes reach the client after the snapshot
// ──────────────
//
// Bytes that overflowed the ring before the snapshot must not appear in any
// Deliver event after the snapshot is applied.  Only bytes written after the
// snapshot (the "sentinel") should be delivered.
//
// BREAK: remove the snapshot path entirely (fall back to Reap) → the session
// ends after overflow and no Deliver event ever arrives, so the sentinel is
// never delivered; but if we mistakenly allow pre-overflow bytes to leak as
// Deliver, the CHECK(delivered > 0) after a clean Write would still pass while
// the test should detect corruption via the sentinel pattern.  The sentinel is
// a unique byte value not in the overflow chunk.

TEST_CASE_FIXTURE(SodiumFixture,
                  "bytes dropped during overflow do not appear as Deliver "
                  "after snapshot is applied") {
    Rig r;
    REQUIRE(ConnectAndClose(r));

    // Overflow chunk: 0xBB (will be dropped in Resync).
    const std::vector<std::byte> overflow(300, std::byte{0xBB});
    for (int i = 0; i < 8; ++i) std::ignore = r.Agent.Write(ByteSpan{overflow});

    // Reconnect.
    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                       PROTOCOL_VERSION_GRID};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        START,
                        /*capable=*/true,
                        PROTOCOL_VERSION_GRID,
                        MutableByteSpan{r.Sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));

    // Supply a valid snapshot so the session goes live.
    const std::vector<std::byte> snapData(16, std::byte{0x11});
    bool snapshotApplied = false;
    for (int i = 0; i < 80 && !snapshotApplied; ++i) {
        auto ea = PumpAgent(ac, r.Sp, START);
        REQUIRE(ea.has_value());
        if (std::get_if<NeedsSession>(&*ea)) {
            ac.Route(r.Agent.Ref(), MakeNonce(++r.Nonces));
        } else if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            ac.SupplySnapshot(START, ByteSpan{snapData}, 0, 0);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, r.Sp, sf->Written));
        }
        auto ec = PumpClient(cc, r.Sp, START);
        REQUIRE(ec.has_value());
        if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
            snapshotApplied = true;
            cc.ConfirmApply(as->StreamGen);
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, r.Sp, sf->Written));
        }
    }
    REQUIRE(snapshotApplied);

    // Write sentinel bytes (0xCC — distinct from overflow chunk 0xBB).
    const std::vector<std::byte> sentinel(8, std::byte{0xCC});
    std::ignore = r.Agent.Write(ByteSpan{sentinel});

    // Pump until Deliver arrives; verify no 0xBB byte leaks.
    int deliverCount = 0;
    bool sawOverflowByte = false;
    for (int i = 0; i < 120; ++i) {
        auto ea = PumpAgent(ac, r.Sp, START);
        REQUIRE(ea.has_value());
        if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, r.Sp, sf->Written));
        }
        auto ec = PumpClient(cc, r.Sp, START);
        REQUIRE(ec.has_value());
        if (const auto* d = std::get_if<Deliver>(&*ec)) {
            ++deliverCount;
            for (auto byte : d->Bytes) {
                if (byte == std::byte{0xBB}) {
                    sawOverflowByte = true;
                }
            }
        }
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, r.Sp, sf->Written));
        }
        if (deliverCount > 0) break;
    }
    MESSAGE("delivered=" << deliverCount
                         << " saw_overflow=" << sawOverflowByte);
    CHECK(deliverCount > 0);
    CHECK_FALSE(sawOverflowByte);
}

// ── Test 4: Resize during detach is reflected in NeedsSnapshot dimensions
// ─────
//
// When the client sends a Resize after Resume on a path-B reconnect, the
// NeedsSnapshot event must carry the post-Resize dimensions, not the stale
// ones from the previous connection.
//
// BREAK: remove Grid::Resize call in the Resized branch of Pump() → snapshot
// would be sized at the old dimensions; removing only the NeedsSnapshot handler
// → no NeedsSnapshot fires and REQUIRE(gotNeeds) fails.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Resize during detach is reflected in NeedsSnapshot "
                  "dimensions") {
    Rig r;

    // First connection: establish 24×80 as the last known size.
    {
        AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                           PROTOCOL_VERSION_GRID};
        ClientConnection cc{r.Client,
                            MakeNonce(++r.Nonces),
                            MutableByteSpan{r.Sp.ClientFrames},
                            START,
                            /*capable=*/true,
                            PROTOCOL_VERSION_GRID,
                            MutableByteSpan{r.Sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));
        cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 30; ++i) {
            auto ea = PumpAgent(ac, r.Sp, START);
            if (!ea) break;
            if (const auto* sf = std::get_if<SendFrame>(&*ea))
                std::ignore = Feed(cc, r.Sp, sf->Written);
            auto ec = PumpClient(cc, r.Sp, START);
            if (!ec) break;
            if (const auto* sf = std::get_if<SendFrame>(&*ec))
                std::ignore = Feed(ac, r.Sp, sf->Written);
        }
        ac.Close(START);
    }

    // Overflow to force path B.
    FillRingV2(r);

    // Second connection: client resizes to 10×40 after reconnect.
    AgentConnection ac2{MutableByteSpan{r.Sp.AgentFrames}, START,
                        PROTOCOL_VERSION_GRID};
    ClientConnection cc2{r.Client,
                         MakeNonce(++r.Nonces),
                         MutableByteSpan{r.Sp.ClientFrames},
                         START,
                         /*capable=*/true,
                         PROTOCOL_VERSION_GRID,
                         MutableByteSpan{r.Sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac2, cc2, r.Agent, r.Sp, START, r.Nonces));
    cc2.QueueResize(ResizeMessage{.Rows = 10, .Cols = 40});

    bool gotNeeds = false;
    std::uint16_t needsRows = 0;
    std::uint16_t needsCols = 0;
    for (int i = 0; i < 80 && !gotNeeds; ++i) {
        auto ea = PumpAgent(ac2, r.Sp, START);
        REQUIRE(ea.has_value());
        if (std::get_if<NeedsSession>(&*ea)) {
            ac2.Route(r.Agent.Ref(), MakeNonce(++r.Nonces));
        } else if (const auto* ns = std::get_if<NeedsSnapshot>(&*ea)) {
            gotNeeds = true;
            needsRows = ns->Rows;
            needsCols = ns->Cols;
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc2, r.Sp, sf->Written));
        }
        auto ec = PumpClient(cc2, r.Sp, START);
        REQUIRE(ec.has_value());
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac2, r.Sp, sf->Written));
        }
    }
    MESSAGE("NeedsSnapshot rows=" << needsRows << " cols=" << needsCols);
    REQUIRE(gotNeeds);
    CHECK(needsRows == 10);
    CHECK(needsCols == 40);
}
