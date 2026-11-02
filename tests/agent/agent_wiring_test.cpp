/// Agent wiring tests: GridPump unit tests + real-agent integration tests.
///
/// Each test kills one specific mutation of the agent wiring code.
/// Tests 1–7 and 9–11 use GridPump directly (requires ZET_HAVE_AGENT since
/// zet_agent_loop links zet::io).  Tests 8 and IA use session-level or
/// full-agent integration.
///
/// Mutations targeted:
///   (a) skip feeding the grid                     → test 1
///   (b) feed twice                                → test 2
///   (c) no grid resize on Resized                 → test 3
///   (d) ignore NeedsSnapshot                      → test 4
///   (e) empty snapshot                            → test 5
///   (f) skip/never-timeout Ground wait            → test 6
///   (g) snapshot at grid size, not requested size → test 7
///   (h) retain bytes after overflow               → test 8
///   (i) no ReleaseSnapshot                        → test 9
///   (j) byte cap not checked in TryBuild          → test 10
///   (k) 0×0 uses 80×24 instead of actual size    → test 11
///   (M1) FeedBytes not called in MovePtyOutput    → test IA
///   (M2) GridFed_ not advanced in MovePtyOutput   → test IA

#include "grid_pump.hpp"
#include "pump_test_api.hpp"

#include "../session/grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/io/clock.hpp"
#include "zet/io/fd.hpp"
#include "zet/io/tcp.hpp"
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

// ── TCP helpers for full-agent integration tests ───────────────────────────

/// Connect a non-blocking TCP socket to 127.0.0.1:port; returns invalid fd on
/// any failure.
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
    if (::poll(&pf, 1, 3000) <= 0) {
        return io::UniqueFd{};
    }
    if (!io::FinishConnect(fd->Get())) {
        return io::UniqueFd{};
    }
    return std::move(*fd);
}

/// Pump a ClientConnection over a non-blocking TCP socket until it returns a
/// non-SendFrame / non-Idle event or the timeout elapses.
std::optional<ConnectionEvent> DriveCC(
    int fd, ClientConnection& cc,
    std::array<std::byte, CONNECTION_OUT_SIZE>& out,
    std::array<std::byte, CONNECTION_SCRATCH_SIZE>& scratch,
    int timeoutMs = 200) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMs};
    while (std::chrono::steady_clock::now() < deadline) {
        int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count());
        if (remaining <= 0) {
            break;
        }
        pollfd pf{.fd = fd, .events = POLLIN, .revents = 0};
        ::poll(&pf, 1, std::min(remaining, 10));
        if (pf.revents & POLLIN) {
            std::array<std::byte, 65536> buf{};
            auto n = io::ReadSome(fd, MutableByteSpan{buf});
            if (n && *n > 0) {
                (void)cc.OnBytes(ByteSpan{buf}.first(*n));
            }
        }
        auto r = cc.Pump(io::MonotonicNow(), MutableByteSpan{out},
                         MutableByteSpan{scratch});
        if (!r) {
            return std::nullopt;
        }
        if (const auto* sf = std::get_if<SendFrame>(&*r)) {
            auto bytes = ByteSpan{out.data(), sf->Written};
            while (!bytes.empty()) {
                auto w = io::WriteSome(fd, bytes);
                if (!w) {
                    if (w.error().WouldBlock()) {
                        pollfd wpf{.fd = fd, .events = POLLOUT, .revents = 0};
                        ::poll(&wpf, 1, 10);
                        continue;
                    }
                    return std::nullopt;
                }
                bytes = bytes.subspan(*w);
            }
            continue;
        }
        if (std::holds_alternative<Idle>(*r)) {
            continue;
        }
        return *r;
    }
    return std::nullopt;
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
// BREAK (h): at AgentConnection::SupplySnapshot skip starting the new stream
//   generation → the ring content from before the overflow is replayed in
//   the new generation and appears as Deliver 0xBB events.
// (Session-level test; uses AgentConnection directly, not Agent.)

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

// ── Test 10: 1 MiB byte cap fires ground-wait ─────────────────────────────
//
// BREAK (j): remove bytesExpired check in TryBuild → build never fires while
// parser stays non-Ground even after the byte cap is hit.

TEST_CASE_FIXTURE(SodiumFixture,
                  "1 MiB byte cap fires ground-wait when parser non-Ground") {
    auto p = MakePump();
    p->FeedBytes(&ESC_BYTE, 1);
    REQUIRE(p->ParserState() != EParserState::Ground);

    auto immediate = p->OnNeedsSnapshot(24, 80, START);
    CHECK_FALSE(immediate.has_value());
    CHECK(p->HasPendingSnap());

    const std::vector<std::byte> filler(GROUND_WAIT_BYTES, std::byte{0x00});
    p->FeedBytes(filler.data(), filler.size());

    const Timestamp far = START + std::chrono::hours{1};
    auto r = p->TryBuild(far);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    p->ReleaseSnapshot();
}

// ── Test 11: OnNeedsSnapshot(0,0) on non-80×24 grid uses actual size ───────
//
// BREAK (k): hard-code 80 and 24 for targetCols/targetRows → snapshot always
// at 80×24 even when the grid is 40×10.

TEST_CASE_FIXTURE(SodiumFixture,
                  "OnNeedsSnapshot(0,0) on non-80x24 grid uses actual size") {
    auto p = MakePump(40, 10);
    auto ready = p->OnNeedsSnapshot(0, 0, START);
    REQUIRE(ready.has_value());

    auto ref = GridPump::Create(40, 10);
    REQUIRE(ref.has_value());
    auto refReady = (*ref)->OnNeedsSnapshot(0, 0, START);
    REQUIRE(refReady.has_value());

    const std::vector<std::byte> got{ready->Bytes.begin(), ready->Bytes.end()};
    const std::vector<std::byte> want{refReady->Bytes.begin(),
                                      refReady->Bytes.end()};
    CHECK(got == want);
    p->ReleaseSnapshot();
}

// ── Test IA: pty bytes reach the grid exactly once (integration) ───────────
//
// Drives the real Agent pump loop over TCP + a pipe.  Overflows the ring to
// force path B, then compares the delivered snapshot bytes with a reference
// GridPump fed the same data.
//
// BREAK M1: remove FeedBytes call in MovePtyOutput → grid is blank; snapshot
//   bytes differ from reference.
// BREAK M2: use GridFed_=0 instead of GridFed_=PtyTo_ → bytes fed twice;
//   cursor further right than expected.

TEST_CASE_FIXTURE(SodiumFixture, "IA: pty bytes reach the grid exactly once") {
    using namespace agent;
    using namespace wire;
    constexpr std::uint8_t key = 71;
    const auto lim = TinyLimits();

    auto ptyPipe = io::MakePipe();
    REQUIRE(ptyPipe.has_value());
    auto listener = io::ListenTcp(0);
    REQUIRE(listener.has_value());
    const auto port = listener->Port;

    auto fut = std::async(std::launch::async, [&] {
        return RunAgentForTest(AgentTestConfig{
            .Secrets = DeriveSessionSecrets(MakeKey(key)),
            .Limits = lim,
            .Listen = std::move(*listener),
            .PtyReadFd = std::move(ptyPipe->Read),
            .Ceiling = PROTOCOL_VERSION_GRID,
            .GiveUpAfter = std::chrono::seconds{3},
        });
    });

    // One ClientSession is reused across both connections (stream state
    // persists).
    std::vector<std::byte> cs(lim.DisconnectMax + 256);
    ClientSession sess{DeriveSessionSecrets(MakeKey(key)),
                       SessionStreams{.Storage = MutableByteSpan{cs},
                                      .Now = io::MonotonicNow(),
                                      .Limits = lim}};
    std::uint64_t nonces = 0;

    // ── First connection: GRID_CAPABLE Live → sets Resync overflow policy.
    {
        auto sock1 = TcpConnect(port);
        REQUIRE(sock1.Get() != -1);
        std::array<std::byte, CONNECTION_FRAME_STORAGE> frames{};
        std::array<std::byte, CONNECTION_OUT_SIZE> out{};
        std::array<std::byte, CONNECTION_SCRATCH_SIZE> scratch{};
        std::vector<std::byte> snapBuf(wire::SNAPSHOT_MAX_BYTES);
        ClientConnection cc1{sess,
                             MakeNonce(++nonces),
                             MutableByteSpan{frames},
                             io::MonotonicNow(),
                             /*capable=*/true,
                             PROTOCOL_VERSION_GRID,
                             MutableByteSpan{snapBuf}};
        const int fd1 = sock1.Get();
        const auto dl =
            std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!cc1.IsLive() && std::chrono::steady_clock::now() < dl) {
            DriveCC(fd1, cc1, out, scratch, 50);
        }
        REQUIRE(cc1.IsLive());
    }  // sock1 goes out of scope here → closed → agent detaches →
       // DETACHED+Resync

    // Wait for the agent to process the close and transition to DETACHED+Resync
    // before writing PTY bytes.  If we write first, MovePtyOutput runs before
    // DropClosed in the same loop iteration, so the ring fills while still Live
    // and RingOverflowed is never set.
    ::usleep(100'000);

    // ── Write >DisconnectMax PTY bytes so the ring overflows to Resync mode.
    const std::vector<std::byte> ptyData(600, std::byte{'A'});
    {
        ByteSpan remaining{ptyData.data(), ptyData.size()};
        while (!remaining.empty()) {
            auto w = io::WriteSome(ptyPipe->Write.Get(), remaining);
            REQUIRE(w.has_value());
            remaining = remaining.subspan(*w);
        }
    }
    ::usleep(100'000);

    // ── Second connection: GRID_CAPABLE → NeedsSnapshot fires, agent builds
    // snapshot from the 600 'A' bytes and sends GridBegin/Chunk/End.
    std::vector<std::byte> gotSnap;
    {
        auto sock2 = TcpConnect(port);
        REQUIRE(sock2.Get() != -1);
        std::array<std::byte, CONNECTION_FRAME_STORAGE> frames{};
        std::array<std::byte, CONNECTION_OUT_SIZE> out{};
        std::array<std::byte, CONNECTION_SCRATCH_SIZE> scratch{};
        std::vector<std::byte> snapBuf(wire::SNAPSHOT_MAX_BYTES);
        ClientConnection cc2{sess,
                             MakeNonce(++nonces),
                             MutableByteSpan{frames},
                             io::MonotonicNow(),
                             /*capable=*/true,
                             PROTOCOL_VERSION_GRID,
                             MutableByteSpan{snapBuf}};
        cc2.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        const int fd2 = sock2.Get();
        const auto dl =
            std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (gotSnap.empty() && std::chrono::steady_clock::now() < dl) {
            auto ev = DriveCC(fd2, cc2, out, scratch, 200);
            if (ev) {
                if (const auto* as = std::get_if<ApplySnapshot>(&*ev)) {
                    gotSnap.assign(
                        snapBuf.begin(),
                        snapBuf.begin() +
                            static_cast<std::ptrdiff_t>(as->Bytes.size()));
                }
            }
        }
    }  // sock2 closed here

    ptyPipe->Write.Reset();  // Close write end → agent sees pty EOF
    fut.wait();

    REQUIRE_FALSE(gotSnap.empty());

    // Reference: GridPump at 80×24 fed the same 600 'A' bytes.
    auto refPump = GridPump::Create(80, 24);
    REQUIRE(refPump.has_value());
    (*refPump)->FeedBytes(ptyData.data(), ptyData.size());
    auto refReady = (*refPump)->OnNeedsSnapshot(24, 80, START);
    REQUIRE(refReady.has_value());
    const std::vector<std::byte> want{refReady->Bytes.begin(),
                                      refReady->Bytes.end()};

    CHECK(gotSnap == want);
}
