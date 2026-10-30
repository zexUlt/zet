#include "zet/session/stream_state.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;

/// Small enough to cross every ceiling with a few writes, ordered as the
/// defaults are: the live ceiling below the detached one below the storage.
constexpr std::size_t RESEND_MAX = 8;
constexpr std::size_t DISCONNECT_MAX = 16;
constexpr std::size_t STORAGE = 32;
constexpr Duration ORPHAN = 30s;

const Timestamp START = Timestamp::FromNanos(1'000'000'000);

std::vector<std::byte> Bytes(std::size_t count) {
    return std::vector<std::byte>(count, std::byte{0x2A});
}

struct Fixture {
    std::vector<std::byte> Storage = std::vector<std::byte>(STORAGE);

    SessionStreams Streams(Duration orphan = ORPHAN) {
        return SessionStreams{
            .Storage = MutableByteSpan{Storage},
            .Now = START,
            .Limits = SessionLimits{.ResendMax = RESEND_MAX,
                                    .DisconnectMax = DISCONNECT_MAX,
                                    .OrphanTimeout = orphan,
                                    .Liveness = {}}};
    }
};

}  // namespace

TEST_CASE_FIXTURE(Fixture,
                  "a session starts detached with the orphan clock running") {
    StreamState state{Streams(), EDetachedOverflow::Reap};
    CHECK(state.State() == ESessionState::Detached);
    REQUIRE(state.NextDeadline().IsSet());
    CHECK_FALSE(state.NextDeadline().HasExpired(START + ORPHAN - 1ns));
    CHECK(state.NextDeadline().HasExpired(START + ORPHAN));
}

TEST_CASE_FIXTURE(Fixture,
                  "nobody coming back reaps the session at the deadline") {
    StreamState state{Streams(), EDetachedOverflow::Reap};

    state.OnTimer(START + ORPHAN - 1ns);
    CHECK(state.State() == ESessionState::Detached);

    state.OnTimer(START + ORPHAN);
    CHECK(state.State() == ESessionState::Reaped);
    CHECK(state.ReapReason() == ESessionError::OrphanTimeout);
    CHECK_FALSE(state.NextDeadline().IsSet());
}

TEST_CASE_FIXTURE(Fixture, "an orphan timeout of zero waits for ever") {
    StreamState state{Streams(Duration::zero()), EDetachedOverflow::Reap};
    CHECK_FALSE(state.NextDeadline().IsSet());
    state.OnTimer(START + 24h);
    CHECK(state.State() == ESessionState::Detached);
}

TEST_CASE_FIXTURE(Fixture, "a live session holds back at the resend ceiling") {
    StreamState state{Streams(), EDetachedOverflow::Reap};
    REQUIRE(state.Attach(0).has_value());
    CHECK_FALSE(state.NextDeadline().IsSet());

    const auto taken = state.Write(ByteSpan{Bytes(RESEND_MAX + 3)});
    REQUIRE(taken.has_value());
    // Backpressure, not loss: the writer keeps the rest and stops reading.
    CHECK(*taken == RESEND_MAX);
    CHECK_FALSE(state.AcceptsWrites());

    REQUIRE(state.Output().Acknowledge(4).has_value());
    CHECK(state.AcceptsWrites());
}

TEST_CASE_FIXTURE(
    Fixture,
    "the agent reaps on overflow while detached rather than drop a byte") {
    StreamState state{Streams(), EDetachedOverflow::Reap};
    // Past the live ceiling is fine here: nobody is there to apply it for.
    REQUIRE(state.Write(ByteSpan{Bytes(DISCONNECT_MAX - 1)}) ==
            DISCONNECT_MAX - 1);
    CHECK(state.AcceptsWrites());

    const auto overflow = state.Write(ByteSpan{Bytes(2)});
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error() == ESessionError::DisconnectBufferOverflow);
    CHECK(state.State() == ESessionState::Reaped);
    CHECK(state.ReapReason() == ESessionError::DisconnectBufferOverflow);
    // Not one byte of the write that did not fit went in.
    CHECK(state.Output().End() == DISCONNECT_MAX - 1);

    const auto late = state.Write(ByteSpan{Bytes(1)});
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error() == ESessionError::SessionReaped);
    CHECK_FALSE(state.AcceptsWrites());
}

TEST_CASE_FIXTURE(Fixture, "the client holds back on overflow while detached") {
    StreamState state{Streams(), EDetachedOverflow::Backpressure};
    const auto taken = state.Write(ByteSpan{Bytes(DISCONNECT_MAX + 5)});
    REQUIRE(taken.has_value());
    CHECK(*taken == DISCONNECT_MAX);
    CHECK_FALSE(state.AcceptsWrites());
    CHECK(state.State() == ESessionState::Detached);
}

TEST_CASE_FIXTURE(
    Fixture,
    "a newer connection supersedes the old one, which cannot detach it") {
    StreamState state{Streams(), EDetachedOverflow::Reap};
    const auto first = state.Attach(0);
    REQUIRE(first.has_value());
    const auto second = state.Attach(0);
    REQUIRE(second.has_value());

    CHECK_FALSE(state.IsCurrent(*first));
    CHECK(state.IsCurrent(*second));

    // The old connection closing late must not take the new one down.
    state.Detach(*first, START + 1s);
    CHECK(state.State() == ESessionState::Live);

    state.Detach(*second, START + 2s);
    CHECK(state.State() == ESessionState::Detached);
    CHECK_FALSE(state.IsCurrent(*second));
    // The orphan clock restarts from the detach, not from the session's birth.
    CHECK(state.NextDeadline().HasExpired(START + 2s + ORPHAN));
    CHECK_FALSE(state.NextDeadline().HasExpired(START + 2s + ORPHAN - 1ns));
}

TEST_CASE_FIXTURE(Fixture,
                  "attaching resends from what the peer says it holds") {
    StreamState state{Streams(), EDetachedOverflow::Reap};
    REQUIRE(state.Write(ByteSpan{Bytes(10)}) == 10);

    REQUIRE(state.Attach(6).has_value());
    CHECK(state.Output().SendNext() == 6);
    // The peer's offset is an acknowledgement too.
    CHECK(state.Output().Base() == 6);
}

TEST_CASE_FIXTURE(Fixture,
                  "an offset the stream cannot honour refuses the attach") {
    StreamState state{Streams(), EDetachedOverflow::Reap};
    REQUIRE(state.Write(ByteSpan{Bytes(10)}) == 10);

    const auto ahead = state.Attach(11);
    REQUIRE_FALSE(ahead.has_value());
    CHECK(ahead.error() == ESessionError::AckBeyondSent);
    CHECK(state.State() == ESessionState::Detached);

    REQUIRE(state.Output().Acknowledge(8).has_value());
    const auto behind = state.Attach(5);
    REQUIRE_FALSE(behind.has_value());
    CHECK(behind.error() == ESessionError::PeerBehindBuffer);
    CHECK(state.State() == ESessionState::Detached);
}

TEST_CASE_FIXTURE(Fixture,
                  "a reaped session stays reaped for its first reason") {
    StreamState state{Streams(), EDetachedOverflow::Reap};
    const auto live = state.Attach(0);
    REQUIRE(live.has_value());

    state.Kill(ESessionError::PeerBehindBuffer);
    state.Kill(ESessionError::OrphanTimeout);
    CHECK(state.State() == ESessionState::Reaped);
    CHECK(state.ReapReason() == ESessionError::PeerBehindBuffer);
    CHECK_FALSE(state.IsCurrent(*live));

    const auto again = state.Attach(0);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error() == ESessionError::SessionReaped);
    // Detaching a reaped session does not bring it back to Detached.
    state.Detach(*live, START + 1s);
    CHECK(state.State() == ESessionState::Reaped);
}

// ── Item 4: Resync mode AcceptsWrites after ring overflow ─────────────────
//
// When the ring is full in Resync mode (RingOverflowed flag set), the PTY
// loop must keep draining the shell's output — otherwise the process blocks
// on write(2) to the pseudo-terminal.  AcceptsWrites must return true in the
// Resync+Detached+RingOverflowed state.

TEST_CASE_FIXTURE(
    Fixture,
    "Resync: AcceptsWrites stays true after ring overflows so PTY keeps "
    "draining") {
    StreamState state{Streams(), EDetachedOverflow::Resync};

    // Fill the ring to exactly DisconnectMax: ring is full, flag not yet set.
    REQUIRE(state.Write(ByteSpan{Bytes(DISCONNECT_MAX)}) == DISCONNECT_MAX);
    CHECK_FALSE(state.AcceptsWrites());  // full but not overflowed yet

    // One more Write with room=0 triggers SetRingOverflowed.
    const auto overflow = state.Write(ByteSpan{Bytes(1)});
    REQUIRE(overflow.has_value());
    CHECK(*overflow == 0);
    CHECK(state.Output().RingOverflowed());

    // BUG: returned false before the fix — PTY loop stops reading and the
    // process would block.  Fix: return true so the loop keeps draining.
    CHECK(state.AcceptsWrites());
    CHECK(state.State() == ESessionState::Detached);  // session stays alive
}
