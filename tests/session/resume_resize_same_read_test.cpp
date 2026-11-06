/// Path B when the client's Resize arrives in the same read as its Resume.
///
/// AgentConnection used to return Idle right after the Resume moved it to
/// AwaitSnapshot.  A caller that pumps only when the socket is readable then
/// has nothing to wake it: the Resize sits in the frame buffer until another
/// byte arrives or RESIZE_WAIT_TIMEOUT runs out.

#include "grid_path_helpers.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <optional>

namespace {
using namespace zet::wire;

/// Copies the bytes of the frame a Pump just wrote into `out`.
std::vector<std::byte> TakeFrame(const Space& sp, std::size_t written) {
    const auto first = sp.Out.begin();
    return {first, first + static_cast<std::ptrdiff_t>(written)};
}

/// A path-B reconnect up to the point where the client has sent its Resume,
/// which is held back.  The client then yields further frames on demand, so a
/// test can put them into one read in any order.  `knownSize` says whether the
/// agent has been told a terminal size before.
struct HeldReconnect {
    explicit HeldReconnect(bool knownSize, SessionLimits limits = TinyLimits())
        : R(limits) {
        {
            AgentConnection ac{MutableByteSpan{R.Sp.AgentFrames}, START,
                               PROTOCOL_VERSION_GRID};
            ClientConnection cc{R.Client,
                                MakeNonce(++R.Nonces),
                                MutableByteSpan{R.Sp.ClientFrames},
                                START,
                                true,
                                PROTOCOL_VERSION_GRID,
                                MutableByteSpan{R.Sp.SnapshotBuf}};
            if (knownSize) {
                cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
            }
            REQUIRE(DriveToLive(ac, cc, R.Agent, R.Sp, START, R.Nonces));
            for (int i = 0; i < 20; ++i) {
                auto ec = PumpClient(cc, R.Sp, START);
                REQUIRE(ec.has_value());
                if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                    REQUIRE(Feed(ac, R.Sp, sf->Written));
                }
                REQUIRE(PumpAgent(ac, R.Sp, START).has_value());
            }
            ac.Close(START);
        }
        // More output than the ring keeps: the reconnect must take path B.
        const std::vector<std::byte> chunk(200, std::byte{0xBB});
        for (int i = 0; i < 6; ++i) {
            std::ignore = R.Agent.Write(ByteSpan{chunk});
        }
        Ac.emplace(MutableByteSpan{R.Sp.AgentFrames}, START,
                   PROTOCOL_VERSION_GRID);
        Cc.emplace(R.Client, MakeNonce(++R.Nonces),
                   MutableByteSpan{R.Sp.ClientFrames}, START, true,
                   PROTOCOL_VERSION_GRID, MutableByteSpan{R.Sp.SnapshotBuf});
        // Handshake until the client has sent Resume (it is Live then, and
        // the Resume is the frame it just wrote).
        for (int i = 0; i < 64 && Resume.empty(); ++i) {
            auto ea = PumpAgent(*Ac, R.Sp, START);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<NeedsSession>(*ea)) {
                Ac->Route(R.Agent.Ref(), MakeNonce(++R.Nonces));
            } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(*Cc, R.Sp, sf->Written));
            }
            auto ec = PumpClient(*Cc, R.Sp, START);
            REQUIRE(ec.has_value());
            if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                if (Cc->IsLive()) {
                    Resume = TakeFrame(R.Sp, sf->Written);
                } else {
                    REQUIRE(Feed(*Ac, R.Sp, sf->Written));
                }
            }
        }
        REQUIRE_FALSE(Resume.empty());
    }

    /// The client's post-Resume Resize.
    std::vector<std::byte> TakeResize(std::uint16_t rows, std::uint16_t cols) {
        Cc->QueueResize(ResizeMessage{.Rows = rows, .Cols = cols});
        return TakeClientFrame(START);
    }

    /// A frame that delivers nothing to the agent: the client's keepalive
    /// Ping, due at `now` (needs a keepalive interval in the limits).  A bare
    /// Ack is the same kind of frame, but a client cannot produce one here:
    /// it owes none until agent data arrives, which path B withholds, and a
    /// queued Resize outranks it.
    std::vector<std::byte> TakePing(Timestamp now) {
        return TakeClientFrame(now);
    }

    std::vector<std::byte> TakeClientFrame(Timestamp now) {
        auto ec = PumpClient(*Cc, R.Sp, now);
        REQUIRE(ec.has_value());
        const auto* sf = std::get_if<SendFrame>(&*ec);
        REQUIRE(sf != nullptr);
        return TakeFrame(R.Sp, sf->Written);
    }

    /// Hands the agent the bytes of `frames` in one read.
    void FeedAgent(std::initializer_list<std::vector<std::byte>> frames) {
        std::vector<std::byte> all;
        for (const auto& frame : frames) {
            all.insert(all.end(), frame.begin(), frame.end());
        }
        REQUIRE(Ac->OnBytes(ByteSpan{all}).has_value());
    }

    ConnectionResult PumpAgentAt(Timestamp now) {
        return PumpAgent(*Ac, R.Sp, now);
    }

    Rig R;
    std::optional<AgentConnection> Ac;
    std::optional<ClientConnection> Cc;
    std::vector<std::byte> Resume;
};

/// Keepalive often enough to make many frames, silence limit far enough off.
SessionLimits PingingLimits() {
    auto limits = TinyLimits();
    limits.Liveness =
        LivenessLimits{.KeepaliveInterval = std::chrono::milliseconds{100},
                       .PeerSilenceTimeout = std::chrono::seconds{30}};
    return limits;
}

void CheckResizedThenNeedsSnapshot(HeldReconnect& h) {
    auto first = h.PumpAgentAt(START);
    REQUIRE(first.has_value());
    const auto* resized = std::get_if<Resized>(&*first);
    REQUIRE_MESSAGE(resized != nullptr,
                    "the Resize stayed buffered behind the Resume");
    CHECK(resized->Size.Rows == 50);
    CHECK(resized->Size.Cols == 200);

    // Pongs owed for the Pings go out first; the request follows.
    const NeedsSnapshot* needs = nullptr;
    ConnectionResult second = Idle{};
    for (int i = 0; i < 64 && needs == nullptr; ++i) {
        second = h.PumpAgentAt(START);
        REQUIRE(second.has_value());
        needs = std::get_if<NeedsSnapshot>(&*second);
        if (needs == nullptr) {
            REQUIRE(std::holds_alternative<SendFrame>(*second));
        }
    }
    REQUIRE(needs != nullptr);
    CHECK(needs->Rows == 50);
    CHECK(needs->Cols == 200);
}

}  // namespace

// Kills: PumpResume returning Idle after the path-B stage change (the Resize
// stays buffered; the first Pump after the read is Idle, not Resized).
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: a Resize read together with Resume is processed in "
                  "the next Pump, without another wake") {
    HeldReconnect h{true};
    h.FeedAgent({h.Resume, h.TakeResize(50, 200)});
    // No further bytes, no further wake: the very next Pump has to take the
    // Resize off the buffer, and the snapshot request carries the new size.
    CheckResizedThenNeedsSnapshot(h);
}

// Kills: PumpAwaitSnapshot taking one frame per call (a frame that delivers
// nothing hides the Resize behind it).
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: a frame that delivers nothing between Resume and "
                  "Resize does not hide the Resize") {
    HeldReconnect h{true, PingingLimits()};
    const auto ping = h.TakePing(START + std::chrono::milliseconds{100});
    h.FeedAgent({h.Resume, ping, h.TakeResize(50, 200)});
    CheckResizedThenNeedsSnapshot(h);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: many frames that deliver nothing between Resume and "
                  "Resize do not hide the Resize") {
    HeldReconnect h{true, PingingLimits()};
    std::vector<std::vector<std::byte>> frames{h.Resume};
    for (int i = 1; i <= 20; ++i) {
        frames.push_back(
            h.TakePing(START + std::chrono::milliseconds{100} * i));
    }
    frames.push_back(h.TakeResize(50, 200));
    std::vector<std::byte> all;
    for (const auto& frame : frames) {
        all.insert(all.end(), frame.begin(), frame.end());
    }
    REQUIRE(h.Ac->OnBytes(ByteSpan{all}).has_value());
    CheckResizedThenNeedsSnapshot(h);
}

// A client that never answers the Resume with a Resize: the Resize wait has a
// deadline, and a caller that sleeps until NextDeadline gets the snapshot
// request at the last known size.
//
// Kills: ResizeWaitDeadline_ missing from AgentConnection::NextDeadline.
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: with a known size and no Resize, NextDeadline "
                  "brings NeedsSnapshot at the old size") {
    HeldReconnect h{true};
    h.FeedAgent({h.Resume});
    auto idle = h.PumpAgentAt(START);
    REQUIRE(idle.has_value());
    CHECK(std::holds_alternative<Idle>(*idle));

    const Deadline next = h.Ac->NextDeadline();
    REQUIRE(next.IsSet());
    CHECK(next.At() <= START + RESIZE_WAIT_TIMEOUT);

    auto due = h.PumpAgentAt(next.At());
    REQUIRE(due.has_value());
    const auto* needs = std::get_if<NeedsSnapshot>(&*due);
    REQUIRE(needs != nullptr);
    CHECK(needs->Rows == 24);
    CHECK(needs->Cols == 80);
}

// With no size ever known the agent still waits for the Resize (design.md §9,
// step 1): a Resize inside the window gives the snapshot at its size.
//
// Kills: WaitForResize_ set only when a size is known.
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: with no known size the agent waits for the Resize "
                  "and asks for its size") {
    HeldReconnect h{false};
    h.FeedAgent({h.Resume});
    auto idle = h.PumpAgentAt(START);
    REQUIRE(idle.has_value());
    CHECK(std::holds_alternative<Idle>(*idle));
    CHECK(h.Ac->NextDeadline().IsSet());

    h.FeedAgent({h.TakeResize(50, 200)});
    CheckResizedThenNeedsSnapshot(h);
}

// ...and with no Resize and no size, the deadline brings NeedsSnapshot{0, 0},
// meaning the grid's current size.  Nothing else is on the wire.
//
// Kills: no deadline while the Resize wait runs with no size known.
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: with no known size and no Resize, NextDeadline "
                  "brings NeedsSnapshot 0x0") {
    HeldReconnect h{false};
    h.FeedAgent({h.Resume});
    auto idle = h.PumpAgentAt(START);
    REQUIRE(idle.has_value());
    CHECK(std::holds_alternative<Idle>(*idle));

    auto early = h.PumpAgentAt(START + RESIZE_WAIT_TIMEOUT / 2);
    REQUIRE(early.has_value());
    CHECK(std::holds_alternative<Idle>(*early));

    const Deadline next = h.Ac->NextDeadline();
    REQUIRE(next.IsSet());
    CHECK(next.At() <= START + RESIZE_WAIT_TIMEOUT);

    auto due = h.PumpAgentAt(next.At());
    REQUIRE(due.has_value());
    const auto* needs = std::get_if<NeedsSnapshot>(&*due);
    REQUIRE(needs != nullptr);
    CHECK(needs->Rows == 0);
    CHECK(needs->Cols == 0);
}

// Path A takes the Resize that came with the Resume straight out of
// PumpResume.  The size has to reach the session there too: the next path-B
// reconnect asks for a snapshot at that size when its client sends no Resize.
//
// Kills: PumpResume's path A returning Resized without ReportResize (the
// snapshot request would carry 0x0).
TEST_CASE_FIXTURE(SodiumFixture,
                  "path A: a Resize read together with Resume is remembered "
                  "for the next reconnect") {
    Rig r;
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
        cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
        std::vector<std::byte> resume;
        for (int i = 0; i < 64 && resume.empty(); ++i) {
            auto ea = PumpAgent(ac, r.Sp, START);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<NeedsSession>(*ea)) {
                ac.Route(r.Agent.Ref(), MakeNonce(++r.Nonces));
            } else if (const auto* sf = std::get_if<SendFrame>(&*ea)) {
                REQUIRE(Feed(cc, r.Sp, sf->Written));
            }
            auto ec = PumpClient(cc, r.Sp, START);
            REQUIRE(ec.has_value());
            if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                if (cc.IsLive()) {
                    resume = TakeFrame(r.Sp, sf->Written);
                } else {
                    REQUIRE(Feed(ac, r.Sp, sf->Written));
                }
            }
        }
        REQUIRE_FALSE(resume.empty());
        auto ec = PumpClient(cc, r.Sp, START);
        REQUIRE(ec.has_value());
        const auto* sf = std::get_if<SendFrame>(&*ec);
        REQUIRE(sf != nullptr);
        std::vector<std::byte> both = resume;
        const auto resize = TakeFrame(r.Sp, sf->Written);
        both.insert(both.end(), resize.begin(), resize.end());
        REQUIRE(ac.OnBytes(ByteSpan{both}).has_value());
        auto first = PumpAgent(ac, r.Sp, START);
        REQUIRE(first.has_value());
        CHECK(std::holds_alternative<Resized>(*first));
        ac.Close(START);
    }
    // Path B next time, from a client that sends no Resize: with the size
    // known the agent waits for one, so a deadline is set.
    const std::vector<std::byte> chunk(200, std::byte{0xBB});
    for (int i = 0; i < 6; ++i) {
        std::ignore = r.Agent.Write(ByteSpan{chunk});
    }
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
    auto idle = PumpAgent(ac, r.Sp, START);
    REQUIRE(idle.has_value());
    CHECK(std::holds_alternative<Idle>(*idle));
    const Deadline next = ac.NextDeadline();
    REQUIRE(next.IsSet());
    CHECK(next.At() > START);  // waiting for the Resize, not due now

    // No Resize comes: the request carries the size the Resize that came with
    // the Resume reported, not 0x0.
    auto due = PumpAgent(ac, r.Sp, next.At());
    REQUIRE(due.has_value());
    const auto* needs = std::get_if<NeedsSnapshot>(&*due);
    REQUIRE(needs != nullptr);
    CHECK(needs->Rows == 24);
    CHECK(needs->Cols == 80);
}
