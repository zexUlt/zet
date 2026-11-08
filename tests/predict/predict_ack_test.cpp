// How an EchoAck meets the mirror: it waits for the output it speaks of, it
// covers only the input it names, and a stale or future generation is not
// mixed in (design.md §9, "Сверка по EchoAck").
#include <doctest/doctest.h>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

TEST_CASE("an ack waits until the mirror has the output it names") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    r.E.OnEchoAck(EchoAck{0, r.Out + 1, r.In}, r.Now, r.View());
    CHECK(r.Count() == 1);
    CHECK(r.E.ConfirmedEpoch() == 0);

    r.Output("a");  // reaches the named offset
    CHECK(r.Count() == 0);
    CHECK(r.E.ConfirmedEpoch() == 1);
}

TEST_CASE("an ack covers only the input up to its offset") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("ab");
    r.Output("ab");
    r.E.OnEchoAck(EchoAck{0, r.Out, 1}, r.Now, r.View());  // up to "a"
    REQUIRE(r.Count() == 1);
    CHECK(r.E.Predictions()[0].Codepoint == U'b');
    CHECK(r.E.Cursor().has_value());  // the cursor belongs to the last key

    r.E.OnEchoAck(EchoAck{0, r.Out, 2}, r.Now, r.View());
    CHECK(r.Count() == 0);
    CHECK_FALSE(r.E.Cursor().has_value());
}

TEST_CASE("the offsets of the bytes of one call are contiguous") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.In = 10;
    r.Type("xyz");
    REQUIRE(r.Count() == 3);
    CHECK(r.E.Predictions()[0].InputEnd == 11);
    CHECK(r.E.Predictions()[1].InputEnd == 12);
    CHECK(r.E.Predictions()[2].InputEnd == 13);
}

TEST_CASE("an ack of an older generation is dropped") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Gen = 3;
    r.E.OnSnapshot(3, r.Now, r.View());
    r.Out = 0;
    r.Prompt();
    r.Type("a");
    r.Output("a");
    r.E.OnEchoAck(EchoAck{2, r.Out, r.In}, r.Now, r.View());
    CHECK(r.Count() == 1);
    CHECK(r.E.ConfirmedEpoch() == 0);
    r.Ack();
    CHECK(r.E.ConfirmedEpoch() > 0);
}

TEST_CASE("an ack of a later generation waits for its snapshot") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    r.E.OnEchoAck(EchoAck{1, 0, r.In}, r.Now, r.View());
    CHECK(r.Count() == 1);
    CHECK(r.E.StreamGen() == 0);

    r.E.OnSnapshot(1, r.Now, r.View());
    CHECK(r.E.StreamGen() == 1);
    CHECK(r.E.OutputOffset() == 0);
    CHECK(r.Count() == 0);
}

TEST_CASE("a snapshot restarts the output offsets") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Output("some output");
    REQUIRE(r.E.OutputOffset() > 0);
    r.E.OnSnapshot(1, r.Now, r.View());
    CHECK(r.E.OutputOffset() == 0);
    r.Gen = 1;
    r.Out = 0;
    r.Output("x");
    CHECK(r.E.OutputOffset() == 1);
}

TEST_CASE("a full queue of acks drops the oldest, and the newest still works") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    for (std::size_t i = 0; i < ECHO_ACK_PENDING_MAX + 4; ++i) {
        r.E.OnEchoAck(EchoAck{0, r.Out + 1 + i, r.In}, r.Now, r.View());
    }
    CHECK(r.Count() == 1);
    r.Output("a");
    r.Output("     ");
    CHECK(r.Count() == 0);
    CHECK(r.E.ConfirmedEpoch() == 1);
}

TEST_CASE("never ignores everything") {
    Rig r(20, 4, EDisplayMode::Never);
    r.Prompt();
    r.Type("a");
    r.Output("a");
    r.Ack();
    r.E.OnSnapshot(1, r.Now, r.View());
    r.E.OnResize(r.Now, r.View());
    CHECK(r.Count() == 0);
    CHECK(r.E.PredictionEpoch() == 1);
    CHECK(r.E.ConfirmedEpoch() == 0);
    CHECK(r.E.StreamGen() == 0);
    CHECK_FALSE(r.E.ColumnTrusted());
}
