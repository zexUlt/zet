// The moment the client's poll must wake the engine (design.md §9, "Когда
// показывать": the glitch thresholds).
#include <doctest/doctest.h>

#include <chrono>

#include "predict_rig.hpp"
#include "zet/predict/limits.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

TEST_CASE("with nothing predicted there is no deadline") {
    Rig r(40, 4, EDisplayMode::Adaptive);
    CHECK_FALSE(r.E.NextDeadline().has_value());
}

TEST_CASE("never has no deadline") {
    Rig r(40, 4, EDisplayMode::Never);
    r.Prompt();
    r.Type("a");
    CHECK_FALSE(r.E.NextDeadline().has_value());
}

TEST_CASE("the first deadline is the oldest prediction's glitch threshold") {
    Rig r(40, 4, EDisplayMode::Adaptive);
    r.Prompt();
    r.ConfirmEpoch();
    const auto created = r.Now;
    r.Type("b");
    REQUIRE(r.Count() == 1);
    const auto deadline = r.E.NextDeadline();
    REQUIRE(deadline.has_value());
    CHECK(*deadline == created + GLITCH_THRESHOLD);

    // Ticking at the deadline counts the glitch; the next is the flag one.
    r.Now = *deadline;
    r.E.OnTick(r.Now);
    const auto later = r.E.NextDeadline();
    REQUIRE(later.has_value());
    CHECK(*later > *deadline);
}

TEST_CASE("an answered prediction takes its deadline away") {
    Rig r(40, 4, EDisplayMode::Adaptive);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("b");
    REQUIRE(r.E.NextDeadline().has_value());
    r.Output("b");
    r.Ack();
    CHECK_FALSE(r.E.NextDeadline().has_value());
}
