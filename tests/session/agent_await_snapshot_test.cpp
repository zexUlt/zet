/// AgentConnection after a Resume that arrives with a Resize in the same read,
/// and the deadlines it leaves behind once the Resize wait is over.
///
/// A deadline that has passed but is not cleared makes NextDeadline() answer
/// "now" for ever: a caller that sleeps until it wakes at once, round after
/// round, with nothing to do.

#include "grid_path_helpers.hpp"

#include <initializer_list>
#include <optional>

namespace {
using namespace zet::wire;

std::vector<std::byte> TakeFrame(const Space& sp, std::size_t written) {
    const auto first = sp.Out.begin();
    return {first, first + static_cast<std::ptrdiff_t>(written)};
}

/// A reconnect up to the point where the client has sent its Resume, which is
/// held back so the test can put the frames that follow into one read.  The
/// session has seen more output than the ring keeps: with `capable` the
/// reconnect takes path B, without it the session is reaped on overflow.
struct HeldReconnect {
    explicit HeldReconnect(bool capable) : R(TinyLimits()) {
        {
            AgentConnection ac{MutableByteSpan{R.Sp.AgentFrames}, START,
                               PROTOCOL_VERSION_GRID};
            ClientConnection cc{R.Client,
                                MakeNonce(++R.Nonces),
                                MutableByteSpan{R.Sp.ClientFrames},
                                START,
                                capable,
                                PROTOCOL_VERSION_GRID,
                                MutableByteSpan{R.Sp.SnapshotBuf}};
            cc.QueueResize(ResizeMessage{.Rows = 24, .Cols = 80});
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
        const std::vector<std::byte> chunk(200, std::byte{0xBB});
        for (int i = 0; i < 6; ++i) {
            std::ignore = R.Agent.Write(ByteSpan{chunk});
        }
        Ac.emplace(MutableByteSpan{R.Sp.AgentFrames}, START,
                   PROTOCOL_VERSION_GRID);
        Cc.emplace(R.Client, MakeNonce(++R.Nonces),
                   MutableByteSpan{R.Sp.ClientFrames}, START, capable,
                   PROTOCOL_VERSION_GRID, MutableByteSpan{R.Sp.SnapshotBuf});
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
        auto ec = PumpClient(*Cc, R.Sp, START);
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

    Rig R;
    std::optional<AgentConnection> Ac;
    std::optional<ClientConnection> Cc;
    std::vector<std::byte> Resume;
};

}  // namespace

// A v2 client without GRID_CAPABLE resumes a session that overflowed its ring
// and sends its Resize in the same read.  The Resume ends the connection with
// SessionOverflowed and moves the keys out; the agent loop pumps again, and
// that Pump must find the connection failed, not open the Resize with keys
// that are gone.
//
// Kills: PumpResume's SessionOverflowed branch not failing the connection
// (Fail removed).
TEST_CASE_FIXTURE(SodiumFixture,
                  "overflow: a Resize read together with the Resume of a "
                  "reaped session fails the next Pump") {
    HeldReconnect h{false};
    h.FeedAgent({h.Resume, h.TakeResize(50, 200)});
    auto first = PumpAgent(*h.Ac, h.R.Sp, START);
    REQUIRE(first.has_value());
    CHECK(std::holds_alternative<SessionOverflowed>(*first));
    auto second = PumpAgent(*h.Ac, h.R.Sp, START);
    CHECK_FALSE(second.has_value());
}

// The Resize that ends the wait must end its deadline too: the caller sleeps
// until NextDeadline, and one that is already in the past wakes it at once for
// ever.
//
// Kills: ResizeWaitDeadline_ not reset when the Resize arrives in
// PumpAwaitSnapshot.
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: the Resize wait leaves no deadline behind once the "
                  "Resize has come") {
    HeldReconnect h{true};
    h.FeedAgent({h.Resume});
    auto idle = PumpAgent(*h.Ac, h.R.Sp, START);
    REQUIRE(idle.has_value());
    REQUIRE(std::holds_alternative<Idle>(*idle));
    REQUIRE(h.Ac->NextDeadline().IsSet());  // the wait is on

    h.FeedAgent({h.TakeResize(50, 200)});
    const auto due = START + RESIZE_WAIT_TIMEOUT;
    auto resized = PumpAgent(*h.Ac, h.R.Sp, START);
    REQUIRE(resized.has_value());
    REQUIRE(std::holds_alternative<Resized>(*resized));
    auto needs = PumpAgent(*h.Ac, h.R.Sp, START);
    REQUIRE(needs.has_value());
    REQUIRE(std::holds_alternative<NeedsSnapshot>(*needs));

    const Deadline next = h.Ac->NextDeadline();
    CHECK((!next.IsSet() || next.At() > due));
}

// Same when the client never answers and the wait runs out.
//
// Kills: ResizeWaitDeadline_ not reset when the wait times out in
// PumpAwaitSnapshot.
TEST_CASE_FIXTURE(SodiumFixture,
                  "path B: the Resize wait leaves no deadline behind once it "
                  "has timed out") {
    HeldReconnect h{true};
    h.FeedAgent({h.Resume});
    const auto due = START + RESIZE_WAIT_TIMEOUT;
    REQUIRE(std::holds_alternative<Idle>(*PumpAgent(*h.Ac, h.R.Sp, START)));
    auto needs = PumpAgent(*h.Ac, h.R.Sp, due);
    REQUIRE(needs.has_value());
    REQUIRE(std::holds_alternative<NeedsSnapshot>(*needs));
    for (int i = 0; i < 3; ++i) {
        auto again = PumpAgent(*h.Ac, h.R.Sp, due);
        REQUIRE(again.has_value());
        CHECK(std::holds_alternative<Idle>(*again));
    }
    const Deadline next = h.Ac->NextDeadline();
    CHECK((!next.IsSet() || next.At() > due));
}
