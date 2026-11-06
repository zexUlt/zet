/// The agent acknowledges the client's input once the pty has taken it, not
/// when the socket delivered it (design.md §9).
///
/// An AgentSession and a ClientSession joined by real connections, with a
/// byte vector standing in for the pty's queue: what a Deliver event hands
/// over is appended to it, and the test calls AgentSession::InputDelivered
/// for the part the "pty" takes.  The client is told only what was taken, so
/// what it still holds is what its ring has not released: observed through
/// how much its Write accepts.
///
///   - the ack offset trails reception and equals the delivered bytes, in
///     the idle Ack and in the Ack a Data frame carries;
///   - the bytes the agent holds never pass the client's window, however
///     little the pty takes;
///   - bytes received but not yet written, then a reconnect: the client
///     resends from the delivered offset and every byte reaches the pty once.

#include "grid_path_helpers.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {

constexpr std::uint16_t VERSION = wire::PROTOCOL_VERSION_MIN;

std::vector<std::byte> Sequence(std::size_t count, std::size_t from = 0) {
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::byte>((from + i) * 131 % 251);
    }
    return bytes;
}

/// Agent and client connection, live, over `r`'s sessions.
struct Link {
    explicit Link(Rig& rig)
        : R(rig),
          Agent{MutableByteSpan{rig.Sp.AgentFrames}, START, VERSION},
          Client{rig.Client,
                 MakeNonce(++rig.Nonces),
                 MutableByteSpan{rig.Sp.ClientFrames},
                 START,
                 /*capable=*/false,
                 VERSION,
                 MutableByteSpan{rig.Sp.SnapshotBuf}} {
        REQUIRE(
            DriveToLive(Agent, Client, rig.Agent, rig.Sp, START, rig.Nonces));
    }

    /// Moves frames both ways until neither end has anything to do at `now`.
    /// What the agent delivers lands in `ptyQueue`.
    void Exchange(Timestamp now, std::vector<std::byte>& ptyQueue) {
        for (int i = 0; i < 100000; ++i) {
            bool moved = false;
            auto ea = PumpAgent(Agent, R.Sp, now);
            REQUIRE(ea.has_value());
            if (std::holds_alternative<SendFrame>(*ea)) {
                REQUIRE(Feed(Client, R.Sp, std::get<SendFrame>(*ea).Written));
                moved = true;
            } else if (const auto* d = std::get_if<Deliver>(&*ea)) {
                ptyQueue.insert(ptyQueue.end(), d->Bytes.begin(),
                                d->Bytes.end());
                moved = true;
            } else if (!std::holds_alternative<Idle>(*ea)) {
                moved = true;
            }
            auto ec = PumpClient(Client, R.Sp, now);
            REQUIRE(ec.has_value());
            if (std::holds_alternative<SendFrame>(*ec)) {
                REQUIRE(Feed(Agent, R.Sp, std::get<SendFrame>(*ec).Written));
                moved = true;
            } else if (!std::holds_alternative<Idle>(*ec)) {
                moved = true;
            }
            if (!moved) {
                return;
            }
        }
        FAIL("the exchange does not settle");
    }

    /// Exchange across the idle-Ack delay: the first turn arms the timer,
    /// the second is past it.
    void ExchangePastIdleAck(Timestamp& now, std::vector<std::byte>& queue) {
        Exchange(now, queue);
        now = now + ACK_IDLE_DELAY + std::chrono::milliseconds{1};
        Exchange(now, queue);
    }

    Rig& R;
    AgentConnection Agent;
    ClientConnection Client;
};

/// How much more the client's ring takes: what it has released.
std::size_t ClientRoom(Rig& r) {
    const std::vector<std::byte> probe(r.Limits.ResendMax, std::byte{0});
    // Write is destructive, so only the count matters to the callers, which
    // account for the bytes they put in.
    const auto taken = r.Client.Write(probe);
    REQUIRE(taken.has_value());
    return *taken;
}

}  // namespace

TEST_CASE_FIXTURE(SodiumFixture,
                  "the ack offset trails reception and equals the bytes the "
                  "pty took, in the idle Ack") {
    Rig r;
    const auto input = Sequence(r.Limits.ResendMax);
    REQUIRE(r.Client.Write(input) == input.size());
    Link link{r};
    std::vector<std::byte> pty;
    Timestamp now = START;

    link.Exchange(now, pty);
    REQUIRE(pty == input);

    // Everything is in the agent's queue and none of it is written: nothing
    // is acknowledged, however long it sits.
    for (int i = 0; i < 3; ++i) {
        link.ExchangePastIdleAck(now, pty);
    }
    CHECK(r.Agent.Ref().ReceiveOffset() == 0);
    CHECK(ClientRoom(r) == 0);

    // The pty takes 100 bytes: the idle Ack says 100, not 512.
    r.Agent.InputDelivered(100);
    link.ExchangePastIdleAck(now, pty);
    CHECK(r.Agent.Ref().ReceiveOffset() == 100);
    CHECK(ClientRoom(r) == 100);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the ack offset equals the bytes the pty took, in the Ack a "
                  "Data frame carries") {
    Rig r;
    const auto input = Sequence(r.Limits.ResendMax);
    REQUIRE(r.Client.Write(input) == input.size());
    Link link{r};
    std::vector<std::byte> pty;
    Timestamp now = START;
    link.Exchange(now, pty);
    REQUIRE(pty.size() == input.size());

    r.Agent.InputDelivered(40);
    // The shell prints something: the Data frame carries the ack, with no
    // idle delay in between.
    const auto output = Sequence(10, 7);
    REQUIRE(r.Agent.Write(output).has_value());
    link.Exchange(now, pty);
    CHECK(ClientRoom(r) == 40);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "the agent holds no more than the client's window while "
                  "the pty takes next to nothing") {
    Rig r;
    const auto source = Sequence(20 * r.Limits.ResendMax);
    std::mt19937 rng{0x5EED};
    Link link{r};
    std::vector<std::byte> pty;      // what the agent holds, not yet taken
    std::vector<std::byte> written;  // what the pty took
    std::size_t fed = 0;
    Timestamp now = START;

    // The client types as much as it is allowed; the window stops it.
    auto type = [&] {
        const auto taken = r.Client.Write(ByteSpan{source}.subspan(
            fed, std::min<std::size_t>(source.size() - fed, 2000)));
        REQUIRE(taken.has_value());
        fed += *taken;
    };
    type();
    CHECK(fed == r.Limits.ResendMax);  // stopped at the window
    for (int i = 0; i < 3; ++i) {
        link.ExchangePastIdleAck(now, pty);
    }
    CHECK(pty.size() == r.Limits.ResendMax);
    type();
    CHECK(fed == r.Limits.ResendMax);  // still stopped: nothing was taken

    std::size_t largest = pty.size();
    for (int turn = 0; turn < 5000 && written.size() < source.size(); ++turn) {
        // The pty takes a few bytes now and then.
        if (turn % 3 == 0 && !pty.empty()) {
            const std::size_t take = std::min<std::size_t>(
                pty.size(),
                std::uniform_int_distribution<std::size_t>{1, 40}(rng));
            const auto end = pty.begin() + static_cast<std::ptrdiff_t>(take);
            written.insert(written.end(), pty.begin(), end);
            pty.erase(pty.begin(), end);
            r.Agent.InputDelivered(take);
        }
        type();
        link.ExchangePastIdleAck(now, pty);
        largest = std::max(largest, pty.size());
        REQUIRE(pty.size() <= r.Limits.ResendMax);
    }
    CHECK(largest <= r.Limits.ResendMax);
    written.insert(written.end(), pty.begin(), pty.end());
    CHECK(written.size() == source.size());
    CHECK(written == source);
}

TEST_CASE_FIXTURE(SodiumFixture,
                  "input received but not yet written reaches the pty "
                  "exactly once across a reconnect") {
    Rig r;
    const auto first = Sequence(300);
    REQUIRE(r.Client.Write(first) == first.size());
    std::vector<std::byte> pty;  // the pty's queue: delivered by the agent
    std::vector<std::byte> written;
    Timestamp now = START;
    {
        Link link{r};
        link.Exchange(now, pty);
        REQUIRE(pty == first);
        // The pty took 100 of the 300; the agent acknowledges 100.
        written.assign(pty.begin(), pty.begin() + 100);
        pty.erase(pty.begin(), pty.begin() + 100);
        r.Agent.InputDelivered(100);
        link.ExchangePastIdleAck(now, pty);
        link.Agent.Close(now);
    }
    // What Resume tells the client: resend from what was written.
    CHECK(r.Agent.Ref().ReceiveOffset() == 100);

    // More is typed while detached.
    const auto second = Sequence(50, 300);
    REQUIRE(r.Client.Write(second) == second.size());
    {
        Link link{r};
        // The 200 bytes the agent already holds come again from 100 and are
        // cut as duplicates; only the 50 new ones are delivered.
        const std::size_t before = pty.size();
        link.Exchange(now, pty);
        CHECK(pty.size() == before + second.size());
        written.insert(written.end(), pty.begin(), pty.end());
        r.Agent.InputDelivered(pty.size());
        pty.clear();
        link.ExchangePastIdleAck(now, pty);
        CHECK(pty.empty());
    }

    std::vector<std::byte> expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    CHECK(written == expected);
    CHECK(r.Agent.Ref().ReceiveOffset() == expected.size());
}
