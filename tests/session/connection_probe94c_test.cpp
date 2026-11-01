// Connection-level verifier probes for PR 94.  Uses grid_path_helpers.hpp.
#include "grid_path_helpers.hpp"

#include <algorithm>
#include <string>

namespace {
using namespace zet::wire;

// ── LiveRig: same layout as Rig but with non-zero liveness timers ────────

struct LiveRig {
    SessionLimits Limits = [] {
        auto l = TinyLimits();
        l.Liveness =
            LivenessLimits{.KeepaliveInterval = std::chrono::seconds{1},
                           .PeerSilenceTimeout = std::chrono::seconds{3}};
        return l;
    }();
    std::vector<std::byte> As =
        std::vector<std::byte>(Limits.DisconnectMax + 256);
    std::vector<std::byte> Cs = std::vector<std::byte>(Limits.ResendMax + 256);
    AgentSession Agent{
        DeriveSessionSecrets(MakeKey(33)),
        SessionStreams{
            .Storage = MutableByteSpan{As}, .Now = START, .Limits = Limits}};
    ClientSession Client{
        DeriveSessionSecrets(MakeKey(33)),
        SessionStreams{
            .Storage = MutableByteSpan{Cs}, .Now = START, .Limits = Limits}};
    Space Sp;
    std::uint64_t Nonces = 0;
};

// ── ForceB: drive first connection to Live, then drop + overflow ring ─────
//
// Works for both Rig and LiveRig (uppercase field names required in both).

template <class R>
void ForceB(R& r, Timestamp now = START) {
    AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, now,
                       PROTOCOL_VERSION_GRID};
    ClientConnection cc{r.Client,
                        MakeNonce(++r.Nonces),
                        MutableByteSpan{r.Sp.ClientFrames},
                        now,
                        true,
                        PROTOCOL_VERSION_GRID,
                        MutableByteSpan{r.Sp.SnapshotBuf}};
    REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, now, r.Nonces));
    cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 40; ++i) {
        auto ea = PumpAgent(ac, r.Sp, now);
        if (!ea) {
            break;
        }
        if (auto* sf = std::get_if<SendFrame>(&*ea)) {
            std::ignore = Feed(cc, r.Sp, sf->Written);
        }
        auto ec = PumpClient(cc, r.Sp, now);
        if (!ec) {
            break;
        }
        if (auto* sf = std::get_if<SendFrame>(&*ec)) {
            std::ignore = Feed(ac, r.Sp, sf->Written);
        }
    }
    ac.Close(now);
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.Agent.Write(ByteSpan{chunk});
    }
}

// ── Link: connection pair with an integrated step-and-collect loop ────────

struct Link {
    template <class R>
    Link(R& r, Timestamp now)
        : ac{MutableByteSpan{r.Sp.AgentFrames}, now, PROTOCOL_VERSION_GRID},
          cc{r.Client,
             MakeNonce(++r.Nonces),
             MutableByteSpan{r.Sp.ClientFrames},
             now,
             true,
             PROTOCOL_VERSION_GRID,
             MutableByteSpan{r.Sp.SnapshotBuf}} {}
    AgentConnection ac;
    ClientConnection cc;
    int needs = 0, applies = 0, agentFramesAfterSupply = 0,
        clientFramesAfterApply = 0;
    bool supplied = false, agentErr = false, clientErr = false;
    std::vector<std::byte> applied, delivered;
    std::optional<SessionError> aerr, cerr;
    std::optional<std::uint32_t> appliedGen;
    // Options
    const std::vector<std::byte>* snap = nullptr;
    bool supply = true;
    int cutAgentAfter =
        -1;  // stop feeding agent→client after N frames post-supply
    enum Mode { Confirm, Deny, Nothing } mode = Confirm;
    bool dropClientAfterApply = false;
    bool cut = false;

    template <class R>
    void Step(R& r, Timestamp now) {
        if (!agentErr) {
            auto ea = PumpAgent(ac, r.Sp, now);
            if (!ea) {
                agentErr = true;
                aerr = ea.error();
            } else if (auto* ns = std::get_if<NeedsSession>(&*ea)) {
                ac.Route(r.Agent.Ref(), MakeNonce(++r.Nonces));
            } else if (std::holds_alternative<NeedsSnapshot>(*ea)) {
                ++needs;
                if (supply && snap) {
                    ac.SupplySnapshot(now, ByteSpan{*snap}, 0, 0);
                    supplied = true;
                }
            } else if (auto* sf = std::get_if<SendFrame>(&*ea)) {
                bool feed = true;
                if (supplied) {
                    ++agentFramesAfterSupply;
                    if (cutAgentAfter >= 0 &&
                        agentFramesAfterSupply > cutAgentAfter) {
                        feed = false;
                        cut = true;
                    }
                }
                if (feed && !clientErr) {
                    if (!Feed(cc, r.Sp, sf->Written)) {
                        clientErr = true;
                    }
                }
            }
        }
        if (!clientErr) {
            auto ec = PumpClient(cc, r.Sp, now);
            if (!ec) {
                clientErr = true;
                cerr = ec.error();
                return;
            }
            if (auto* as = std::get_if<ApplySnapshot>(&*ec)) {
                ++applies;
                applied.assign(as->Bytes.begin(), as->Bytes.end());
                appliedGen = as->StreamGen;
                if (mode == Confirm) {
                    cc.ConfirmApply(as->StreamGen);
                } else if (mode == Deny) {
                    cc.DenyApply(as->StreamGen);
                }
            } else if (auto* d = std::get_if<Deliver>(&*ec)) {
                delivered.insert(delivered.end(), d->Bytes.begin(),
                                 d->Bytes.end());
            } else if (auto* sf = std::get_if<SendFrame>(&*ec)) {
                if (appliedGen) {
                    ++clientFramesAfterApply;
                }
                if (!(dropClientAfterApply && appliedGen) && !agentErr) {
                    if (!Feed(ac, r.Sp, sf->Written)) {
                        agentErr = true;
                    }
                }
            }
        }
    }
};

}  // namespace

// ── PROBE1: resize-wait deadline value ───────────────────────────────────────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "PROBE1 resize-wait deadline is exactly START+RESIZE_WAIT_TIMEOUT") {
    Rig r;
    ForceB(r);
    Link l(r, START);
    REQUIRE(DriveToLive(l.ac, l.cc, r.Agent, r.Sp, START, r.Nonces));
    for (int i = 0; i < 5; ++i) {
        auto e = PumpAgent(l.ac, r.Sp, START);
        REQUIRE(e.has_value());
        CHECK(std::holds_alternative<Idle>(*e));
    }
    const auto dl = l.ac.NextDeadline();
    REQUIRE(dl.IsSet());
    MESSAGE("deadline-from-start ns=" << (dl.At() - START).count());
    CHECK(dl.At() == START + RESIZE_WAIT_TIMEOUT);
    // just before: still Idle
    auto e1 = PumpAgent(l.ac, r.Sp, START + RESIZE_WAIT_TIMEOUT - 1ns);
    REQUIRE(e1.has_value());
    CHECK(std::holds_alternative<Idle>(*e1));
    // at expiry: NeedsSnapshot fires with the size from ForceB (24×80)
    auto e2 = PumpAgent(l.ac, r.Sp, START + RESIZE_WAIT_TIMEOUT);
    REQUIRE(e2.has_value());
    REQUIRE(std::holds_alternative<NeedsSnapshot>(*e2));
    CHECK(std::get<NeedsSnapshot>(*e2).Rows == 24);
    CHECK(std::get<NeedsSnapshot>(*e2).Cols == 80);
    MESSAGE("after expiry deadline set=" << l.ac.NextDeadline().IsSet());
}

// ── PROBE1b: SupplySnapshot wait is bounded (SUPPLY_SNAPSHOT_TIMEOUT) ────────

TEST_CASE_FIXTURE(SodiumFixture, "PROBE1b SupplySnapshot wait is bounded") {
    LiveRig r;
    ForceB(r);
    Link l(r, START);
    l.supply = false;
    REQUIRE(DriveToLive(l.ac, l.cc, r.Agent, r.Sp, START, r.Nonces));
    l.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    Timestamp now = START;
    // 2 h simulated, 0.5 s ticks; SUPPLY_SNAPSHOT_TIMEOUT (30 s) must fire.
    for (int i = 0; i < 2 * 3600 * 2 && !l.agentErr && !l.clientErr; ++i) {
        now = now + std::chrono::milliseconds{500};
        l.Step(r, now);
    }
    MESSAGE("after 2h: needs="
            << l.needs << " agentErr=" << l.agentErr
            << " clientErr=" << l.clientErr << " agent.IsLive=" << l.ac.IsLive()
            << " nextDeadlineSet=" << l.ac.NextDeadline().IsSet());
    if (l.aerr) {
        MESSAGE("agent err: " << l.aerr->Description());
    }
    CHECK(l.agentErr);                                // connection must close
    CHECK(r.Agent.State() != ESessionState::Reaped);  // session must survive
}

// ── PROBE2: Deny leaves StreamGen unchanged, next Resume takes path B
// ─────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "PROBE2 Deny: no GridApplied; next Resume carries old gen") {
    Rig r;
    ForceB(r);
    const std::vector<std::byte> snap(16, std::byte{0x11});
    const auto g0 = r.Client.StreamGen();
    {
        Link l(r, START);
        l.snap = &snap;
        l.mode = Link::Deny;
        REQUIRE(DriveToLive(l.ac, l.cc, r.Agent, r.Sp, START, r.Nonces));
        l.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        REQUIRE(l.applies == 1);
        const std::vector<std::byte> z(8, std::byte{0x5A});
        std::ignore = r.Agent.Write(ByteSpan{z});
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        MESSAGE("after Deny: delivered=" << l.delivered.size()
                                         << " gen=" << r.Client.StreamGen()
                                         << " g0=" << g0);
        CHECK(
            l.delivered.empty());  // agent still awaiting GridApplied → no data
        CHECK(r.Client.StreamGen() == g0);
        l.ac.Close(START);
    }
    Link l2(r, START);
    l2.supply = false;
    REQUIRE(DriveToLive(l2.ac, l2.cc, r.Agent, r.Sp, START, r.Nonces));
    l2.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 40; ++i) {
        l2.Step(r, START);
    }
    MESSAGE("reconnect after Deny: needs=" << l2.needs);
    CHECK(l2.needs >= 1);  // old gen carried → path B again
}

// ── PROBE2b: Confirm sends GridApplied, next Resume takes path A
// ──────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "PROBE2b Confirm: GridApplied sent only after ConfirmApply; "
                  "next Resume path A") {
    Rig r;
    ForceB(r);
    const std::vector<std::byte> snap(16, std::byte{0x11});
    {
        Link l(r, START);
        l.snap = &snap;
        l.mode = Link::Nothing;
        REQUIRE(DriveToLive(l.ac, l.cc, r.Agent, r.Sp, START, r.Nonces));
        l.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        REQUIRE(l.applies == 1);
        const std::vector<std::byte> z(8, std::byte{0x5A});
        std::ignore = r.Agent.Write(ByteSpan{z});
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        CHECK(l.delivered.empty());  // not confirmed → agent suppresses data
        CHECK(r.Client.StreamGen() == 0);
        l.cc.ConfirmApply(*l.appliedGen);
        CHECK(r.Client.StreamGen() == *l.appliedGen);
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        CHECK(l.delivered.size() == 8);
        l.ac.Close(START);
    }
    Link l2(r, START);
    l2.supply = false;
    REQUIRE(DriveToLive(l2.ac, l2.cc, r.Agent, r.Sp, START, r.Nonces));
    for (int i = 0; i < 40; ++i) {
        l2.Step(r, START);
    }
    CHECK(l2.needs == 0);
}

// ── PROBE2c: late ConfirmApply from superseded connection must not regress
// StreamGen

TEST_CASE_FIXTURE(SodiumFixture,
                  "PROBE2c late ConfirmApply from superseded connection does "
                  "not regress StreamGen") {
    Rig r;
    ForceB(r);
    const std::vector<std::byte> snap(16, std::byte{0x11});
    std::optional<Link> a;
    a.emplace(r, START);
    a->snap = &snap;
    a->mode = Link::Nothing;
    REQUIRE(DriveToLive(a->ac, a->cc, r.Agent, r.Sp, START, r.Nonces));
    a->cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 80; ++i) {
        a->Step(r, START);
    }
    REQUIRE(a->applies == 1);
    const auto genA = *a->appliedGen;
    a->ac.Close(START);
    // second path-B round on a new connection, confirmed
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.Agent.Write(ByteSpan{chunk});
    }
    Link b(r, START);
    b.snap = &snap;
    b.mode = Link::Confirm;
    for (int i = 0; i < 200; ++i) {
        b.Step(r, START);
    }
    b.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 200; ++i) {
        b.Step(r, START);
    }
    REQUIRE(b.applies == 1);
    const auto genB = r.Client.StreamGen();
    MESSAGE("genA=" << genA << " genB=" << genB);
    a->cc.ConfirmApply(genA);  // stale connection — must be ignored
    MESSAGE("StreamGen after stale confirm=" << r.Client.StreamGen());
    CHECK(r.Client.StreamGen() == genB);
}

// ── PROBE5a: path A byte-identical replay ────────────────────────────────────

TEST_CASE_FIXTURE(SodiumFixture, "PROBE5a path A strict equality") {
    const SessionLimits limits = TinyLimits();
    std::vector<std::byte> as(limits.DisconnectMax + 256),
        cs(limits.ResendMax + 256);
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
    std::vector<std::byte> all;
    auto wr = [&](std::size_t n, std::uint8_t b0) {
        std::vector<std::byte> v(n);
        for (std::size_t i = 0; i < n; ++i) {
            v[i] = std::byte(b0 + i * 13);
        }
        all.insert(all.end(), v.begin(), v.end());
        std::ignore = agent.Write(ByteSpan{v});
    };
    wr(300, 1);
    std::vector<std::byte> got;
    auto run = [&](bool collect) {
        AgentConnection ac{MutableByteSpan{sp.AgentFrames}, START};
        ClientConnection cc{client, MakeNonce(++nonces),
                            MutableByteSpan{sp.ClientFrames}, START};
        for (int i = 0; i < 400; ++i) {
            auto ea = PumpAgent(ac, sp, START);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<NeedsSession>(*ea)) {
                ac.Route(agent.Ref(), MakeNonce(++nonces));
            } else if (auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(cc, sp, sf->Written));
            }
            auto ec = PumpClient(cc, sp, START);
            REQUIRE(ec.has_value());
            if (auto* d = std::get_if<Deliver>(&*ec)) {
                if (collect) {
                    got.insert(got.end(), d->Bytes.begin(), d->Bytes.end());
                }
            }
            if (auto* sf = std::get_if<SendFrame>(&*ec)) {
                REQUIRE(Feed(ac, sp, sf->Written));
            }
        }
        ac.Close(START);
    };
    run(true);
    wr(200, 9);
    run(true);
    CHECK(got == all);
}

// ── PROBE5b: cuts at various points in the grid emission ─────────────────────

namespace {
void CutCase(int cutAfter, bool confirm, bool dropGridApplied) {
    Rig r;
    ForceB(r);
    std::vector<std::byte> snap(2 * GRID_CHUNK_MAX_DATA + 17);
    for (std::size_t i = 0; i < snap.size(); ++i) {
        snap[i] = std::byte(i * 31 + 7);
    }
    {
        Link l(r, START);
        l.snap = &snap;
        l.cutAgentAfter = cutAfter;
        l.mode = confirm ? Link::Confirm : Link::Nothing;
        l.dropClientAfterApply = dropGridApplied;
        REQUIRE(DriveToLive(l.ac, l.cc, r.Agent, r.Sp, START, r.Nonces));
        l.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 200; ++i) {
            l.Step(r, START);
        }
        MESSAGE("cut after " << cutAfter << " frames: applies=" << l.applies
                             << " agentFrames=" << l.agentFramesAfterSupply);
        l.ac.Close(START);
    }
    const std::uint32_t gen1 = r.Client.StreamGen();
    const std::vector<std::byte> post(10, std::byte{0x77});
    std::ignore = r.Agent.Write(ByteSpan{post});
    Link l2(r, START);
    l2.snap = &snap;
    l2.mode = Link::Confirm;
    l2.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 300; ++i) {
        l2.Step(r, START);
    }
    MESSAGE("reconnect: needs="
            << l2.needs << " applies=" << l2.applies << " gen1=" << gen1
            << " gen2=" << r.Client.StreamGen()
            << " delivered=" << l2.delivered.size() << " cerr=" << l2.clientErr
            << " aerr=" << l2.agentErr);
    CHECK_FALSE(l2.clientErr);
    CHECK_FALSE(l2.agentErr);
    const bool gotSnapshot = l2.applies == 1;
    if (gotSnapshot) {
        CHECK(l2.applied == snap);
    }
    // Either a fresh snapshot arrived or (confirmed locally) path A continues;
    // the 10 post-bytes arrive exactly once in either case.
    if (gotSnapshot) {
        CHECK(l2.delivered.empty());
    } else {
        CHECK(l2.delivered == post);
    }
    CHECK(r.Client.StreamGen() >= 1);
}
}  // namespace

TEST_CASE_FIXTURE(SodiumFixture, "PROBE5b cut after GridBegin") {
    CutCase(1, false, false);
}
TEST_CASE_FIXTURE(SodiumFixture, "PROBE5b cut after first chunk") {
    CutCase(2, false, false);
}
TEST_CASE_FIXTURE(
    SodiumFixture,
    "PROBE5b cut after GridEnd, applied+confirmed, GridApplied lost") {
    CutCase(1000, true, true);
}
TEST_CASE_FIXTURE(
    SodiumFixture,
    "PROBE5b cut after GridEnd, applied not confirmed, GridApplied lost") {
    CutCase(1000, false, true);
}

// ── PROBE7: empty and oversize snapshot at connection level
// ───────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "PROBE7 empty and oversize Supply at connection level") {
    Rig r;
    ForceB(r);
    Link l(r, START);
    l.supply = false;
    REQUIRE(DriveToLive(l.ac, l.cc, r.Agent, r.Sp, START, r.Nonces));
    l.cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 40; ++i) {
        l.Step(r, START);
    }
    CHECK(l.needs == 1);
    std::vector<std::byte> empty;
    l.ac.SupplySnapshot(START, ByteSpan{empty}, 0, 0);
    for (int i = 0; i < 40; ++i) {
        l.Step(r, START);
    }
    MESSAGE("needs after empty supply=" << l.needs
                                        << " IsLive=" << l.ac.IsLive());
    CHECK(l.needs == 2);
    CHECK_FALSE(l.agentErr);
    // oversize
    std::vector<std::byte> big(wire::SNAPSHOT_MAX_BYTES + 1);
    l.ac.SupplySnapshot(START, ByteSpan{big}, 0, 0);
    for (int i = 0; i < 40; ++i) {
        l.Step(r, START);
    }
    CHECK(l.needs == 3);
}

// ── PROBE8: Write contract in Resync past overflow
// ────────────────────────────
//
// After ring overflow (Resync mode) Write returns bytes.size() — not 0.
// AcceptsWrites() stays true so the caller loop keeps running.

TEST_CASE_FIXTURE(SodiumFixture,
                  "PROBE8 Resync Write past overflow: returns full length, "
                  "AcceptsWrites stays true") {
    Rig r;
    ForceB(r);  // ring already overflowed in Resync mode
    const std::vector<std::byte> more(100, std::byte{0x33});
    auto w = r.Agent.Write(ByteSpan{more});
    MESSAGE("Write past overflow -> ok="
            << w.has_value() << " n=" << (w ? *w : 0)
            << " AcceptsWrites=" << r.Agent.AcceptsWrites());
    CHECK(w.has_value());
    CHECK(*w == more.size());        // consumes and drops: returns full length
    CHECK(r.Agent.AcceptsWrites());  // stays true; caller loop is safe
}
