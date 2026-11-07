// A Resume with a stale stream_gen forces path B (design.md §9, "Путь B"),
// with nothing else pushing the session there: the ring is not overflowed and
// the replay is cheap, so the cost check alone would pick path A.
//
// The other path-B tests get there through ring overflow (PrimeForPathB,
// ForceB) and would pass without the stream_gen rule.  Here the overflow is
// spent on the first snapshot, which opens generation 1 and clears the flag;
// the client never confirms it (the link went during the transfer, before
// GridApplied), so it comes back with generation 0.
//
// Mutant these tests were checked against (applied by hand):
//   connection.cpp: a Resume with a stale stream_gen does not force path B
//                                  -> "a client that never applied ..."

#include "grid_path_helpers.hpp"

#include <string>
#include <variant>
#include <vector>

namespace {

/// First connection: live, then dropped; the ring is then overflowed in
/// Resync mode, so the next connection must take path B.
void OverflowAfterOneConnection(Rig& r) {
    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                       wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        START,
                        /*capable=*/true,
                        wire::PROTOCOL_VERSION_GRID,
                        MutableByteSpan{r.Sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));
    ac.Close(START);
    const std::vector<std::byte> fill(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.Agent.Write(ByteSpan{fill});
    }
}

/// The path-B connection that delivers a snapshot.  With `confirm` the client
/// applies it and sends GridApplied; without, the link is dropped as soon as
/// the client has the snapshot, before anything is confirmed.
void DeliverSnapshot(Rig& r, bool confirm) {
    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                       wire::PROTOCOL_VERSION_GRID};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        START,
                        /*capable=*/true,
                        wire::PROTOCOL_VERSION_GRID,
                        MutableByteSpan{r.Sp.SnapshotBuf}};
    cc.QueueResize(wire::ResizeMessage{.Rows = 24, .Cols = 80});
    REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));
    const std::vector<std::byte> snap(16, std::byte{0x22});
    bool applied = false;
    for (int i = 0; i < 100 && !applied; ++i) {
        auto ea = PumpAgent(ac, r.Sp, START);
        REQUIRE(ea.has_value());
        if (std::holds_alternative<NeedsSnapshot>(*ea)) {
            ac.SupplySnapshot(START, ByteSpan{snap}, 0, 0);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            REQUIRE(Feed(cc, r.Sp, sf->Written));
        }
        auto ec = PumpClient(cc, r.Sp, START);
        REQUIRE(ec.has_value());
        if (const auto* as = std::get_if<ApplySnapshot>(&*ec)) {
            applied = true;
            if (confirm) {
                cc.ConfirmApply(as->StreamGen);
            }
        } else if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            REQUIRE(Feed(ac, r.Sp, sf->Written));
        }
    }
    REQUIRE(applied);
    ac.Close(START);
}

}  // namespace

// Kills: a Resume whose stream_gen is behind the agent's generation but is
// answered by a replay (path A) because the ring is intact and the replay is
// cheap: the client would then splice old-generation offsets into the new
// generation's stream.

TEST_CASE_FIXTURE(SodiumFixture,
                  "stale stream_gen: a client that never applied the "
                  "snapshot is sent a new one although the ring is intact") {
    Rig r;
    OverflowAfterOneConnection(r);
    DeliverSnapshot(r, /*confirm=*/false);

    // The snapshot opened generation 1 and spent the overflow (the control
    // below shows the same setup resuming by replay); the client still holds
    // generation 0; the replay is a few bytes.
    REQUIRE(r.Client.StreamGen() == 0);
    const std::vector<std::byte> little(8, std::byte{0x5A});
    std::ignore = r.Agent.Write(ByteSpan{little});

    CHECK(Reconnect(r, /*capable=*/true, wire::PROTOCOL_VERSION_GRID) ==
          "NeedsSnapshot");
}

// The control: the same session, same cheap replay and intact ring, but the
// client confirmed generation 1, so nothing is stale and path A resumes.

TEST_CASE_FIXTURE(SodiumFixture,
                  "stale stream_gen: a client that applied the snapshot "
                  "resumes by replay") {
    Rig r;
    OverflowAfterOneConnection(r);
    DeliverSnapshot(r, /*confirm=*/true);

    REQUIRE(r.Client.StreamGen() == 1);
    const std::vector<std::byte> little(8, std::byte{0x5A});
    std::ignore = r.Agent.Write(ByteSpan{little});

    CHECK(Reconnect(r, /*capable=*/true, wire::PROTOCOL_VERSION_GRID) ==
          "no-snapshot");
}
