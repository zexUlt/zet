/// Agent wiring tests: GridPump unit tests plus tests that run the real agent
/// pump loop (src/agent/agent.cpp) through pump_test_api.hpp.
///
/// The agent tests (prefix "Agent:") start RunAgentForTest on a TCP listener
/// with a real pty in place of the shell, connect real ClientConnections, and
/// look at what comes out: snapshots on the wire and
/// the grid the agent built (AgentTestConfig::Inspect).  Each one is aimed at
/// one line of agent.cpp's glue; the mutation it kills is named in its header
/// and was checked by applying the mutant by hand.
///
/// Agent glue (src/agent/agent.cpp) and its killing tests:
///   MovePtyOutput does not call FeedBytes     "Agent: pty bytes reach the grid
///                                              exactly once"
///   GridFed_ does not advance                 "Agent: bytes the session cannot
///                                              take are fed to the grid once"
///   PtyFrom_ = PtyTo_ missing, TrySupply...   "Agent: bytes fed while waiting
///                                              for Ground are not replayed"
///   PtyFrom_ = PtyTo_ missing, inline supply  "Agent: bytes fed while waiting
///                                              for the Resize are not
///                                              replayed"
///   GridEmissionDone never true / Release...  "Agent: a second snapshot cycle
///                                              succeeds and frees the first"
///   Resized not forwarded to GridPump         "Agent: Resized is forwarded to
///                                              the grid"
///   no clear of PtyInput_ once the child has  "Agent: input for a shell that
///   gone (RunLoop)                             has gone is dropped and
///                                              acknowledged"
///
/// GridPump units (tests 1-7 and 9-11) call GridPump
/// directly; they cover GridPump's own logic, not the agent glue.  Test 8 is a
/// session-level regression test of AgentConnection::SupplySnapshot, not of
/// agent.cpp.
///
/// PtyFrom_ = PtyTo_ is NOT redundant: while a snapshot is pending the session
/// is Live with a full ring, so AcceptsWrites() is false and pty bytes pile up
/// in PtyRead_ after the grid has seen them; the assignment drops them so the
/// new stream generation does not replay what the snapshot already holds.

#include "agent_rig.hpp"

#include <doctest/doctest.h>

// ── Test 1: FeedBytes writes into the grid ─────────────────────────────────
//
// Kills: FeedBytes does not parse (cells stay blank).

TEST_CASE_FIXTURE(SodiumFixture, "FeedBytes writes byte into grid cell") {
    auto p = MakePump();
    const std::byte b{static_cast<std::byte>('A')};
    p->FeedBytes(&b, 1);
    CHECK(p->GetGrid().CellAt(0, 0).Codepoint == static_cast<char32_t>('A'));
    CHECK(p->TotalBytesProcessed() == 1);
}

// ── Test 2: FeedBytes advances cursor exactly once ─────────────────────────
//
// Kills: FeedBytes feeds the parser twice (cursor at col 4, not 2).

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
// Kills: HandleResize does not resize the grid.

TEST_CASE_FIXTURE(SodiumFixture,
                  "HandleResize resizes grid to requested dimensions") {
    auto p = MakePump();
    auto r = p->HandleResize(40, 10);
    REQUIRE(r.has_value());
    CHECK(p->GetGrid().Cols() == 40);
    CHECK(p->GetGrid().Rows() == 10);
}

// ── Test 4: OnNeedsSnapshot builds at once from Ground ─────────────────────
//
// Kills: OnNeedsSnapshot never builds or arms the wait.

TEST_CASE_FIXTURE(SodiumFixture,
                  "OnNeedsSnapshot in Ground builds a snapshot at once") {
    auto p = MakePump();
    CHECK_FALSE(p->HasPendingSnap());

    const auto ready = BuildNow(*p, 24, 80);
    CHECK_FALSE(ready.Bytes.empty());
    CHECK(p->HasSnapshot());
    CHECK_FALSE(p->HasPendingSnap());  // cleared by DoBuild

    p->ReleaseSnapshot();
}

// ── Test 5: Snapshot bytes are non-empty ──────────────────────────────────
//
// Kills: DoBuild returns empty Bytes.

TEST_CASE_FIXTURE(SodiumFixture, "Snapshot bytes carry grid content") {
    auto p = MakePump();
    const std::byte b{static_cast<std::byte>('Z')};
    p->FeedBytes(&b, 1);

    const auto ready = BuildNow(*p, 24, 80);
    CHECK_FALSE(ready.Bytes.empty());

    p->ReleaseSnapshot();
}

// ── Test 6: Ground-wait timeout → PossiblyDiverged flag set ───────────────
//
// Kills: TryBuild never fires on the time limit, or fires without setting
// PossiblyDiverged.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Ground-wait timeout fires and sets PossiblyDiverged") {
    auto p = MakePump();

    // Take the parser out of Ground with ESC.
    p->FeedBytes(&ESC_BYTE, 1);
    REQUIRE(p->ParserState() != EParserState::Ground);

    // Arm ground-wait; not in Ground so nothing is built yet.
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
// Kills: DoBuild skips the grid resize (grid stays 80×24, not 40×10).

TEST_CASE_FIXTURE(SodiumFixture,
                  "OnNeedsSnapshot resizes grid to requested dimensions") {
    auto p = MakePump(80, 24);

    // Request a snapshot at 40×10 (different from grid's current 80×24).
    std::ignore = BuildNow(*p, /*rows=*/10, /*cols=*/40);
    CHECK(p->GetGrid().Cols() == 40);
    CHECK(p->GetGrid().Rows() == 10);

    p->ReleaseSnapshot();
}

// ── Test 8: bytes dropped during overflow do not appear after snapshot ──────
//
// Session-level regression test of AgentConnection::SupplySnapshot (it drives
// AgentConnection directly, not agent.cpp).  Kills: SupplySnapshot does not
// start the new stream generation, so the ring content from before the
// overflow is replayed as Deliver 0xBB events.

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
    // The agent waits for this before asking for a snapshot (design.md §9,
    // path B, step 1), and the clock here is frozen.
    cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
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
// Kills: ReleaseSnapshot does not free (HasSnapshot stays true).

TEST_CASE_FIXTURE(SodiumFixture, "ReleaseSnapshot clears HasSnapshot") {
    auto p = MakePump();
    const std::byte b{static_cast<std::byte>('X')};
    p->FeedBytes(&b, 1);

    std::ignore = BuildNow(*p, 24, 80);
    CHECK(p->HasSnapshot());

    p->ReleaseSnapshot();
    CHECK_FALSE(p->HasSnapshot());
}

// ── Test 10: 1 MiB byte cap fires ground-wait ─────────────────────────────
//
// Kills: TryBuild does not check the byte cap.  The clock stays at START, so
// only the byte cap can make TryBuild fire; one byte under the cap must not.

TEST_CASE_FIXTURE(SodiumFixture,
                  "1 MiB byte cap fires ground-wait when parser non-Ground") {
    auto p = MakePump();
    p->FeedBytes(&ESC_BYTE, 1);
    REQUIRE(p->ParserState() != EParserState::Ground);

    auto immediate = p->OnNeedsSnapshot(24, 80, START);
    CHECK_FALSE(immediate.has_value());
    CHECK(p->HasPendingSnap());

    // One byte short of the cap: still waiting.
    const std::vector<std::byte> filler(GROUND_WAIT_BYTES - 1, std::byte{0x00});
    p->FeedBytes(filler.data(), filler.size());
    REQUIRE(p->ParserState() != EParserState::Ground);
    {
        auto r = p->TryBuild(START);
        REQUIRE(r.has_value());
        CHECK_FALSE(r->has_value());
    }

    // The byte that reaches the cap: the build fires, at the same clock.
    p->FeedBytes(&ESC_BYTE, 1);
    auto r = p->TryBuild(START);
    REQUIRE(r.has_value());
    REQUIRE(r->has_value());
    CHECK((*r)->Flags != 0);  // forced out of a non-Ground parser
    p->ReleaseSnapshot();
}

// ── Test 11: OnNeedsSnapshot(0,0) on non-80×24 grid uses actual size ───────
//
// Kills: 0×0 means 80×24 instead of the grid's current size (the snapshot
// would be 80×24 instead of 40×10).

TEST_CASE_FIXTURE(SodiumFixture,
                  "OnNeedsSnapshot(0,0) on non-80x24 grid uses actual size") {
    auto p = MakePump(40, 10);
    const auto ready = BuildNow(*p, 0, 0);
    const std::vector<std::byte> got{ready.Bytes.begin(), ready.Bytes.end()};
    CHECK(p->GetGrid().Cols() == 40);
    CHECK(p->GetGrid().Rows() == 10);

    auto ref = MakePump(40, 10);
    const auto refReady = BuildNow(*ref, 10, 40);
    const std::vector<std::byte> want{refReady.Bytes.begin(),
                                      refReady.Bytes.end()};
    CHECK(got == want);
    p->ReleaseSnapshot();
}

// ── Agent tests ────────────────────────────────────────────────────────────
//
// Shared shape for the snapshot tests: connection 1 is grid-capable, goes Live
// and is dropped, so the session is Detached with Resync overflow.  Printing
// more than DisconnectMax (512) bytes then overflows the ring, which makes
// connection 2 take path B: the agent asks for a snapshot (NeedsSnapshot).

constexpr std::size_t OVERFLOW_BYTES = 600;

// ── Agent: pty bytes reach the grid exactly once ───────────────────────────
//
// Kills: MovePtyOutput does not call GridPump_->FeedBytes (the snapshot is of
// a blank grid), and a FeedBytes call that feeds a range twice (the snapshot
// differs from the reference).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: pty bytes reach the grid exactly once") {
    AgentRig rig{71};
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);

    const std::vector<std::byte> printed(OVERFLOW_BYTES, std::byte{'A'});
    rig.PtyPrints(printed);
    SleepMs(300);

    auto c2 = rig.Connect();
    c2->QueueResize(24, 80);
    REQUIRE(rig.AwaitSnapshot(*c2));

    const auto& seen = rig.Finish();
    REQUIRE(seen.Seen);
    CHECK(seen.GridBytes == OVERFLOW_BYTES);
    CHECK(c2->Snapshots.front() == ReferenceSnapshot({printed}, 24, 80));
}

// ── Agent: bytes the session cannot take are fed to the grid once ──────────
//
// A Live client that never acks leaves the ring full, so the rest of what the
// shell printed stays in PtyRead_ (PtyFrom_ < PtyTo_) and MovePtyOutput runs
// on every loop turn.  Extra connections force loop turns.
//
// Kills: GridFed_ does not advance after FeedBytes (the same bytes are fed on
// every turn, GridBytes is a multiple of 1000).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: bytes the session cannot take are fed to the grid "
                  "once") {
    AgentRig rig{72};
    auto idle = rig.Connect();
    idle->QueueResize(24, 80);
    REQUIRE(idle->RunUntil(8000, [&] { return idle->IsLive(); }));
    // `idle` is not pumped from here on, so it never acks.

    const std::vector<std::byte> printed(1000, std::byte{'A'});
    rig.PtyPrints(printed);
    SleepMs(200);

    std::vector<io::UniqueFd> pokes;
    for (int i = 0; i < 5; ++i) {
        pokes.push_back(TcpConnect(rig.Port));
        SleepMs(50);
    }

    const auto& seen = rig.Finish();
    REQUIRE(seen.Seen);
    CHECK(seen.GridBytes == printed.size());
    CHECK(seen.Cell00 == static_cast<char32_t>('A'));
}

// ── Agent: bytes fed while waiting for Ground are not replayed ─────────────
//
// The shell stops inside an escape sequence, so the snapshot waits for Ground
// (TrySupplySnapshot).  The rest of the sequence arrives during the wait: the
// grid takes it, the session (Live, ring full) cannot, and once the snapshot
// is supplied those bytes must not be written to the new generation.
//
// Kills: `PtyFrom_ = PtyTo_` removed from TrySupplySnapshot (the client gets
// "[HQ" again after the snapshot).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: bytes fed while waiting for Ground are not "
                  "replayed") {
    AgentRig rig{73};
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);

    std::vector<std::byte> first(OVERFLOW_BYTES, std::byte{'A'});
    first.push_back(ESC_BYTE);
    rig.PtyPrints(first);
    SleepMs(300);

    auto c2 = rig.Connect();
    c2->QueueResize(24, 80);
    c2->Run(300);
    c2->Run(200);
    REQUIRE(c2->Snapshots.empty());  // parser is inside the escape

    const auto rest = Bytes("[HQ");  // CSI H, then a printable
    rig.PtyPrints(rest);
    REQUIRE(rig.AwaitSnapshot(*c2));
    CHECK(c2->Snapshots.front() == ReferenceSnapshot({first, rest}, 24, 80));

    c2->Run(300);
    rig.PtyPrints(Bytes("Z"));
    // Wait for the Z itself, not for a fixed time: anything replayed from
    // before the snapshot would be ahead of it in the stream.
    const auto z = Bytes("Z");
    REQUIRE(c2->RunUntil(5000, [&] {
        return !c2->Delivered.empty() && c2->Delivered.back() == z.front();
    }));
    CHECK(c2->Delivered == z);
}

// ── Agent: bytes fed while waiting for the Resize are not replayed ─────────
//
// The client withholds its post-Resume Resize, so the agent holds off
// NeedsSnapshot (up to RESIZE_WAIT_TIMEOUT).  The shell prints during that
// time; the parser is in Ground, so the snapshot is supplied inline in Pump
// as soon as the Resize arrives.
//
// Kills: `PtyFrom_ = PtyTo_` removed from the inline supply in Pump.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: bytes fed while waiting for the Resize are not "
                  "replayed") {
    AgentRig rig{74};
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);

    const std::vector<std::byte> first(OVERFLOW_BYTES, std::byte{'A'});
    rig.PtyPrints(first);
    SleepMs(300);

    auto c2 = rig.Connect();  // no QueueResize yet
    c2->Run(300);
    REQUIRE(c2->Snapshots.empty());

    const auto rest = Bytes("QQQ");
    rig.PtyPrints(rest);
    SleepMs(200);

    c2->QueueResize(24, 80);
    REQUIRE(rig.AwaitSnapshot(*c2));
    CHECK(c2->Snapshots.front() == ReferenceSnapshot({first, rest}, 24, 80));

    c2->Run(300);
    rig.PtyPrints(Bytes("Z"));
    // Wait for the Z itself, not for a fixed time: anything replayed from
    // before the snapshot would be ahead of it in the stream.
    const auto z = Bytes("Z");
    REQUIRE(c2->RunUntil(5000, [&] {
        return !c2->Delivered.empty() && c2->Delivered.back() == z.front();
    }));
    CHECK(c2->Delivered == z);
}

// ── Agent: a reconnect snapshot does not wait out RESIZE_WAIT_TIMEOUT ───────
//
// The client's Resize follows its Resume, usually in the same read.  The agent
// must take it off the buffer on the same turn; if it does not, NeedsSnapshot
// waits for RESIZE_WAIT_TIMEOUT (2 s) with nothing to wake the loop.
//
// Kills: PumpResume returning Idle after the path-B stage change.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a reconnect snapshot does not wait out "
                  "RESIZE_WAIT_TIMEOUT") {
    AgentRig rig{77};
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);
    rig.PtyPrints(std::vector<std::byte>(OVERFLOW_BYTES, std::byte{'A'}));
    SleepMs(300);

    auto c = rig.Connect();
    c->QueueResize(24, 80);
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(rig.AwaitSnapshot(*c));
    CHECK(std::chrono::steady_clock::now() - start <
          std::chrono::milliseconds{1500});
}

// ── Agent: no Resize and no known size: the snapshot follows the wait ───────
//
// With no size ever reported the agent still waits for the Resize (design.md
// §9, step 1).  None comes, nothing else is on the wire, and only the
// connection's deadline can wake the loop; after RESIZE_WAIT_TIMEOUT the
// snapshot is built at the grid's size.
//
// Kills: no wake-up for the Resize wait when no size is known.

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: with no Resize and no known size the snapshot "
                  "follows the Resize wait") {
    AgentRig rig{79};
    {
        auto c = rig.Connect();  // never sends a Resize
        REQUIRE(c->RunUntil(8000, [&] { return c->LiveFramesSent >= 1; }));
        rig.AwaitResumeTaken(*c);
    }
    SleepMs(300);  // the agent notices the drop
    rig.PtyPrints(std::vector<std::byte>(OVERFLOW_BYTES, std::byte{'A'}));
    SleepMs(300);

    auto c = rig.Connect();
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(rig.AwaitSnapshot(*c, 6000));
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(took >= std::chrono::milliseconds{1500});
    CHECK(took < std::chrono::milliseconds{4000});
}

// ── Agent: the pty gets the size the grid has ──────────────────────────────
//
// A Resize of 0×0 is accepted; the grid clamps it to 1×1, and the pty must be
// told the same, not 0×0.
//
// Kills: the pty resized with the raw Resize instead of the grid's size.

TEST_CASE_FIXTURE(SodiumFixture, "Agent: a 0x0 Resize leaves the pty at 1x1") {
    AgentRig rig{80};
    auto c = rig.Connect();
    c->QueueResize(30, 100);
    REQUIRE(c->RunUntil(8000, [&] { return c->LiveFramesSent >= 2; }));
    rig.AwaitPtySize(*c, 30, 100);

    c->QueueResize(0, 0);
    REQUIRE(c->RunUntil(8000, [&] {
        const auto ws = rig.SlaveSize();
        return ws.ws_row != 30 || ws.ws_col != 100;
    }));
    const auto size = rig.SlaveSize();
    CHECK(size.ws_row == 1);
    CHECK(size.ws_col == 1);
}

// ── Agent: a second snapshot cycle succeeds and frees the first ────────────
//
// Cycle 2 waits for Ground, which is the path that needs the first snapshot
// released.
//
// Kills: GridEmissionDone never returns true / is not consulted, and
// ReleaseSnapshot never called (HasSnapshot stays true at exit and the second
// wait never builds).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: a second snapshot cycle succeeds and frees the "
                  "first") {
    AgentRig rig{75};

    // Cycle 1.
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);
    const std::vector<std::byte> a(OVERFLOW_BYTES, std::byte{'A'});
    rig.PtyPrints(a);
    SleepMs(300);
    {
        auto c = rig.Connect();
        c->QueueResize(24, 80);
        REQUIRE(rig.AwaitSnapshot(*c));
        c->Run(300);  // GridEnd sent, GridApplied back: the pump releases.
    }
    SleepMs(300);

    // Cycle 2: overflow again, with the parser left inside an escape.
    rig.ConnectAndDrop(24, 80);
    SleepMs(300);
    std::vector<std::byte> b(OVERFLOW_BYTES, std::byte{'B'});
    b.push_back(ESC_BYTE);
    rig.PtyPrints(b);
    SleepMs(300);
    auto c4 = rig.Connect();
    c4->QueueResize(24, 80);
    c4->Run(300);
    const auto rest = Bytes("[H");
    rig.PtyPrints(rest);
    REQUIRE(rig.AwaitSnapshot(*c4));
    c4->Run(300);

    const auto& seen = rig.Finish();
    REQUIRE(seen.Seen);
    CHECK_FALSE(seen.HasSnapshot);
    CHECK_FALSE(seen.HasPendingSnap);
}

// ── Agent: Resized is forwarded to the grid ───────────────────────────────
//
// Kills: the Resized handler does not call GridPump_->HandleResize (the grid
// stays 80×24 while the shell prints at 40 columns, so the snapshot differs
// from the reference).

TEST_CASE_FIXTURE(SodiumFixture, "Agent: Resized is forwarded to the grid") {
    AgentRig rig{76};
    rig.ConnectAndDrop(10, 40);
    SleepMs(300);

    const std::vector<std::byte> printed(OVERFLOW_BYTES, std::byte{'A'});
    rig.PtyPrints(printed);
    SleepMs(300);

    auto c2 = rig.Connect();
    c2->QueueResize(10, 40);
    REQUIRE(rig.AwaitSnapshot(*c2));

    const auto& seen = rig.Finish();
    REQUIRE(seen.Seen);
    CHECK(seen.Cols == 40);
    CHECK(seen.Rows == 10);

    // The grid was resized before the shell's bytes were fed.
    auto ref = MakePump(80, 24);
    REQUIRE(ref->HandleResize(40, 10).has_value());
    ref->FeedBytes(printed.data(), printed.size());
    const auto ready = BuildNow(*ref, 10, 40);
    CHECK(c2->Snapshots.front() ==
          std::vector<std::byte>{ready.Bytes.begin(), ready.Bytes.end()});
}

// ── Input for a shell that has gone ────────────────────────────────────────
//
// The test seam presets the child as exited, so the pty never takes what the
// client types.  The agent drops it and acknowledges it as gone, which is how
// the client's window opens again (design.md §9); the agent does not hold on
// to input nobody will read.
//
// Kills: RunLoop does not clear PtyInput_ once the child has gone (the
// window stays full, the client never writes again).

TEST_CASE_FIXTURE(SodiumFixture,
                  "Agent: input for a shell that has gone is dropped and "
                  "acknowledged") {
    AgentRig rig{78};
    const std::vector<std::byte> typed(TinyLimits().ResendMax, std::byte{'k'});
    REQUIRE(rig.Session().Write(typed) == typed.size());
    CHECK_FALSE(rig.Session().AcceptsWrites());  // the window is full

    auto c = rig.Connect();
    c->QueueResize(10, 40);
    CHECK(c->RunUntil(
        8000, [&] { return c->IsLive() && rig.Session().AcceptsWrites(); }));
}
