// When predictions are shown: SRTT hysteresis, glitches, always and never
// (design.md §9, "Когда показывать"; §16 predict_trigger_test).
#include <doctest/doctest.h>

#include <chrono>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;
using std::chrono::milliseconds;

namespace {

Rig Adaptive() {
    // Rig is not movable (the engine refers to the grid it owns), so build it
    // through guaranteed elision.
    return Rig(40, 4, EDisplayMode::Adaptive);
}

void Srtt(Rig& r, int ms) { r.E.OnSrtt(milliseconds(ms), r.Now); }

}  // namespace

TEST_CASE("without a measurement adaptive shows nothing") {
    Rig r = Adaptive();
    CHECK_FALSE(r.E.ShowPredictions());
    CHECK_FALSE(r.E.UnderlinePredictions());
}

TEST_CASE("show switches on above SRTT/2 of 30 ms and not at 30") {
    Rig r = Adaptive();
    Srtt(r, 60);  // SRTT/2 = 30: not above
    CHECK_FALSE(r.E.ShowPredictions());
    Srtt(r, 61);
    CHECK(r.E.ShowPredictions());
}

TEST_CASE("show switches off at SRTT/2 of 20 ms or less, with hysteresis") {
    Rig r = Adaptive();
    Srtt(r, 100);
    REQUIRE(r.E.ShowPredictions());
    Srtt(r, 60);  // between the thresholds: stays on
    CHECK(r.E.ShowPredictions());
    Srtt(r, 41);  // SRTT/2 = 20.5: stays on
    CHECK(r.E.ShowPredictions());
    Srtt(r, 40);  // SRTT/2 = 20: off
    CHECK_FALSE(r.E.ShowPredictions());
    Srtt(r, 55);  // and between the thresholds it stays off
    CHECK_FALSE(r.E.ShowPredictions());
}

TEST_CASE("show does not switch off while something is drawn") {
    Rig r = Adaptive();
    r.Prompt();
    Srtt(r, 100);
    r.Type("a");
    REQUIRE(r.Count() == 1);
    r.E.SetDrawn(0, true);

    Srtt(r, 10);
    CHECK(r.E.ShowPredictions());
    r.E.OnTick(r.Now);
    CHECK(r.E.ShowPredictions());

    r.E.ClearDrawn();
    r.E.OnTick(r.Now);
    CHECK_FALSE(r.E.ShowPredictions());
}

TEST_CASE("underline switches on above SRTT/2 of 80 ms, off at 50 ms or less") {
    Rig r = Adaptive();
    Srtt(r, 160);
    CHECK_FALSE(r.E.UnderlinePredictions());
    Srtt(r, 161);
    CHECK(r.E.UnderlinePredictions());
    Srtt(r, 120);  // SRTT/2 = 60: between, stays
    CHECK(r.E.UnderlinePredictions());
    Srtt(r, 101);  // 50.5
    CHECK(r.E.UnderlinePredictions());
    Srtt(r, 100);  // 50
    CHECK_FALSE(r.E.UnderlinePredictions());
    Srtt(r, 130);
    CHECK_FALSE(r.E.UnderlinePredictions());
}

TEST_CASE("an unconfirmed prediction shows predictions at low SRTT") {
    Rig r = Adaptive();
    r.Prompt();
    Srtt(r, 10);
    r.Type("a");
    r.Advance(milliseconds(249));
    r.E.OnTick(r.Now);
    CHECK_FALSE(r.E.ShowPredictions());
    r.Advance(milliseconds(1));
    r.E.OnTick(r.Now);
    CHECK(r.E.ShowPredictions());
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
    CHECK_FALSE(r.E.UnderlinePredictions());
}

TEST_CASE("a prediction unconfirmed for five seconds also underlines") {
    Rig r = Adaptive();
    r.Prompt();
    Srtt(r, 10);
    r.Type("a");
    r.Advance(milliseconds(4999));
    r.E.OnTick(r.Now);
    CHECK_FALSE(r.E.UnderlinePredictions());
    r.Advance(milliseconds(1));
    r.E.OnTick(r.Now);
    CHECK(r.E.ShowPredictions());
    CHECK(r.E.UnderlinePredictions());
    CHECK(r.E.GlitchTrigger() == 2 * GLITCH_REPAIR_COUNT);
}

TEST_CASE("quick confirmations repair a glitch, one per 150 ms") {
    Rig r = Adaptive();
    r.Prompt();
    Srtt(r, 10);
    r.Type("a");
    r.Advance(milliseconds(300));
    r.E.OnTick(r.Now);
    REQUIRE(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
    r.Output("a");
    r.Ack();  // slow: counts for nothing
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);

    // Ten quick ones, spaced enough.
    for (std::uint32_t i = 1; i <= GLITCH_REPAIR_COUNT; ++i) {
        CHECK(r.E.ShowPredictions());
        r.Advance(milliseconds(200));
        r.Type("b");
        r.Advance(milliseconds(10));
        r.Output("b");
        r.Ack();
        CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT - i);
    }
    CHECK_FALSE(r.E.ShowPredictions());
}

TEST_CASE("confirmations closer than 150 ms do not each count") {
    Rig r = Adaptive();
    r.Prompt();
    Srtt(r, 10);
    r.Type("a");
    r.Advance(milliseconds(300));
    r.E.OnTick(r.Now);
    REQUIRE(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
    r.Output("a");
    r.Ack();  // slow, counts for nothing

    for (int i = 0; i < 4; ++i) {
        r.Advance(milliseconds(100));
        r.Type("b");
        r.Output("b");
        r.Ack();
    }
    // At 100 ms spacing only every second one counts: the first, the third.
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT - 2);
}

TEST_CASE("a visible misprediction turns display on at low SRTT") {
    Rig r = Adaptive();
    r.Prompt();
    Srtt(r, 10);
    r.Confirm("a");
    CHECK_FALSE(r.E.ShowPredictions());
    r.Type("b");
    r.Output("Z");
    r.Ack();
    CHECK(r.E.ShowPredictions());
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
}

TEST_CASE("always shows confirmed epochs at once, never nothing") {
    Rig a(40, 4, EDisplayMode::Always);
    a.Prompt();
    CHECK(a.E.ShowPredictions());
    a.Type("x");
    CHECK_FALSE(a.AnyVisible());  // the epoch is not confirmed yet
    a.Output("x");
    a.Ack();
    a.Type("y");
    CHECK(a.AnyVisible());

    Rig n(40, 4, EDisplayMode::Never);
    n.Prompt();
    n.E.OnSrtt(milliseconds(500), n.Now);
    n.Type("x");
    n.Output("x");
    n.Ack();
    n.Type("y");
    CHECK_FALSE(n.E.ShowPredictions());
    CHECK_FALSE(n.E.UnderlinePredictions());
    CHECK(n.Count() == 0);
    CHECK_FALSE(n.E.Cursor().has_value());
    CHECK(n.E.PredictionEpoch() == 1);
    CHECK(n.E.ConfirmedEpoch() == 0);
}

TEST_CASE("underline in always follows the SRTT thresholds") {
    Rig a(40, 4, EDisplayMode::Always);
    CHECK_FALSE(a.E.UnderlinePredictions());
    a.E.OnSrtt(milliseconds(200), a.Now);
    CHECK(a.E.UnderlinePredictions());
}
