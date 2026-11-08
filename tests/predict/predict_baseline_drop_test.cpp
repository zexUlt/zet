// The first EchoAck of a connection is a baseline (design.md §9): predictions
// for input up to its input offset are dropped with no verdict, and later ones
// are left alone.
#include <doctest/doctest.h>

#include <chrono>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

TEST_CASE("a baseline drops the predictions it covers, with no glitch") {
    Rig r(20, 4, EDisplayMode::Adaptive);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("x");
    r.Type("y");
    REQUIRE(r.Count() == 2);
    const auto epoch = r.E.PredictionEpoch();
    r.Advance(std::chrono::seconds{6});  // as old as a glitch makes them
    r.Baseline();
    CHECK(r.Count() == 0);
    CHECK_FALSE(r.E.Cursor().has_value());
    CHECK(r.E.GlitchTrigger() == 0);
    CHECK_FALSE(r.E.ShowPredictions());
    CHECK(r.E.PredictionEpoch() == epoch);
    CHECK(r.E.ConfirmedEpoch() >= 1);
}

TEST_CASE("a baseline leaves the predictions for later input") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("x");
    const auto covered = r.In;
    r.Type("y");
    REQUIRE(r.Count() == 2);
    r.E.OnBaseline(EchoAck{r.Gen, r.Out, covered}, r.Now, r.View());
    REQUIRE(r.Count() == 1);
    CHECK(r.E.Predictions()[0].Codepoint == U'y');
    CHECK(r.E.Cursor().has_value());
}

TEST_CASE("a baseline does nothing with --predict=never") {
    Rig r(20, 4, EDisplayMode::Never);
    r.Prompt();
    r.Type("x");
    r.Baseline();
    CHECK(r.Count() == 0);
}
