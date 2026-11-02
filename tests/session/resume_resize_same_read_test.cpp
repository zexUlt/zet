/// Path B when the client's Resize arrives in the same read as its Resume.
///
/// AgentConnection used to return Idle right after the Resume moved it to
/// AwaitSnapshot.  A caller that pumps only when the socket is readable then
/// has nothing to wake it: the Resize sits in the frame buffer until another
/// byte arrives or RESIZE_WAIT_TIMEOUT runs out.

#include "grid_path_helpers.hpp"

#include <algorithm>
#include <cstring>

namespace {
using namespace zet::wire;

/// Copies the bytes of the frame a Pump just wrote into `out`.
std::vector<std::byte> TakeFrame(const Space& sp, std::size_t written) {
    const auto first = sp.Out.begin();
    return {first, first + static_cast<std::ptrdiff_t>(written)};
}
}  // namespace

// Kills: PumpResume returning Idle after the path-B stage change (the Resize
// stays buffered; the first Pump after the read is Idle, not Resized).
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: a Resize read together with Resume is processed in "
                  "the next Pump, without another wake") {
    Rig r;
    {
        // First connection: the agent learns the terminal is 24x80, so that
        // the reconnect waits for a Resize before it asks for a snapshot.
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
        REQUIRE(DriveToLive(ac, cc, r.Agent, r.Sp, START, r.Nonces));
        for (int i = 0; i < 20; ++i) {
            auto ec = PumpClient(cc, r.Sp, START);
            REQUIRE(ec.has_value());
            if (const auto* sf = std::get_if<SendFrame>(&*ec)) {
                REQUIRE(Feed(ac, r.Sp, sf->Written));
            }
            REQUIRE(PumpAgent(ac, r.Sp, START).has_value());
        }
        ac.Close(START);
    }
    // More output than the ring keeps: the reconnect must take path B.
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
    cc.QueueResize(ResizeMessage{.Rows = 50, .Cols = 200});

    // Handshake until the client has sent Resume (it is Live then, and the
    // Resume is the frame it just wrote).
    std::vector<std::byte> resume;
    for (int i = 0; i < 64 && resume.empty(); ++i) {
        auto ea = PumpAgent(ac, r.Sp, START);
        REQUIRE(ea.has_value());
        if (const auto* ns = std::get_if<NeedsSession>(&*ea)) {
            ac.Route(r.Agent.Ref(), MakeNonce(++r.Nonces));
            std::ignore = ns;
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

    // The client's next frame is the post-Resume Resize.
    auto ec = PumpClient(cc, r.Sp, START);
    REQUIRE(ec.has_value());
    const auto* resizeFrame = std::get_if<SendFrame>(&*ec);
    REQUIRE(resizeFrame != nullptr);
    const auto resize = TakeFrame(r.Sp, resizeFrame->Written);

    // One read delivers both.
    std::vector<std::byte> both = resume;
    both.insert(both.end(), resize.begin(), resize.end());
    REQUIRE(ac.OnBytes(ByteSpan{both}).has_value());

    // No further bytes, no further wake: the very next Pump has to take the
    // Resize off the buffer.
    auto first = PumpAgent(ac, r.Sp, START);
    REQUIRE(first.has_value());
    const auto* resized = std::get_if<Resized>(&*first);
    REQUIRE_MESSAGE(resized != nullptr,
                    "the Resize stayed buffered behind the Resume");
    CHECK(resized->Size.Rows == 50);
    CHECK(resized->Size.Cols == 200);

    // And the snapshot request carries the new size.
    auto second = PumpAgent(ac, r.Sp, START);
    REQUIRE(second.has_value());
    const auto* needs = std::get_if<NeedsSnapshot>(&*second);
    REQUIRE(needs != nullptr);
    CHECK(needs->Rows == 50);
    CHECK(needs->Cols == 200);
}
