// Epochs and echo-ack reconciliation (design.md §9, "Модель предсказаний" and
// "Сверка по EchoAck"; §16 predict_epoch_test).
#include <doctest/doctest.h>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

TEST_CASE("the first epoch is invisible until a check confirms it") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    CHECK(r.E.PredictionEpoch() == 1);
    CHECK(r.E.ConfirmedEpoch() == 0);

    r.Type("a");
    REQUIRE(r.Count() == 1);
    CHECK(r.E.Predictions()[0].Epoch == 1);
    CHECK(r.E.Predictions()[0].Codepoint == U'a');
    CHECK_FALSE(r.AnyVisible());

    // One correct echo is not enough: a mask glyph would give it too.
    r.Output("a");
    r.Ack();
    CHECK(r.E.ConfirmedEpoch() == 0);
    CHECK(r.Count() == 0);
    r.Type("b");
    CHECK_FALSE(r.AnyVisible());
    r.Output("b");
    r.Ack();
    CHECK(r.E.ConfirmedEpoch() == 1);

    // The next key of the same epoch is visible at once.
    r.Type("c");
    REQUIRE(r.Count() == 1);
    CHECK(r.E.Predictions()[0].Epoch == 1);
    CHECK(r.AnyVisible());
}

TEST_CASE("a check that finds the echo not yet written confirms nothing") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    r.Ack();  // the output has not reached the mirror's cell
    CHECK(r.E.ConfirmedEpoch() == 0);
}

TEST_CASE(
    "a wrong prediction in an invisible epoch kills it without a glitch") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("x");
    REQUIRE(r.Count() == 1);
    CHECK_FALSE(r.AnyVisible());
    const auto epoch = r.E.PredictionEpoch();

    r.Output("y");
    r.Ack();
    CHECK(r.Count() == 0);
    CHECK_FALSE(r.E.Cursor().has_value());
    CHECK(r.E.ConfirmedEpoch() == 0);
    CHECK(r.E.GlitchTrigger() == 0);
    CHECK(r.E.PredictionEpoch() > epoch);
}

TEST_CASE("killing an epoch kills the epochs above it") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");   // epoch 1
    r.Type("\t");  // raises the epoch
    r.Type("b");   // epoch 2
    REQUIRE(r.Count() == 2);
    CHECK(r.E.Predictions()[1].Epoch > r.E.Predictions()[0].Epoch);

    // The echo of "a" is wrong; the ack covers only the first key.
    r.Output("Z");
    r.E.OnEchoAck(EchoAck{0, r.Out, 1}, r.Now, r.View());
    CHECK(r.Count() == 0);
    CHECK_FALSE(r.E.Cursor().has_value());
    CHECK_FALSE(r.AnyVisible());
}

TEST_CASE("a wrong prediction in a visible epoch drops everything and counts") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("b");
    r.Type("c");
    REQUIRE(r.Count() == 2);
    REQUIRE(r.AnyVisible());
    const auto epoch = r.E.PredictionEpoch();

    r.Output("Z");  // not "b"
    r.E.OnEchoAck(EchoAck{0, r.Out, 3}, r.Now, r.View());
    CHECK(r.Count() == 0);
    CHECK_FALSE(r.E.Cursor().has_value());
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
    CHECK(r.E.PredictionEpoch() > epoch);
    CHECK(r.E.ConfirmedEpoch() == 1);
}

TEST_CASE("a match that was already there confirms nothing") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    r.Output("a");   // echoed, not yet acked
    r.Type("\x7f");  // predicts the cell empty again
    r.Type("a");     // and "a" over a cell that already shows "a"
    REQUIRE(r.Count() == 1);
    CHECK(r.E.Predictions()[0].Prior == U'a');

    r.Output("\b \ba");  // the program's echo of both keys
    r.Ack();
    CHECK(r.Count() == 0);
    CHECK(r.E.ConfirmedEpoch() == 0);
}

TEST_CASE("an empty cell and a typed space confirm nothing") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type(" ");
    REQUIRE(r.Count() == 1);
    r.Output(" ");
    r.Ack();
    CHECK(r.Count() == 0);
    CHECK(r.E.ConfirmedEpoch() == 0);
}

TEST_CASE("the predicted cursor is checked against the mirror's cursor") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("b");
    REQUIRE(r.E.Cursor().has_value());
    CHECK(r.E.Cursor()->Col == 5);

    r.Output("b");
    r.Ack();
    CHECK_FALSE(r.E.Cursor().has_value());
    CHECK(r.E.GlitchTrigger() == 0);
}

TEST_CASE("a wrong cursor in a visible epoch is a glitch") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("b");
    // The cell is right, the cursor is not: the program also moved it.
    r.Output("b\x1b[9G");
    r.Ack();
    CHECK(r.Count() == 0);
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
}

TEST_CASE("typing over a predicted cell replaces its prediction") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    r.Type("\x7f");
    r.Type("b");
    r.Type("c");
    // One cell, one prediction: the intermediate states are never compared.
    REQUIRE(r.Count() == 2);
    CHECK(r.E.Predictions()[0].Codepoint == U'b');
    r.Output("bc");
    r.Ack();
    CHECK(r.E.ConfirmedEpoch() == 1);
}
