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

template <class TR>
void ForceB(TR& r, Timestamp now = START) {
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
    template <class TR>
    Link(TR& r, Timestamp now)
        : Ac{MutableByteSpan{r.Sp.AgentFrames}, now, PROTOCOL_VERSION_GRID},
          Cc{r.Client,
             MakeNonce(++r.Nonces),
             MutableByteSpan{r.Sp.ClientFrames},
             now,
             true,
             PROTOCOL_VERSION_GRID,
             MutableByteSpan{r.Sp.SnapshotBuf}} {}
    AgentConnection Ac;
    ClientConnection Cc;
    int Needs = 0, Applies = 0, AgentFramesAfterSupply = 0,
        ClientFramesAfterApply = 0;
    bool Supplied = false, AgentErr = false, ClientErr = false;
    std::vector<std::byte> Applied, Delivered;
    std::optional<SessionError> Aerr, Cerr;
    std::optional<std::uint32_t> AppliedGen;
    // Options
    const std::vector<std::byte>* Snap = nullptr;
    bool Supply = true;
    int CutAgentAfter =
        -1;  // stop feeding agent→client after N frames post-supply
    enum EMode { Confirm, Deny, Nothing } Mode = Confirm;
    bool DropClientAfterApply = false;
    bool Cut = false;

    template <class TR>
    void Step(TR& r, Timestamp now) {
        if (!AgentErr) {
            auto ea = PumpAgent(Ac, r.Sp, now);
            if (!ea) {
                AgentErr = true;
                Aerr = ea.error();
            } else if (auto* ns = std::get_if<NeedsSession>(&*ea)) {
                Ac.Route(r.Agent.Ref(), MakeNonce(++r.Nonces));
            } else if (std::holds_alternative<NeedsSnapshot>(*ea)) {
                ++Needs;
                if (Supply && Snap) {
                    Ac.SupplySnapshot(now, ByteSpan{*Snap}, 0, 0);
                    Supplied = true;
                }
            } else if (auto* sf = std::get_if<SendFrame>(&*ea)) {
                bool feed = true;
                if (Supplied) {
                    ++AgentFramesAfterSupply;
                    if (CutAgentAfter >= 0 &&
                        AgentFramesAfterSupply > CutAgentAfter) {
                        feed = false;
                        Cut = true;
                    }
                }
                if (feed && !ClientErr) {
                    if (!Feed(Cc, r.Sp, sf->Written)) {
                        ClientErr = true;
                    }
                }
            }
        }
        if (!ClientErr) {
            auto ec = PumpClient(Cc, r.Sp, now);
            if (!ec) {
                ClientErr = true;
                Cerr = ec.error();
                return;
            }
            if (auto* as = std::get_if<ApplySnapshot>(&*ec)) {
                ++Applies;
                Applied.assign(as->Bytes.begin(), as->Bytes.end());
                AppliedGen = as->StreamGen;
                if (Mode == Confirm) {
                    Cc.ConfirmApply(as->StreamGen);
                } else if (Mode == Deny) {
                    Cc.DenyApply(as->StreamGen);
                }
            } else if (auto* d = std::get_if<Deliver>(&*ec)) {
                Delivered.insert(Delivered.end(), d->Bytes.begin(),
                                 d->Bytes.end());
            } else if (auto* sf = std::get_if<SendFrame>(&*ec)) {
                if (AppliedGen) {
                    ++ClientFramesAfterApply;
                }
                if (!(DropClientAfterApply && AppliedGen) && !AgentErr) {
                    if (!Feed(Ac, r.Sp, sf->Written)) {
                        AgentErr = true;
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
    REQUIRE(DriveToLive(l.Ac, l.Cc, r.Agent, r.Sp, START, r.Nonces));
    for (int i = 0; i < 5; ++i) {
        auto e = PumpAgent(l.Ac, r.Sp, START);
        REQUIRE(e.has_value());
        CHECK(std::holds_alternative<Idle>(*e));
    }
    const auto dl = l.Ac.NextDeadline();
    REQUIRE(dl.IsSet());
    MESSAGE("deadline-from-start ns=" << (dl.At() - START).count());
    CHECK(dl.At() == START + RESIZE_WAIT_TIMEOUT);
    // just before: still Idle
    auto e1 = PumpAgent(l.Ac, r.Sp, START + RESIZE_WAIT_TIMEOUT - 1ns);
    REQUIRE(e1.has_value());
    CHECK(std::holds_alternative<Idle>(*e1));
    // at expiry: NeedsSnapshot fires with the size from ForceB (24×80)
    auto e2 = PumpAgent(l.Ac, r.Sp, START + RESIZE_WAIT_TIMEOUT);
    REQUIRE(e2.has_value());
    REQUIRE(std::holds_alternative<NeedsSnapshot>(*e2));
    CHECK(std::get<NeedsSnapshot>(*e2).Rows == 24);
    CHECK(std::get<NeedsSnapshot>(*e2).Cols == 80);
    MESSAGE("after expiry deadline set=" << l.Ac.NextDeadline().IsSet());
}

// ── PROBE1b: SupplySnapshot wait is bounded (SUPPLY_SNAPSHOT_TIMEOUT) ────────

TEST_CASE_FIXTURE(SodiumFixture, "PROBE1b SupplySnapshot wait is bounded") {
    LiveRig r;
    ForceB(r);
    Link l(r, START);
    l.Supply = false;
    REQUIRE(DriveToLive(l.Ac, l.Cc, r.Agent, r.Sp, START, r.Nonces));
    l.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    Timestamp now = START;
    // 2 h simulated, 0.5 s ticks; SUPPLY_SNAPSHOT_TIMEOUT (30 s) must fire.
    for (int i = 0; i < 2 * 3600 * 2 && !l.AgentErr && !l.ClientErr; ++i) {
        now = now + std::chrono::milliseconds{500};
        l.Step(r, now);
    }
    MESSAGE("after 2h: needs="
            << l.Needs << " agentErr=" << l.AgentErr
            << " clientErr=" << l.ClientErr << " agent.IsLive=" << l.Ac.IsLive()
            << " nextDeadlineSet=" << l.Ac.NextDeadline().IsSet());
    if (l.Aerr) {
        MESSAGE("agent err: " << l.Aerr->Description());
    }
    CHECK(l.AgentErr);                                // connection must close
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
        l.Snap = &snap;
        l.Mode = Link::Deny;
        REQUIRE(DriveToLive(l.Ac, l.Cc, r.Agent, r.Sp, START, r.Nonces));
        l.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        REQUIRE(l.Applies == 1);
        const std::vector<std::byte> z(8, std::byte{0x5A});
        std::ignore = r.Agent.Write(ByteSpan{z});
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        MESSAGE("after Deny: delivered=" << l.Delivered.size()
                                         << " gen=" << r.Client.StreamGen()
                                         << " g0=" << g0);
        CHECK(
            l.Delivered.empty());  // agent still awaiting GridApplied → no data
        CHECK(r.Client.StreamGen() == g0);
        l.Ac.Close(START);
    }
    Link l2(r, START);
    l2.Supply = false;
    REQUIRE(DriveToLive(l2.Ac, l2.Cc, r.Agent, r.Sp, START, r.Nonces));
    l2.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 40; ++i) {
        l2.Step(r, START);
    }
    MESSAGE("reconnect after Deny: needs=" << l2.Needs);
    CHECK(l2.Needs >= 1);  // old gen carried → path B again
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
        l.Snap = &snap;
        l.Mode = Link::Nothing;
        REQUIRE(DriveToLive(l.Ac, l.Cc, r.Agent, r.Sp, START, r.Nonces));
        l.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        REQUIRE(l.Applies == 1);
        const std::vector<std::byte> z(8, std::byte{0x5A});
        std::ignore = r.Agent.Write(ByteSpan{z});
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        CHECK(l.Delivered.empty());  // not confirmed → agent suppresses data
        CHECK(r.Client.StreamGen() == 0);
        l.Cc.ConfirmApply(*l.AppliedGen);
        CHECK(r.Client.StreamGen() == *l.AppliedGen);
        for (int i = 0; i < 80; ++i) {
            l.Step(r, START);
        }
        CHECK(l.Delivered.size() == 8);
        l.Ac.Close(START);
    }
    Link l2(r, START);
    l2.Supply = false;
    REQUIRE(DriveToLive(l2.Ac, l2.Cc, r.Agent, r.Sp, START, r.Nonces));
    for (int i = 0; i < 40; ++i) {
        l2.Step(r, START);
    }
    CHECK(l2.Needs == 0);
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
    a->Snap = &snap;
    a->Mode = Link::Nothing;
    REQUIRE(DriveToLive(a->Ac, a->Cc, r.Agent, r.Sp, START, r.Nonces));
    a->Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 80; ++i) {
        a->Step(r, START);
    }
    REQUIRE(a->Applies == 1);
    const auto genA = *a->AppliedGen;
    a->Ac.Close(START);
    // second path-B round on a new connection, confirmed
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.Agent.Write(ByteSpan{chunk});
    }
    Link b(r, START);
    b.Snap = &snap;
    b.Mode = Link::Confirm;
    for (int i = 0; i < 200; ++i) {
        b.Step(r, START);
    }
    b.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 200; ++i) {
        b.Step(r, START);
    }
    REQUIRE(b.Applies == 1);
    const auto genB = r.Client.StreamGen();
    MESSAGE("genA=" << genA << " genB=" << genB);
    a->Cc.ConfirmApply(genA);  // stale connection — must be ignored
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
        l.Snap = &snap;
        l.CutAgentAfter = cutAfter;
        l.Mode = confirm ? Link::Confirm : Link::Nothing;
        l.DropClientAfterApply = dropGridApplied;
        REQUIRE(DriveToLive(l.Ac, l.Cc, r.Agent, r.Sp, START, r.Nonces));
        l.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 200; ++i) {
            l.Step(r, START);
        }
        MESSAGE("cut after " << cutAfter << " frames: applies=" << l.Applies
                             << " agentFrames=" << l.AgentFramesAfterSupply);
        l.Ac.Close(START);
    }
    const std::uint32_t gen1 = r.Client.StreamGen();
    const std::vector<std::byte> post(10, std::byte{0x77});
    std::ignore = r.Agent.Write(ByteSpan{post});
    Link l2(r, START);
    l2.Snap = &snap;
    l2.Mode = Link::Confirm;
    l2.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 300; ++i) {
        l2.Step(r, START);
    }
    MESSAGE("reconnect: needs="
            << l2.Needs << " applies=" << l2.Applies << " gen1=" << gen1
            << " gen2=" << r.Client.StreamGen()
            << " delivered=" << l2.Delivered.size() << " cerr=" << l2.ClientErr
            << " aerr=" << l2.AgentErr);
    CHECK_FALSE(l2.ClientErr);
    CHECK_FALSE(l2.AgentErr);
    const bool gotSnapshot = l2.Applies == 1;
    if (gotSnapshot) {
        CHECK(l2.Applied == snap);
    }
    // Either a fresh snapshot arrived or (confirmed locally) path A continues;
    // the 10 post-bytes arrive exactly once in either case.
    if (gotSnapshot) {
        CHECK(l2.Delivered.empty());
    } else {
        CHECK(l2.Delivered == post);
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
    l.Supply = false;
    REQUIRE(DriveToLive(l.Ac, l.Cc, r.Agent, r.Sp, START, r.Nonces));
    l.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 40; ++i) {
        l.Step(r, START);
    }
    CHECK(l.Needs == 1);
    std::vector<std::byte> empty;
    l.Ac.SupplySnapshot(START, ByteSpan{empty}, 0, 0);
    for (int i = 0; i < 40; ++i) {
        l.Step(r, START);
    }
    MESSAGE("needs after empty supply=" << l.Needs
                                        << " IsLive=" << l.Ac.IsLive());
    CHECK(l.Needs == 2);
    CHECK_FALSE(l.AgentErr);
    // oversize
    std::vector<std::byte> big(wire::SNAPSHOT_MAX_BYTES + 1);
    l.Ac.SupplySnapshot(START, ByteSpan{big}, 0, 0);
    for (int i = 0; i < 40; ++i) {
        l.Step(r, START);
    }
    CHECK(l.Needs == 3);
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
    // coordinator decision: Write consumes in Resync, returns full length;
    // the probe's original *w == 0 contradicted the design.
    CHECK(*w == more.size());        // consumes and drops: returns full length
    CHECK(r.Agent.AcceptsWrites());  // stays true; caller loop is safe

    // After path B, the 0x33 bytes are consumed/dropped and NOT retained by
    // the caller.  Only bytes written AFTER GridApplied appear in the new
    // generation.  Write a distinct marker to verify new-generation delivery.
    const std::vector<std::byte> snap(16, std::byte{0x11});
    Link l(r, START);
    l.Snap = &snap;
    l.Mode = Link::Confirm;
    for (int i = 0; i < 60; ++i) l.Step(r, START);
    l.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 100; ++i) l.Step(r, START);
    REQUIRE(l.Applies == 1);

    const std::vector<std::byte> marker(12, std::byte{0xAA});
    auto wm = r.Agent.Write(ByteSpan{marker});
    REQUIRE(wm.has_value());
    CHECK(*wm == marker.size());
    for (int i = 0; i < 100; ++i) l.Step(r, START);
    MESSAGE("post-snapshot marker: delivered=" << l.Delivered.size());
    CHECK(l.Delivered == marker);  // only the 0xAA marker; no 0x33 bytes
}

// ── R4-2: stream_gen ahead of session_gen is MalformedField ──────────────────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "R4-2 Resume stream_gen ahead of session_gen is MalformedField") {
    for (int ahead = 0; ahead < 2; ++ahead) {
        Rig r;
        r.Client.ProbeSetStreamGen(ahead ? 5u : 0u);
        Link l(r, START);
        l.Supply = false;
        for (int i = 0; i < 60; ++i) {
            l.Step(r, START);
        }
        MESSAGE("ahead=" << ahead << " AgentErr=" << l.AgentErr
                         << " IsLive=" << l.Ac.IsLive()
                         << (l.Aerr
                                 ? " err=" + std::string(l.Aerr->Description())
                                 : std::string{}));
        if (ahead != 0) {
            REQUIRE(l.AgentErr);
            REQUIRE(l.Aerr.has_value());
            CHECK(l.Aerr->Disposition() == EDisposition::CloseConnection);
            REQUIRE(l.Aerr->Proto().has_value());
            CHECK(*l.Aerr->Proto() == EProtoError::MalformedField);
            CHECK_FALSE(l.Ac.IsLive());
        } else {
            CHECK_FALSE(l.AgentErr);
            CHECK(l.Ac.IsLive());
        }
    }
}

// ── R4-3: replayed GridBegin gen <= client StreamGen is rejected
// ──────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "R4-3 replayed GridBegin gen<=client StreamGen is rejected") {
    Rig r;
    ForceB(r);
    const std::vector<std::byte> snap(16, std::byte{0x11});
    {
        Link l(r, START);
        l.Snap = &snap;
        l.Mode = Link::Confirm;
        REQUIRE(DriveToLive(l.Ac, l.Cc, r.Agent, r.Sp, START, r.Nonces));
        l.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 100; ++i) {
            l.Step(r, START);
        }
        REQUIRE(l.Applies == 1);
        l.Ac.Close(START);
    }
    REQUIRE(r.Client.StreamGen() == 1);
    Link l2(r, START);
    REQUIRE(DriveToLive(l2.Ac, l2.Cc, r.Agent, r.Sp, START, r.Nonces));
    for (int i = 0; i < 20; ++i) {
        l2.Step(r, START);
    }
    REQUIRE(l2.Ac.IsLive());
    // Directly call StartGrid with gen=1. The client's channel was seeded with
    // agentStreamGen=Session_.StreamGen()=1, so gen=1 <= baseline=1 →
    // UnexpectedMessage on the client side.
    std::ignore = l2.Ac.ProbeChannel().StartGrid(ByteSpan{snap}, 0, 1, 0);
    for (int i = 0; i < 40; ++i) {
        l2.Step(r, START);
    }
    MESSAGE("ClientErr=" << l2.ClientErr << " Applies=" << l2.Applies);
    CHECK(l2.ClientErr);
    CHECK(l2.Applies == 0);
}

// ── R4-6: partial-fill Resync Write returns bytes.size(), not partial count ──

TEST_CASE_FIXTURE(SodiumFixture,
                  "R4-6 partial-fill Resync Write returns bytes.size() not "
                  "partial count") {
    Rig r;
    // Establish a grid-capable connection (sets Resync overflow mode) then
    // drop.
    {
        AgentConnection ac{MutableByteSpan{r.Sp.AgentFrames}, START,
                           PROTOCOL_VERSION_GRID};
        ClientConnection cc{r.Client,
                            MakeNonce(++r.Nonces),
                            MutableByteSpan{r.Sp.ClientFrames},
                            START,
                            true,
                            PROTOCOL_VERSION_GRID,
                            MutableByteSpan{r.Sp.SnapshotBuf}};
        REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));
        cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        for (int i = 0; i < 10; ++i) {
            auto ea = PumpAgent(ac, r.Sp, START);
            if (!ea) break;
            if (auto* sf = std::get_if<SendFrame>(&*ea))
                std::ignore = Feed(cc, r.Sp, sf->Written);
            auto ec = PumpClient(cc, r.Sp, START);
            if (!ec) break;
            if (auto* sf = std::get_if<SendFrame>(&*ec))
                std::ignore = Feed(ac, r.Sp, sf->Written);
        }
        ac.Close(START);  // agent enters Detached/Resync
    }
    // DisconnectMax=512 (TinyLimits). Write 300 bytes: fits in ring.
    const std::vector<std::byte> first(300, std::byte{0xCC});
    auto w1 = r.Agent.Write(ByteSpan{first});
    REQUIRE(w1.has_value());
    CHECK(*w1 == first.size());  // fits completely
    CHECK(r.Agent.AcceptsWrites());

    // Write 300 more: only 212 bytes of room remain; triggers partial-fill
    // path. Before the fix: returned 212. After: returns 300 (bytes.size()).
    const std::vector<std::byte> second(300, std::byte{0xDD});
    auto w2 = r.Agent.Write(ByteSpan{second});
    REQUIRE(w2.has_value());
    MESSAGE("partial-fill Write returned " << *w2 << " expected "
                                           << second.size());
    CHECK(*w2 == second.size());
    // Ring is now overflowed; AcceptsWrites stays true so PTY loop keeps
    // draining.
    CHECK(r.Agent.AcceptsWrites());

    // Subsequent writes hit the ring_overflowed fast path: also return
    // bytes.size().
    const std::vector<std::byte> third(400, std::byte{0xEE});
    auto w3 = r.Agent.Write(ByteSpan{third});
    REQUIRE(w3.has_value());
    CHECK(*w3 == third.size());
}

// ── R4-4: GRID_APPLIED_TIMEOUT fires when client never calls ConfirmApply ────

TEST_CASE_FIXTURE(
    SodiumFixture,
    "R4-4 GRID_APPLIED_TIMEOUT closes connection, session survives") {
    LiveRig r;
    ForceB(r);
    const std::vector<std::byte> snap(16, std::byte{0x22});
    Link l(r, START);
    l.Snap = &snap;
    l.Mode = Link::Nothing;  // client receives ApplySnapshot but never confirms
    REQUIRE(DriveToLive(l.Ac, l.Cc, r.Agent, r.Sp, START, r.Nonces));
    l.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    // Step with 0.5s ticks; GRID_APPLIED_TIMEOUT (30 s) must fire within 2 h.
    Timestamp now = START;
    for (int i = 0; i < 2 * 3600 * 2 && !l.AgentErr && !l.ClientErr; ++i) {
        now = now + std::chrono::milliseconds{500};
        l.Step(r, now);
    }
    MESSAGE("R4-4: applies=" << l.Applies << " agentErr=" << l.AgentErr
                             << " clientErr=" << l.ClientErr
                             << " state=" << static_cast<int>(r.Agent.State()));
    if (l.Aerr) {
        MESSAGE("agent err: " << l.Aerr->Description());
    }
    CHECK(l.Applies >= 1);                            // snapshot was delivered
    CHECK(l.AgentErr);                                // connection must close
    CHECK(r.Agent.State() != ESessionState::Reaped);  // session must survive
}

// ── PROBE2g: ConfirmApply with wrong gen is silently ignored ─────────────────

TEST_CASE_FIXTURE(SodiumFixture,
                  "PROBE2g ConfirmApply wrong gen is silently ignored, "
                  "StreamGen unchanged") {
    Rig r;
    ForceB(r);
    const std::vector<std::byte> snap(16, std::byte{0x33});
    Link l(r, START);
    l.Snap = &snap;
    l.Mode = Link::Nothing;  // we handle ConfirmApply manually below
    REQUIRE(DriveToLive(l.Ac, l.Cc, r.Agent, r.Sp, START, r.Nonces));
    l.Cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
    for (int i = 0; i < 100; ++i) {
        l.Step(r, START);
    }
    REQUIRE(l.Applies == 1);
    REQUIRE(l.AppliedGen.has_value());
    const auto correct = *l.AppliedGen;
    const std::uint32_t wrong = correct + 1;
    // Confirm with the wrong gen: guard must discard it.
    l.Cc.ConfirmApply(wrong);
    MESSAGE("PROBE2g: correct=" << correct << " wrong=" << wrong
                                << " StreamGen=" << r.Client.StreamGen());
    CHECK(r.Client.StreamGen() == 0);  // no advance: wrong gen was rejected
    // Now confirm with the correct gen to verify the guard is the cause.
    l.Cc.ConfirmApply(correct);
    CHECK(r.Client.StreamGen() == correct);
}
