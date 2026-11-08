// Boundaries that mutation testing found unchecked: which echo-ack a full
// queue keeps, an ack of a future generation, and the two time windows of
// glitch repair (design.md §9; §16 predict_boundary_test).
#include <doctest/doctest.h>

#include <chrono>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;
using std::chrono::milliseconds;

namespace {

/// An adaptive rig at low SRTT with the glitch count raised to
/// GLITCH_REPAIR_COUNT by a prediction left unconfirmed for 300 ms, and that
/// prediction settled slowly (no credit towards repair).
void Glitchy(Rig& r) {
    r.E.OnSrtt(milliseconds(10), r.Now);
    r.Prompt();
    r.Type("a");
    r.Advance(milliseconds(300));
    r.E.OnTick(r.Now);
    REQUIRE(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
    r.Output("a");
    r.Ack();
    REQUIRE(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
}

}  // namespace

TEST_CASE("a full ack queue keeps the newest ack, which covers the input") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    const std::uint64_t waitFor = r.Out + 5;
    // Sixteen acks that cover nothing, then one that covers the keystroke.
    for (std::size_t i = 0; i < ECHO_ACK_PENDING_MAX; ++i) {
        r.E.OnEchoAck(EchoAck{0, waitFor, 0}, r.Now, r.View());
    }
    r.E.OnEchoAck(EchoAck{0, waitFor, r.In}, r.Now, r.View());
    REQUIRE(r.Count() == 1);
    r.Output("a    ");
    CHECK(r.Count() == 0);
    CHECK(r.E.ConfirmedEpoch() == 1);
}

TEST_CASE(
    "an ack of a future generation is kept until its snapshot, not dropped") {
    Rig r(40, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    // From generation 1, naming output not yet read and input not yet typed.
    r.E.OnEchoAck(EchoAck{1, 1, r.In + 1}, r.Now, r.View());
    r.E.OnSnapshot(1, r.Now, r.View());
    r.Gen = 1;
    r.Out = 0;
    r.Type("b");
    REQUIRE(r.Count() == 1);
    r.Output("b");
    CHECK(r.Count() == 0);
    CHECK(r.E.ConfirmedEpoch() > 0);
}

TEST_CASE("a confirmation at exactly 250 ms is not quick") {
    Rig r(40, 4, EDisplayMode::Adaptive);
    Glitchy(r);
    r.Type("b");
    r.Advance(milliseconds(250));
    r.Output("b");
    r.Ack();
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
}

TEST_CASE("a confirmation at 249 ms is quick") {
    Rig r(40, 4, EDisplayMode::Adaptive);
    Glitchy(r);
    r.Type("b");
    r.Advance(milliseconds(249));
    r.Output("b");
    r.Ack();
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT - 1);
}

TEST_CASE("quick confirmations exactly 150 ms apart both count") {
    Rig r(40, 4, EDisplayMode::Adaptive);
    Glitchy(r);
    r.Type("b");
    r.Output("b");
    r.Ack();
    REQUIRE(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT - 1);
    r.Advance(milliseconds(150));
    r.Type("c");
    r.Output("c");
    r.Ack();
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT - 2);
}

TEST_CASE("quick confirmations 149 ms apart count once") {
    Rig r(40, 4, EDisplayMode::Adaptive);
    Glitchy(r);
    r.Type("b");
    r.Output("b");
    r.Ack();
    r.Advance(milliseconds(149));
    r.Type("c");
    r.Output("c");
    r.Ack();
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT - 1);
}
