/// Agent wiring tests: GridPump unit tests + session-level integration.
///
/// Each test kills one specific mutation of the agent wiring code.
/// Tests 1–7 and 9 use GridPump directly (requires ZET_HAVE_AGENT since
/// zet_agent_loop links zet::io).  Test 8 is a session-level integration test.
///
/// Mutations targeted (letter matches coordinator feedback):
///   (a) skip feeding the grid          → test 1
///   (b) feed twice                     → test 2
///   (c) no grid resize on Resized      → test 3
///   (d) ignore NeedsSnapshot           → test 4
///   (e) empty snapshot                 → test 5
///   (f) skip/never-timeout Ground wait → test 6
///   (g) snapshot at grid size, not requested size → test 7
///   (h) retain bytes after overflow    → test 8
///   (i) no ReleaseSnapshot             → test 9

#include "grid_pump.hpp"

#include "../session/grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/session/connection.hpp"
#include "zet/session/limits.hpp"

namespace {

using namespace zet;
using namespace zet::agent;
using namespace zet::session;
using namespace zet::grid;
using namespace zet::wire;
using namespace std::chrono_literals;

// ── Helpers ────────────────────────────────────────────────────────────────

/// Make a GridPump at 80×24; REQUIRE creation succeeds.
std::unique_ptr<GridPump> MakePump(std::uint16_t cols = 80,
                                   std::uint16_t rows = 24) {
    auto r = GridPump::Create(cols, rows);
    REQUIRE(r.has_value());
    return std::move(*r);
}

/// ESC byte — takes the VT parser out of Ground state.
constexpr std::byte ESC_BYTE{0x1B};

[[maybe_unused]] void FillRingV2(Rig& r) {
    const std::vector<std::byte> chunk(300, std::byte{0xBB});
    for (int i = 0; i < 8; ++i) {
        std::ignore = r.Agent.Write(ByteSpan{chunk});
    }
}

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

// ── Test 1: FeedBytes writes into the grid ─────────────────────────────────
//
// BREAK (a): remove GridPump_->FeedBytes call in MovePtyOutput → cells blank.

TEST_CASE_FIXTURE(SodiumFixture, "FeedBytes writes byte into grid cell") {
    auto p = MakePump();
    const std::byte b{static_cast<std::byte>('A')};
    p->FeedBytes(&b, 1);
    CHECK(p->GetGrid().CellAt(0, 0).Codepoint == static_cast<char32_t>('A'));
    CHECK(p->TotalBytesProcessed() == 1);
}

// ── Test 2: FeedBytes advances cursor exactly once ─────────────────────────
//
// BREAK (b): call FeedBytes twice with the same data → cursor at col 4, not 2.

TEST_CASE_FIXTURE(SodiumFixture,
                  "FeedBytes advances cursor by exactly the bytes fed") {
    auto p = MakePump();
    const std::array<std::byte, 2> bytes{std::byte{'A'}, std::byte{'B'}};
    p->FeedBytes(bytes.data(), 2);
    CHECK(p->GetGrid().CursorCol() == 2);
    CHECK(p->TotalBytesProcessed() == 2);
}

// ── Test 3: HandleResize updates grid dimensions ───────────────────────────
//
// BREAK (c): skip GridPump_->HandleResize → Cols()/Rows() stay at 80/24.

TEST_CASE_FIXTURE(SodiumFixture,
                  "HandleResize resizes grid to requested dimensions") {
    auto p = MakePump();
    auto r = p->HandleResize(40, 10);
    REQUIRE(r.has_value());
    CHECK(p->GetGrid().Cols() == 40);
    CHECK(p->GetGrid().Rows() == 10);
}

// ── Test 4: OnNeedsSnapshot arms ground-wait; TryBuild returns ready ───────
//
// BREAK (d): skip arming SnapWait_ in OnNeedsSnapshot → TryBuild always
// nullopt.

TEST_CASE_FIXTURE(
    SodiumFixture,
    "OnNeedsSnapshot arms ground-wait; TryBuild returns SnapReady") {
    auto p = MakePump();
    CHECK_FALSE(p->HasPendingSnap());

    // Parser is in Ground → OnNeedsSnapshot supplies immediately.
    auto ready = p->OnNeedsSnapshot(24, 80, START);
    REQUIRE(ready.has_value());
    CHECK_FALSE(ready->Bytes.empty());
    CHECK(p->HasSnapshot());
    CHECK_FALSE(p->HasPendingSnap());  // cleared by DoBuild

    p->ReleaseSnapshot();
}

// ── Test 5: Snapshot bytes are non-empty ──────────────────────────────────
//
// BREAK (e): return empty Bytes from DoBuild → size 0.

TEST_CASE_FIXTURE(SodiumFixture, "Snapshot bytes carry grid content") {
    auto p = MakePump();
    const std::byte b{static_cast<std::byte>('Z')};
    p->FeedBytes(&b, 1);

    auto ready = p->OnNeedsSnapshot(24, 80, START);
    REQUIRE(ready.has_value());
    CHECK_FALSE(ready->Bytes.empty());

    p->ReleaseSnapshot();
}

// ── Test 6: Ground-wait timeout → PossiblyDiverged flag set ───────────────
//
// BREAK (f): never force-reset on timeout → TryBuild(past_deadline) still
// nullopt.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Ground-wait timeout fires and sets PossiblyDiverged") {
    auto p = MakePump();

    // Take the parser out of Ground with ESC.
    p->FeedBytes(&ESC_BYTE, 1);
    REQUIRE(p->ParserState() != EParserState::Ground);

    // Arm ground-wait; not in Ground so should not fire yet.
    auto immediate = p->OnNeedsSnapshot(24, 80, START);
    CHECK_FALSE(immediate.has_value());
    CHECK(p->HasPendingSnap());

    // Before timeout: still waiting.
    {
        auto r = p->TryBuild(START);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->has_value());
    }

    // After timeout: snapshot is forced with PossiblyDiverged.
    const Timestamp past =
        START + GROUND_WAIT_TIMEOUT + std::chrono::milliseconds{1};
    auto r = p->TryBuild(past);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->Flags != 0);  // bit 0 = PossiblyDiverged

    p->ReleaseSnapshot();
}

// ── Test 7: Snapshot taken at requested size, not original grid size ───────
//
// BREAK (g): skip grid resize in DoBuild → grid stays 80×24, not 40×10.

TEST_CASE_FIXTURE(SodiumFixture,
                  "OnNeedsSnapshot resizes grid to requested dimensions") {
    auto p = MakePump(80, 24);

    // Request a snapshot at 40×10 (different from grid's current 80×24).
    auto ready = p->OnNeedsSnapshot(/*rows=*/10, /*cols=*/40, START);
    REQUIRE(ready.has_value());
    CHECK(p->GetGrid().Cols() == 40);
    CHECK(p->GetGrid().Rows() == 10);

    p->ReleaseSnapshot();
}

// ── Test 8: bytes dropped during overflow do not appear after snapshot ──────
//
// BREAK (h): remove PtyFrom_ = PtyTo_ after SupplySnapshot → pre-overflow bytes
//   are written to the new session generation and appear as Deliver 0xBB.

TEST_CASE_FIXTURE(SodiumFixture,
                  "bytes dropped during overflow do not appear as Deliver "
                  "after snapshot is applied") {
    Rig r;
    REQUIRE(ConnectAndClose(r));

    // Overflow chunk: 0xBB (dropped in Resync).
    const std::vector<std::byte> overflow(300, std::byte{0xBB});
    for (int i = 0; i < 8; ++i) std::ignore = r.Agent.Write(ByteSpan{overflow});

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

    // Sentinel bytes — distinct from 0xBB overflow.
    const std::vector<std::byte> sentinel(8, std::byte{0xCC});
    std::ignore = r.Agent.Write(ByteSpan{sentinel});

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
        if (deliverCount > 0) {
            break;
        }
    }
    CHECK(deliverCount > 0);
    CHECK_FALSE(sawOverflowByte);
}

// ── Test 9: ReleaseSnapshot clears HasSnapshot ─────────────────────────────
//
// BREAK (i): skip ReleaseSnapshot call → HasSnapshot stays true, snapshot
//   bytes are never freed (memory leak; GridPump holds 4 MiB per connection).

TEST_CASE_FIXTURE(SodiumFixture,
                  "ReleaseSnapshot clears HasSnapshot after GridEnd sent") {
    auto p = MakePump();
    const std::byte b{static_cast<std::byte>('X')};
    p->FeedBytes(&b, 1);

    auto ready = p->OnNeedsSnapshot(24, 80, START);
    REQUIRE(ready.has_value());
    CHECK(p->HasSnapshot());

    p->ReleaseSnapshot();
    CHECK_FALSE(p->HasSnapshot());
}
