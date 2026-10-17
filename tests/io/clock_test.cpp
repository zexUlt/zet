#include "zet/io/clock.hpp"

#include <doctest/doctest.h>

TEST_CASE("the monotonic clock does not run backwards") {
    const zet::Timestamp first = zet::io::MonotonicNow();
    const zet::Timestamp second = zet::io::MonotonicNow();
    CHECK(second >= first);
    CHECK(first.SinceEpoch().count() > 0);
}
