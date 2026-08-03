#include "zet/core/time.hpp"

#include <doctest/doctest.h>

namespace {

using namespace std::chrono_literals;
using zet::Deadline;
using zet::Duration;
using zet::Timestamp;

}  // namespace

TEST_CASE("timestamps subtract into durations and move by them") {
    const Timestamp start = Timestamp::FromNanos(1'000'000);
    const Timestamp later = start + 500ms;

    CHECK((later - start) == 500ms);
    CHECK((start - later) == -500ms);
    CHECK(later > start);
    CHECK((later - 500ms) == start);
}

TEST_CASE(
    "arithmetic is constexpr, so deadlines can be computed at compile time") {
    constexpr Timestamp base = Timestamp::FromNanos(0);
    constexpr Timestamp deadline = base + 30s;
    static_assert(deadline - base == 30s);
    static_assert(deadline > base);
    CHECK(deadline.SinceEpoch() == 30s);
}

TEST_CASE("an unset deadline never expires") {
    const Deadline none;
    CHECK_FALSE(none.IsSet());
    CHECK_FALSE(none.HasExpired(Timestamp::FromNanos(0)));
    // Not even at an absurdly late time: "no deadline" is a distinct state,
    // not a very large number that something might drift past.
    CHECK_FALSE(none.HasExpired(Timestamp{Duration::max()}));
}

TEST_CASE("a deadline expires at its instant, not after it") {
    const Timestamp at = Timestamp::FromNanos(1000);
    const Deadline d{at};

    CHECK(d.IsSet());
    CHECK_FALSE(d.HasExpired(at - 1ns));
    CHECK(d.HasExpired(at));
    CHECK(d.HasExpired(at + 1ns));
}

TEST_CASE("Earliest treats an unset deadline as infinitely far away") {
    const Deadline none;
    const Deadline soon{Timestamp::FromNanos(10)};
    const Deadline late{Timestamp::FromNanos(1000)};

    CHECK(Earliest(soon, late).At() == soon.At());
    CHECK(Earliest(late, soon).At() == soon.At());
    CHECK(Earliest(none, soon).At() == soon.At());
    CHECK(Earliest(soon, none).At() == soon.At());
    CHECK_FALSE(Earliest(none, Deadline{}).IsSet());
}
