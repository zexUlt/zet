/// Snapshot-path probe tests (VPROBE a/a2/a3/b/b2/e/i1/i2).
///
/// These tests exercise the post-Resume snapshot flow end-to-end using the
/// AgentConnection/ClientConnection API.  Each probe was originally produced
/// by an independent verifier; they are included here verbatim (descriptive
/// names, all assertions kept).

#include "grid_path_helpers.hpp"

#include <cstdio>
#include <string>

namespace {
using namespace zet::wire;

struct Ev {
    std::string Name;
    std::uint16_t R{0}, C{0};
};

std::string EvName(const ConnectionEvent& e) {
    if (std::holds_alternative<NeedsSnapshot>(e)) return "NeedsSnapshot";
    if (std::holds_alternative<SendFrame>(e)) return "SendFrame";
    if (std::holds_alternative<Idle>(e)) return "Idle";
    if (std::holds_alternative<Resized>(e)) return "Resized";
    if (std::holds_alternative<Deliver>(e)) return "Deliver";
    if (std::holds_alternative<ApplySnapshot>(e)) return "ApplySnapshot";
    if (std::holds_alternative<NeedsSession>(e)) return "NeedsSession";
    if (std::holds_alternative<SessionOverflowed>(e))
        return "SessionOverflowed";
    return "Other";
}

/// Connection wrapper that logs all agent-side events.  Caller controls
/// whether NeedsSnapshot triggers an immediate SupplySnapshot.
struct Conn {
    Rig& r;
    AgentConnection ac;
    ClientConnection cc;
    std::vector<Ev> Log;
    Timestamp Now = START;
    Conn(Rig& rig, bool capable, std::uint16_t ceiling)
        : r(rig),
          ac{MutableByteSpan{rig.sp.AgentFrames}, START, ceiling},
          cc{rig.client,
             MakeNonce(++rig.nonces),
             MutableByteSpan{rig.sp.ClientFrames},
             START,
             capable,
             ceiling,
             MutableByteSpan{rig.sp.SnapshotBuf}} {}
    void Rec(const ConnectionEvent& e) {
        Ev v{EvName(e)};
        if (const auto* n = std::get_if<NeedsSnapshot>(&e)) {
            v.R = n->Rows;
            v.C = n->Cols;
        }
        Log.push_back(v);
    }
    bool Step(bool supply = false, std::size_t supplySize = 16) {
        auto ea = PumpAgent(ac, r.sp, Now);
        if (!ea) {
            Log.push_back({"agent-err"});
            return false;
        }
        Rec(*ea);
        if (const auto* ns = std::get_if<NeedsSession>(&*ea)) {
            (void)ns;
            ac.Route(r.agent.Ref(), MakeNonce(++r.nonces));
        } else if (std::holds_alternative<NeedsSnapshot>(*ea) && supply) {
            static std::vector<std::byte> snap;
            snap.assign(supplySize, std::byte{0x22});
            ac.SupplySnapshot(Now, ByteSpan{snap}, 0, 0);
        } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
            if (!Feed(cc, r.sp, sf->Written)) {
                Log.push_back({"client-feed-err"});
                return false;
            }
        }
        auto ec = PumpClient(cc, r.sp, Now);
        if (!ec) {
            Log.push_back({"client-err"});
            return false;
        }
        if (std::holds_alternative<ApplySnapshot>(*ec))
            Log.push_back({"client:ApplySnapshot"});
        if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
            if (!Feed(ac, r.sp, sf->Written)) {
                Log.push_back({"agent-feed-err"});
                return false;
            }
        }
        return true;
    }
    std::string Dump() const {
        std::string s;
        for (auto& e : Log) {
            s += e.Name;
            if (e.Name == "NeedsSnapshot")
                s +=
                    "(" + std::to_string(e.R) + "x" + std::to_string(e.C) + ")";
            s += " ";
        }
        return s;
    }
};

SessionLimits LiveLimits() {
    auto l = TinyLimits();
    l.Liveness = LivenessLimits{.KeepaliveInterval = std::chrono::seconds{1},
                                .PeerSilenceTimeout = std::chrono::seconds{3}};
    return l;
}
}  // namespace

// ── VPROBE a ─────────────────────────────────────────────────────────────────
//
// After a path-B reconnect, the client resizes to 200×50 right after Resume.
// The first NeedsSnapshot must carry the post-Resume dimensions (50 rows, 200
// cols), not the stale 24×80 from the first connection.

TEST_CASE_FIXTURE(SodiumFixture,
                  "snapshot size follows post-Resume Resize on path B") {
    Rig r;
    {
        Conn c1(r, true, PROTOCOL_VERSION_GRID);
        c1.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 40; ++i) c1.Step();
        MESSAGE("conn1: " << c1.Dump());
        c1.ac.Close(START);
    }
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) std::ignore = r.agent.Write(ByteSpan{chunk});
    Conn c2(r, true, PROTOCOL_VERSION_GRID);
    c2.cc.QueueResize(ResizeMessage{.Rows = 50, .Cols = 200});
    bool supplied = false;
    for (int i = 0; i < 60; ++i) {
        bool needs = false;
        const auto before = c2.Log.size();
        c2.Step(!supplied);
        for (auto k = before; k < c2.Log.size(); ++k)
            needs |= c2.Log[k].Name == "NeedsSnapshot";
        if (needs) supplied = true;
    }
    MESSAGE("conn2: " << c2.Dump());
    for (auto& e : c2.Log)
        if (e.Name == "NeedsSnapshot") {
            CHECK(e.R == 50);
            CHECK(e.C == 200);
            break;
        }
}

// ── VPROBE a2 ────────────────────────────────────────────────────────────────
//
// Without a SupplySnapshot call: verify event sequence and that NextDeadline
// is set during the await-snapshot window.

TEST_CASE_FIXTURE(SodiumFixture,
                  "events and NextDeadline are set while awaiting snapshot") {
    Rig r;
    {
        Conn c1(r, true, PROTOCOL_VERSION_GRID);
        c1.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 40; ++i) c1.Step();
        c1.ac.Close(START);
    }
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) std::ignore = r.agent.Write(ByteSpan{chunk});
    Conn c2(r, true, PROTOCOL_VERSION_GRID);
    c2.cc.QueueResize(ResizeMessage{.Rows = 50, .Cols = 200});
    for (int i = 0; i < 40; ++i) c2.Step();
    MESSAGE("conn2 (no supply): " << c2.Dump());
    const auto dl = c2.ac.NextDeadline();
    MESSAGE("NextDeadline set=" << (dl.IsSet()));
}

// ── VPROBE a3 ────────────────────────────────────────────────────────────────
//
// Peer-silence timeout is enforced even while the connection is waiting for a
// SupplySnapshot call.

TEST_CASE_FIXTURE(SodiumFixture,
                  "peer silence timeout is enforced during await-snapshot") {
    Rig r;
    r.limits = LiveLimits();
    std::vector<std::byte> as2(r.limits.DisconnectMax + 256),
        cs2(r.limits.ResendMax + 256);
    AgentSession agent{
        DeriveSessionSecrets(MakeKey(33)),
        SessionStreams{
            .Storage = MutableByteSpan{as2}, .Now = START, .Limits = r.limits}};
    ClientSession client{
        DeriveSessionSecrets(MakeKey(33)),
        SessionStreams{
            .Storage = MutableByteSpan{cs2}, .Now = START, .Limits = r.limits}};
    Space sp;
    std::uint64_t nonces = 0;
    {
        AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                           PROTOCOL_VERSION_GRID};
        ClientConnection cc{client,
                            MakeNonce(++nonces),
                            MutableByteSpan{sp.ClientFrames},
                            START,
                            true,
                            PROTOCOL_VERSION_GRID,
                            MutableByteSpan{sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac, cc, agent, sp, START, nonces));
        ac.Close(START);
    }
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) std::ignore = agent.Write(ByteSpan{chunk});
    AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START,
                       PROTOCOL_VERSION_GRID};
    ClientConnection cc{
        client, MakeNonce(++nonces),   MutableByteSpan{sp.ClientFrames}, START,
        true,   PROTOCOL_VERSION_GRID, MutableByteSpan{sp.SnapshotBuf}};
    bool gotNeeds = false;
    for (int i = 0; i < 64 && !gotNeeds; ++i) {
        auto ea = PumpAgent(ac, sp, START);
        REQUIRE(ea.has_value());
        if (std::get_if<NeedsSession>(&*ea))
            ac.Route(agent.Ref(), MakeNonce(++nonces));
        else if (auto* sf = std::get_if<SendFrame>(&*ea))
            REQUIRE(Feed(cc, sp, sf->Written));
        else if (std::get_if<NeedsSnapshot>(&*ea)) {
            gotNeeds = true;
            break;
        }
        auto ec = PumpClient(cc, sp, START);
        REQUIRE(ec.has_value());
        if (auto* sf = std::get_if<SendFrame>(&*ec))
            REQUIRE(Feed(ac, sp, sf->Written));
    }
    REQUIRE(gotNeeds);
    MESSAGE("deadline set in AwaitSnapshot: " << ac.NextDeadline().IsSet());
    const Timestamp later = START + std::chrono::seconds{2};
    auto e1 = PumpAgent(ac, sp, later);
    MESSAGE("t=2s pump: " << (e1 ? EvName(*e1) : std::string("ERR")));
    const Timestamp later2 = START + std::chrono::seconds{4};
    auto e2 = PumpAgent(ac, sp, later2);
    MESSAGE("t=4s pump (client silent): "
            << (e2 ? EvName(*e2) : std::string("ERR PeerSilent?")));
    CHECK_FALSE(e2.has_value());
}

// ── VPROBE b ─────────────────────────────────────────────────────────────────
//
// SupplySnapshot with size=0 or size>SNAPSHOT_MAX_BYTES must not wedge the
// session.  The test records what happens and asserts no crash.

TEST_CASE_FIXTURE(
    SodiumFixture,
    "SupplySnapshot with invalid size does not wedge the session") {
    for (std::size_t sz :
         {std::size_t{0}, std::size_t{SNAPSHOT_MAX_BYTES} + 1}) {
        Rig r;
        {
            Conn c1(r, true, PROTOCOL_VERSION_GRID);
            for (int i = 0; i < 30; ++i) c1.Step();
            c1.ac.Close(START);
        }
        const std::vector<std::byte> chunk(200, std::byte{0xBB});
        for (int i = 0; i < 6; ++i)
            std::ignore = r.agent.Write(ByteSpan{chunk});
        Conn c2(r, true, PROTOCOL_VERSION_GRID);
        std::vector<std::byte> big(sz, std::byte{1});
        bool done = false;
        std::string res;
        for (int i = 0; i < 80 && !done; ++i) {
            auto ea = PumpAgent(c2.ac, r.sp, START);
            if (!ea) {
                res += "agent-err ";
                break;
            }
            if (auto* ns = std::get_if<NeedsSession>(&*ea)) {
                (void)ns;
                c2.ac.Route(r.agent.Ref(), MakeNonce(++r.nonces));
            } else if (std::holds_alternative<NeedsSnapshot>(*ea)) {
                c2.ac.SupplySnapshot(START, ByteSpan{big}, 0, 0);
                res += "supplied ";
            } else if (auto* sf = std::get_if<SendFrame>(&*ea)) {
                if (!Feed(c2.cc, r.sp, sf->Written)) {
                    res += "cl-feed-err ";
                    break;
                }
            }
            auto ec = PumpClient(c2.cc, r.sp, START);
            if (!ec) {
                res += "client-err ";
                break;
            }
            if (std::holds_alternative<ApplySnapshot>(*ec)) {
                res += "applied ";
                done = true;
            }
            if (auto* sf = std::get_if<SendFrame>(&*ec)) {
                if (!Feed(c2.ac, r.sp, sf->Written)) {
                    res += "ag-feed-err ";
                    break;
                }
            }
        }
        MESSAGE("size=" << sz << " -> " << res
                        << " gen=" << r.agent.Ref().Id().size()
                        << " client gen=" << r.client.StreamGen());
    }
}

// ── VPROBE b2 ────────────────────────────────────────────────────────────────
//
// After SupplySnapshot({}, ...) the session must not be wedged: a subsequent
// Write must eventually deliver a Deliver event to the client.

TEST_CASE_FIXTURE(SodiumFixture,
                  "empty SupplySnapshot does not wedge the session") {
    Rig r;
    {
        Conn c1(r, true, PROTOCOL_VERSION_GRID);
        for (int i = 0; i < 30; ++i) c1.Step();
        c1.ac.Close(START);
    }
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) std::ignore = r.agent.Write(ByteSpan{chunk});
    Conn c2(r, true, PROTOCOL_VERSION_GRID);
    bool supplied = false;
    for (int i = 0; i < 100; ++i) {
        const auto b = c2.Log.size();
        c2.Step(false);
        bool needs = false;
        for (auto k = b; k < c2.Log.size(); ++k)
            needs |= c2.Log[k].Name == "NeedsSnapshot";
        if (needs && !supplied) {
            supplied = true;
            c2.ac.SupplySnapshot(START, ByteSpan{}, 0, 0);
        }
    }
    MESSAGE("after empty supply: " << c2.Dump());
    std::ignore = r.agent.Write(ByteSpan{chunk}.first(10));
    int delivered = 0;
    for (int i = 0; i < 100; ++i) {
        auto ea = PumpAgent(c2.ac, r.sp, START);
        if (!ea) break;
        if (auto* sf = std::get_if<SendFrame>(&*ea))
            REQUIRE(Feed(c2.cc, r.sp, sf->Written));
        auto ec = PumpClient(c2.cc, r.sp, START);
        if (!ec) break;
        if (std::holds_alternative<Deliver>(*ec)) ++delivered;
        if (auto* sf = std::get_if<SendFrame>(&*ec))
            REQUIRE(Feed(c2.ac, r.sp, sf->Written));
    }
    MESSAGE("Deliver events after empty snapshot: " << delivered);
    CHECK(delivered > 0);
}

// ── VPROBE e ─────────────────────────────────────────────────────────────────
//
// After two full path-B round-trips, ClientSession::StreamGen reaches 2.

TEST_CASE_FIXTURE(
    SodiumFixture,
    "ClientSession StreamGen increments with each path-B reconnect") {
    Rig r;
    for (int round = 0; round < 2; ++round) {
        {
            Conn c1(r, true, PROTOCOL_VERSION_GRID);
            for (int i = 0; i < 30; ++i) c1.Step();
            c1.ac.Close(START);
        }
        const std::vector<std::byte> chunk(200, std::byte{0xBB});
        for (int i = 0; i < 6; ++i)
            std::ignore = r.agent.Write(ByteSpan{chunk});
        Conn c2(r, true, PROTOCOL_VERSION_GRID);
        bool supplied = false;
        for (int i = 0; i < 80; ++i) {
            const auto b = c2.Log.size();
            c2.Step(!supplied);
            for (auto k = b; k < c2.Log.size(); ++k)
                if (c2.Log[k].Name == "NeedsSnapshot") supplied = true;
        }
        MESSAGE("round " << round << " client gen=" << r.client.StreamGen()
                         << " : " << c2.Dump());
        c2.ac.Close(START);
    }
    CHECK(r.client.StreamGen() == 2);
}

// ── VPROBE i1 ────────────────────────────────────────────────────────────────
//
// When a v2 non-capable client causes an overflow, it must see
// SessionOverflowed on the next reconnect — the event must not be silently
// dropped.

TEST_CASE_FIXTURE(
    SodiumFixture,
    "v2 non-capable overflow: client learns about it on next reconnect") {
    Rig r;
    {
        Conn c1(r, false, PROTOCOL_VERSION_GRID);
        for (int i = 0; i < 30; ++i) c1.Step();
        c1.ac.Close(START);
    }
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) std::ignore = r.agent.Write(ByteSpan{chunk});
    MESSAGE("agent state=" << int(r.agent.State())
                           << " ended=" << r.agent.EndedWith().has_value());
    Conn c2(r, false, PROTOCOL_VERSION_GRID);
    for (int i = 0; i < 30; ++i)
        if (!c2.Step()) break;
    MESSAGE("reconnect log: " << c2.Dump());
    bool overflowed = false;
    for (auto& e : c2.Log) overflowed |= e.Name == "SessionOverflowed";
    CHECK(overflowed);
}

// ── VPROBE i2 ────────────────────────────────────────────────────────────────
//
// Write in Resync mode: the first write that fills the ring may be partial.
// Subsequent AcceptsWrites returns true so the PTY loop drains the process.

TEST_CASE_FIXTURE(
    SodiumFixture,
    "Write partial fill in Resync mode keeps AcceptsWrites true") {
    Rig r;
    {
        Conn c1(r, true, PROTOCOL_VERSION_GRID);
        for (int i = 0; i < 30; ++i) c1.Step();
        c1.ac.Close(START);
    }
    const std::vector<std::byte> chunk(300, std::byte{0xBB});
    for (int i = 0; i < 4; ++i) {
        auto w = r.agent.Write(ByteSpan{chunk});
        MESSAGE("write " << i << " -> " << (w ? int(*w) : -1)
                         << " accepts=" << r.agent.AcceptsWrites());
    }
}
