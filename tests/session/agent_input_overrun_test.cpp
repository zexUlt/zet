/// The agent's queue of input for the pty cannot lose what the client
/// legitimately sent (design.md §9).
///
/// After an outage the client replays everything it buffered while detached,
/// up to its DisconnectMax, and the agent's queue (PtyInput_ in agent.cpp)
/// takes it all-or-nothing per frame. The agent sizes it for
/// max(ResendMax, DisconnectMax), so a peer with the same limits never
/// overfills it; a peer with bigger ones can. A refused frame is rolled back
/// out of the receive cursor (AgentSession::InputRejected) before the
/// connection closes, so the client's resend brings it again:
///
///   - a replay larger than the queue, with the pty taking input only between
///     connections: every byte reaches the pty once, in order, and the
///     acknowledged offset ends at the input's length;
///   - the same replay with the limits the agent sizes its queue for: no
///     refusal at all;
///   - random typing, partial pty writes and reconnects: exactly once, the
///     ack never goes down and equals what the pty took;
///   - input discarded because the shell is gone is acknowledged.
///
/// An AgentSession and a ClientSession with separate limits, joined by real
/// connections; a byte vector with a capacity stands in for PtyInput_.

#include "grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {
constexpr std::uint16_t VERSION = wire::PROTOCOL_VERSION_MIN;

std::vector<std::byte> Seq(std::size_t count, std::size_t from = 0) {
    std::vector<std::byte> b(count);
    for (std::size_t i = 0; i < count; ++i)
        b[i] = static_cast<std::byte>((from + i) * 131 % 251);
    return b;
}

/// Rig with separate limits for the two ends.
struct Rig2 {
    explicit Rig2(SessionLimits agentLim, SessionLimits clientLim)
        : AL(agentLim),
          CL(clientLim),
          As(std::max(AL.ResendMax, AL.DisconnectMax) + 256),
          Cs(std::max(CL.ResendMax, CL.DisconnectMax) + 256),
          Agent{
              DeriveSessionSecrets(MakeKey(33)),
              SessionStreams{
                  .Storage = MutableByteSpan{As}, .Now = START, .Limits = AL}},
          Client{DeriveSessionSecrets(MakeKey(33)),
                 SessionStreams{.Storage = MutableByteSpan{Cs},
                                .Now = START,
                                .Limits = CL}} {}
    SessionLimits AL, CL;
    std::vector<std::byte> As, Cs;
    AgentSession Agent;
    ClientSession Client;
    Space Sp;
    std::uint64_t Nonces = 0;
};

/// Models agent.cpp: Deliver -> PtyInput_.Append (all-or-nothing, capacity
/// `limit`); a refusal is handed back with InputRejected and the connection
/// closes.
struct Link2 {
    Link2(Rig2& r, std::size_t limit)
        : R(r),
          Limit(limit),
          Agent{MutableByteSpan{r.Sp.AgentFrames}, START, VERSION},
          Client{r.Client,
                 MakeNonce(++r.Nonces),
                 MutableByteSpan{r.Sp.ClientFrames},
                 START,
                 false,
                 VERSION,
                 MutableByteSpan{r.Sp.SnapshotBuf}} {
        REQUIRE(DriveToLive(Agent, Client, r.Agent, r.Sp, START, r.Nonces));
    }
    // returns false if the "agent" had to close (queue full)
    bool Exchange(Timestamp now, std::vector<std::byte>& q) {
        if (Closed) return false;  // the agent drops a connection it closed
        for (int i = 0; i < 100000; ++i) {
            bool moved = false;
            auto ea = PumpAgent(Agent, R.Sp, now);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<SendFrame>(*ea)) {
                REQUIRE(Feed(Client, R.Sp, std::get<SendFrame>(*ea).Written));
                moved = true;
            } else if (const auto* d = std::get_if<Deliver>(&*ea)) {
                if (q.size() + d->Bytes.size() > Limit) {
                    R.Agent.InputRejected(d->Bytes.size());
                    Closed = true;
                    return false;
                }
                q.insert(q.end(), d->Bytes.begin(), d->Bytes.end());
                moved = true;
            } else if (!std::holds_alternative<Idle>(*ea))
                moved = true;
            auto ec = PumpClient(Client, R.Sp, now);
            REQUIRE(ec.has_value());
            if (std::holds_alternative<SendFrame>(*ec)) {
                REQUIRE(Feed(Agent, R.Sp, std::get<SendFrame>(*ec).Written));
                moved = true;
            } else if (!std::holds_alternative<Idle>(*ec))
                moved = true;
            if (!moved) return true;
        }
        FAIL("no settle");
        return true;
    }
    void Idle2(Timestamp& now, std::vector<std::byte>& q) {
        Exchange(now, q);
        now = now + ACK_IDLE_DELAY + std::chrono::milliseconds{1};
        Exchange(now, q);
    }
    Rig2& R;
    std::size_t Limit;
    bool Closed = false;
    AgentConnection Agent;
    ClientConnection Client;
};

SessionLimits Lim(std::size_t resend, std::size_t disc) {
    auto l = TinyLimits();
    l.ResendMax = resend;
    l.DisconnectMax = disc;
    return l;
}
}  // namespace

// The client buffered more while detached (DisconnectMax 1500000) than the
// agent's queue holds (300000, the agent's own max(ResendMax, DisconnectMax)).
// The queue refuses a frame and the connection closes; after each reconnect
// the pty takes what is queued and the next frames fit, until every byte has
// reached the pty exactly once. The queue holds more than one frame (256 KiB):
// a smaller one could never take any.
TEST_CASE_FIXTURE(SodiumFixture,
                  "a replay larger than the agent's queue loses and repeats "
                  "no input") {
    Rig2 r{Lim(300000, 300000), Lim(300000, 1500000)};
    const auto src = Seq(1200000);
    REQUIRE(r.Client.Write(src) == src.size());  // detached: DisconnectMax
    std::vector<std::byte> q, written;
    Timestamp now = START;
    {
        Link2 l{r, 300000};
        const bool ok = l.Exchange(now, q);
        CHECK_FALSE(ok);  // overrun happened
        l.Agent.Close(now);
    }
    // the pty now takes everything
    for (int round = 0; round < 40; ++round) {
        written.insert(written.end(), q.begin(), q.end());
        r.Agent.InputDelivered(q.size());
        q.clear();
        Link2 l{r, 300000};
        // What is acknowledged is what the pty took, never more.
        REQUIRE(r.Agent.Ref().ReceiveOffset() == written.size());
        l.Idle2(now, q);
        l.Agent.Close(now);
    }
    written.insert(written.end(), q.begin(), q.end());
    r.Agent.InputDelivered(q.size());
    CHECK(std::equal(written.begin(), written.end(), src.begin()));
    CHECK(written == src);
    CHECK(r.Agent.Ref().ReceiveOffset() == src.size());
}

// The same replay with matching limits: the agent's queue is as large as the
// client's DisconnectMax, so nothing is refused and the pty sees one pass.
TEST_CASE_FIXTURE(SodiumFixture,
                  "a replay up to DisconnectMax fits the agent's queue") {
    Rig2 r{Lim(100000, 700000), Lim(100000, 700000)};
    const auto src = Seq(700000);
    REQUIRE(r.Client.Write(src) == src.size());  // detached: DisconnectMax
    std::vector<std::byte> q;
    Timestamp now = START;
    Link2 l{r, 700000};
    CHECK(l.Exchange(now, q));
    CHECK(q == src);
    r.Agent.InputDelivered(q.size());
    l.Idle2(now, q);
    CHECK(r.Agent.Ref().ReceiveOffset() == src.size());
}

// Random typing, partial pty writes and reconnects, with limits that match:
// the queue is sized as the agent sizes it and never refuses.
TEST_CASE_FIXTURE(SodiumFixture,
                  "random reconnects deliver input exactly once and the ack "
                  "only grows") {
    for (unsigned seed = 1; seed <= 40; ++seed) {
        Rig2 r{Lim(512, 2048), Lim(512, 2048)};
        std::mt19937 rng{seed};
        const auto src = Seq(6000);
        std::size_t fed = 0;
        std::vector<std::byte> q, written;
        Timestamp now = START;
        std::uint64_t lastAck = 0;
        auto type = [&] {
            auto n = std::min<std::size_t>(src.size() - fed, rng() % 300 + 1);
            auto t = r.Client.Write(ByteSpan{src}.subspan(fed, n));
            REQUIRE(t.has_value());
            fed += *t;
        };
        for (int conn = 0;
             conn < 60 && written.size() + q.size() < src.size() + 0; ++conn) {
            type();
            Link2 l{r, 2048};
            for (int turn = 0; turn < 12; ++turn) {
                type();
                REQUIRE(l.Exchange(now, q));
                if (!q.empty() && rng() % 2) {
                    auto take =
                        std::min<std::size_t>(q.size(), rng() % 200 + 1);
                    written.insert(
                        written.end(), q.begin(),
                        q.begin() + static_cast<std::ptrdiff_t>(take));
                    q.erase(q.begin(),
                            q.begin() + static_cast<std::ptrdiff_t>(take));
                    r.Agent.InputDelivered(take);
                }
                if (rng() % 3 == 0) l.Idle2(now, q);
                const auto a = r.Agent.Ref().ReceiveOffset();
                REQUIRE(a >= lastAck);
                lastAck = a;
                REQUIRE(a == written.size());
            }
            l.Agent.Close(now);
        }
        // drain
        for (int i = 0; i < 50 && written.size() < src.size(); ++i) {
            written.insert(written.end(), q.begin(), q.end());
            r.Agent.InputDelivered(q.size());
            q.clear();
            type();
            Link2 l{r, 2048};
            l.Exchange(now, q);
            l.Idle2(now, q);
            l.Agent.Close(now);
        }
        written.insert(written.end(), q.begin(), q.end());
        CHECK(written.size() == fed);
        CHECK(std::equal(written.begin(), written.end(), src.begin()));
    }
}

// The shell is gone: the agent discards what is queued and acknowledges it,
// and a reconnect finds nothing to resend.
TEST_CASE_FIXTURE(SodiumFixture,
                  "input discarded for a shell that has gone stays "
                  "acknowledged across a reconnect") {
    Rig2 r{Lim(512, 512), Lim(512, 512)};
    const auto src = Seq(500);
    REQUIRE(r.Client.Write(src) == 500);
    std::vector<std::byte> q;
    Timestamp now = START;
    {
        Link2 l{r, 2048};
        REQUIRE(l.Exchange(now, q));
        r.Agent.InputDelivered(q.size());  // DiscardPtyInput
        q.clear();
        l.Idle2(now, q);
        l.Agent.Close(now);
    }
    CHECK(r.Agent.Ref().ReceiveOffset() == 500);
    {
        Link2 l{r, 2048};
        REQUIRE(l.Exchange(now, q));
        CHECK(q.empty());
        l.Agent.Close(now);
    }
}
