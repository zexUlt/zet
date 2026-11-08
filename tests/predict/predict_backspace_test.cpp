// Backspace (design.md §9; §16 predict_backspace_test).
#include <doctest/doctest.h>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

TEST_CASE("backspace right of the start of input predicts an empty cell") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();  // cursor at column 2
    r.Type("ab");
    const auto epoch = r.E.PredictionEpoch();
    REQUIRE(r.E.Cursor().has_value());
    CHECK(r.E.Cursor()->Col == 4);

    r.Type("\x7f");
    // The prediction for the cell of "b" is now an empty cell, and the cursor
    // moved left; nothing was raised.
    REQUIRE(r.Count() == 2);
    CHECK(r.E.Predictions()[1].Col == 3);
    CHECK(r.E.Predictions()[1].Codepoint == 0);
    REQUIRE(r.E.Cursor().has_value());
    CHECK(r.E.Cursor()->Col == 3);
    CHECK(r.E.PredictionEpoch() == epoch);
}

TEST_CASE("0x08 is a backspace too") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("ab");
    r.Type("\x08");
    REQUIRE(r.E.Cursor().has_value());
    CHECK(r.E.Cursor()->Col == 3);
}

TEST_CASE("backspace at the start of input raises the epoch") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    r.Type("\x7f");  // back to the start: fine
    const auto epoch = r.E.PredictionEpoch();
    REQUIRE(r.E.Cursor().has_value());
    CHECK(r.E.Cursor()->Col == 2);

    r.Type("\x7f");  // the prompt is not ours
    CHECK(r.E.PredictionEpoch() > epoch);
    CHECK(r.E.Cursor()->Col == 2);
}

TEST_CASE("backspace with nothing typed raises the epoch") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    const auto epoch = r.E.PredictionEpoch();
    r.Type("\x7f");
    CHECK(r.Count() == 0);
    CHECK(r.E.PredictionEpoch() > epoch);
}

TEST_CASE("backspace before text to the right raises the epoch") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt("\r$ abc\x1b[1;5H");  // cursor inside the text, tail not empty
    r.Type("x");                   // in the middle: not predicted
    CHECK(r.Count() == 0);
    // The start of input is only set by a prediction, so backspace is refused.
    const auto epoch = r.E.PredictionEpoch();
    r.Type("\x7f");
    CHECK(r.Count() == 0);
    CHECK(r.E.PredictionEpoch() > epoch);
}

TEST_CASE("backspace with text to the right of the cursor is not predicted") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Confirm("a");
    r.Confirm("b");
    r.Output("\x1b[D");  // the program moved the cursor left of "b"
    const auto epoch = r.E.PredictionEpoch();
    r.Type("\x7f");
    CHECK(r.Count() == 0);
    CHECK(r.E.PredictionEpoch() > epoch);
}

TEST_CASE("the echo of a backspace is checked and earns nothing") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("b");
    r.Output("b");
    r.Type("\x7f");
    REQUIRE(r.Count() == 1);  // the cell of "b" is now predicted empty
    CHECK(r.E.Predictions()[0].Codepoint == 0);
    r.Output("\b \b");
    r.Ack();
    CHECK(r.Count() == 0);
    CHECK(r.E.GlitchTrigger() == 0);
}

TEST_CASE("a backspace the program ignores is a misprediction") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Type("b");
    r.Output("b");
    r.Type("\x7f");
    // The program did not erase: the cell still shows "b".
    r.Ack();
    CHECK(r.Count() == 0);
    CHECK(r.E.GlitchTrigger() == GLITCH_REPAIR_COUNT);
}

TEST_CASE("backspace next to a wide glyph is not predicted") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.Type("a");
    r.Output("\xe4\xb8\xad");  // a wide glyph now sits at the predicted cursor
    const auto epoch = r.E.PredictionEpoch();
    const auto count = r.Count();
    r.Type("\x7f");
    CHECK(r.Count() == count);
    CHECK(r.E.PredictionEpoch() > epoch);
}
