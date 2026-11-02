/// Path B when the client's Resize arrives in the same read as its Resume.
///
/// AgentConnection used to return Idle right after the Resume moved it to
/// AwaitSnapshot.  A caller that pumps only when the socket is readable then
/// has nothing to wake it: the Resize sits in the frame buffer until another
/// byte arrives or RESIZE_WAIT_TIMEOUT runs out.

#include "grid_path_helpers.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

namespace {
using namespace zet::wire;

/// Copies the bytes of the frame a Pump just wrote into `out`.
std::vector<std::byte> TakeFrame(const Space& sp, std::size_t written) {
    const auto first = sp.Out.begin();
    return {first, first + static_cast<std::ptrdiff_t>(written)};
}

/// A path-B reconnect up to the point where the client has sent its Resume
/// and its post-Resume Resize, held back as two byte strings.  `knownSize`
/// says whether the agent has been told a terminal size before.
struct HeldReconnect {
    explicit HeldReconnect(bool knownSize) {
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
        Cc->QueueResize(ResizeMessage{.Rows = 50, .Cols = 200});
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
        // The client's next frame is the post-Resume Resize.
        auto ec = PumpClient(*Cc, R.Sp, START);
        REQUIRE(ec.has_value());
        const auto* sf = std::get_if<SendFrame>(&*ec);
        REQUIRE(sf != nullptr);
        Resize = TakeFrame(R.Sp, sf->Written);
    }

    Rig R;
    std::optional<AgentConnection> Ac;
    std::optional<ClientConnection> Cc;
    std::vector<std::byte> Resume;
    std::vector<std::byte> Resize;
};

}  // namespace

// Kills: PumpResume returning Idle after the path-B stage change (the Resize
// stays buffered; the first Pump after the read is Idle, not Resized).
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: a Resize read together with Resume is processed in "
                  "the next Pump, without another wake") {
    HeldReconnect h{true};

    // One read delivers both.
    std::vector<std::byte> both = h.Resume;
    both.insert(both.end(), h.Resize.begin(), h.Resize.end());
    REQUIRE(h.Ac->OnBytes(ByteSpan{both}).has_value());

    // No further bytes, no further wake: the very next Pump has to take the
    // Resize off the buffer.
    auto first = PumpAgent(*h.Ac, h.R.Sp, START);
    REQUIRE(first.has_value());
    const auto* resized = std::get_if<Resized>(&*first);
    REQUIRE_MESSAGE(resized != nullptr,
                    "the Resize stayed buffered behind the Resume");
    CHECK(resized->Size.Rows == 50);
    CHECK(resized->Size.Cols == 200);

    // And the snapshot request carries the new size.
    auto second = PumpAgent(*h.Ac, h.R.Sp, START);
    REQUIRE(second.has_value());
    const auto* needs = std::get_if<NeedsSnapshot>(&*second);
    REQUIRE(needs != nullptr);
    CHECK(needs->Rows == 50);
    CHECK(needs->Cols == 200);
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
    REQUIRE(h.Ac->OnBytes(ByteSpan{h.Resume}).has_value());
    auto idle = PumpAgent(*h.Ac, h.R.Sp, START);
    REQUIRE(idle.has_value());
    CHECK(std::holds_alternative<Idle>(*idle));

    const Deadline next = h.Ac->NextDeadline();
    REQUIRE(next.IsSet());
    CHECK(next.At() <= START + RESIZE_WAIT_TIMEOUT);

    auto due = PumpAgent(*h.Ac, h.R.Sp, next.At());
    REQUIRE(due.has_value());
    const auto* needs = std::get_if<NeedsSnapshot>(&*due);
    REQUIRE(needs != nullptr);
    CHECK(needs->Rows == 24);
    CHECK(needs->Cols == 80);
}

// With no size ever known there is no Resize to wait for, and the request is
// owed as soon as the Resume is taken.  Nothing else will wake the caller, so
// NextDeadline has to say it is due.
//
// Kills: AgentConnection::NextDeadline not reporting an owed NeedsSnapshot.
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: with no known size NextDeadline is due while "
                  "NeedsSnapshot is owed") {
    HeldReconnect h{false};
    REQUIRE(h.Ac->OnBytes(ByteSpan{h.Resume}).has_value());
    auto idle = PumpAgent(*h.Ac, h.R.Sp, START);
    REQUIRE(idle.has_value());
    CHECK(std::holds_alternative<Idle>(*idle));

    const Deadline next = h.Ac->NextDeadline();
    REQUIRE(next.IsSet());
    CHECK(next.At() <= START);

    auto due = PumpAgent(*h.Ac, h.R.Sp, START);
    REQUIRE(due.has_value());
    CHECK(std::holds_alternative<NeedsSnapshot>(*due));
    // Asked for once: after that, nothing is owed.
    const Deadline after = h.Ac->NextDeadline();
    const bool dueNow = after.IsSet() && after.At() <= START;
    CHECK_FALSE(dueNow);
}

// Path A takes the Resize that came with the Resume straight out of
// PumpResume.  The size has to reach the session there too: the next path-B
// reconnect waits for a Resize only if a size is known.
//
// Kills: PumpResume's path A returning Resized without ReportResize.
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
}
