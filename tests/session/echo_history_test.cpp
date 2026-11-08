/// EchoHistory (design.md §9, «Предиктивное эхо»): the pairs «end of the input
/// the pty took, moment» behind the EchoAck. Time is handed in.

#include "zet/session/echo_history.hpp"

#include <doctest/doctest.h>

#include <chrono>

#include "zet/core/time.hpp"
#include "zet/session/limits.hpp"

namespace {

using namespace std::chrono_literals;
using namespace zet;
using namespace zet::session;

const Timestamp T0 = Timestamp::FromNanos(1'000'000'000);

}  // namespace

TEST_CASE("an empty history has no deadline and nothing aged") {
    EchoHistory history;
    CHECK_FALSE(history.NextDeadline(0).IsSet());
    CHECK_FALSE(history.Aged(0, T0 + 1h).has_value());
    CHECK(history.Confirmed() == 0);
}

TEST_CASE("a pair counts once it is ECHO_TIMEOUT old, not before") {
    EchoHistory history;
    history.Record(3, T0);
    CHECK(history.NextDeadline(0).At() == T0 + ECHO_TIMEOUT);
    CHECK_FALSE(history.Aged(0, T0 + ECHO_TIMEOUT - 1ns).has_value());
    CHECK(history.Aged(0, T0 + ECHO_TIMEOUT) == 3);
}

TEST_CASE("the largest aged end wins and younger pairs wait") {
    EchoHistory history;
    history.Record(1, T0);
    history.Record(2, T0 + 5ms);
    history.Record(3, T0 + 30ms);
    CHECK(history.Aged(0, T0 + 50ms) == 1);
    CHECK(history.Aged(0, T0 + 55ms) == 2);
    CHECK(history.Aged(0, T0 + 80ms) == 3);
}

TEST_CASE("the floor hides the pairs already reported") {
    EchoHistory history;
    history.Record(1, T0);
    history.Record(2, T0 + 5ms);
    CHECK(history.NextDeadline(1).At() == T0 + 5ms + ECHO_TIMEOUT);
    CHECK_FALSE(history.Aged(1, T0 + 50ms).has_value());
    CHECK(history.Aged(1, T0 + 55ms) == 2);
    CHECK_FALSE(history.NextDeadline(2).IsSet());
}

TEST_CASE("confirming spends the pairs up to it and only moves forward") {
    EchoHistory history;
    history.Record(1, T0);
    history.Record(2, T0 + 5ms);
    history.Record(3, T0 + 10ms);
    history.Confirm(2);
    CHECK(history.Confirmed() == 2);
    CHECK(history.Size() == 1);
    history.Confirm(1);
    CHECK(history.Confirmed() == 2);
    // An end the confirmation already covers adds nothing.
    history.Record(2, T0 + 20ms);
    CHECK(history.Size() == 1);
}

TEST_CASE("an end that does not grow adds nothing") {
    EchoHistory history;
    history.Record(5, T0);
    history.Record(5, T0 + 1ms);
    history.Record(4, T0 + 2ms);
    CHECK(history.Size() == 1);
    CHECK(history.NextDeadline(0).At() == T0 + ECHO_TIMEOUT);
}

TEST_CASE("a full history replaces its last pair, which only delays") {
    EchoHistory history;
    for (std::size_t i = 1; i <= ECHO_HISTORY_MAX; ++i) {
        history.Record(i, T0);
    }
    CHECK(history.Size() == ECHO_HISTORY_MAX);
    // Six more at a later moment fold into the last slot, with its new moment.
    for (std::size_t i = 1; i <= 6; ++i) {
        history.Record(ECHO_HISTORY_MAX + i, T0 + 40ms);
    }
    CHECK(history.Size() == ECHO_HISTORY_MAX);
    // At T0 + 50 ms the first 63 are old enough; the replaced one is not.
    CHECK(history.Aged(0, T0 + 50ms) == ECHO_HISTORY_MAX - 1);
    CHECK(history.Aged(0, T0 + 90ms) == ECHO_HISTORY_MAX + 6);
}

TEST_CASE("the ring survives wrapping around") {
    EchoHistory history;
    std::uint64_t end = 0;
    for (int round = 0; round < 5; ++round) {
        for (std::size_t i = 0; i < ECHO_HISTORY_MAX - 3; ++i) {
            history.Record(++end, T0 + std::chrono::milliseconds{round * 100});
        }
        history.Confirm(end - 1);
        CHECK(history.Size() == 1);
        CHECK(history.Aged(history.Confirmed(), T0 + 1h) == end);
    }
}
