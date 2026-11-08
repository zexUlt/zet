// One case for every source of a raised epoch or a reset (design.md §9;
// §16 predict_tentative_test). Each case first makes a prediction or two, so
// that a reset is visible as an emptied list, and a refusal as an unchanged
// one.
#include <doctest/doctest.h>

#include <string>

#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

namespace {

// A rig with the column known, the first epoch confirmed and one key typed
// ahead of the echo.
struct Ready {
    Rig R{20, 5, EDisplayMode::Always};
    Ready() {
        R.Prompt();
        R.Confirm("a");
        R.Type("b");
    }
};

// Typing `keys` is refused: no new prediction, epoch raised.
void CheckInputRefused(Rig& r, const std::string& keys) {
    const auto before = r.Count();
    const auto epoch = r.E.PredictionEpoch();
    r.Type(keys);
    CHECK(r.Count() == before);
    CHECK(r.E.PredictionEpoch() > epoch);
}

// The output made the engine drop everything and raise the epoch.
void CheckOutputResets(Rig& r, const std::string& out) {
    REQUIRE(r.Count() > 0);
    const auto epoch = r.E.PredictionEpoch();
    r.Output(out);
    CHECK(r.Count() == 0);
    CHECK_FALSE(r.E.Cursor().has_value());
    CHECK(r.E.PredictionEpoch() > epoch);
}

}  // namespace

// --- Input ------------------------------------------------------------------

TEST_CASE("CR raises the epoch") {
    Ready t;
    CheckInputRefused(t.R, "\r");
}

TEST_CASE("LF raises the epoch") {
    Ready t;
    CheckInputRefused(t.R, "\n");
}

TEST_CASE("Tab raises the epoch") {
    Ready t;
    CheckInputRefused(t.R, "\t");
}

TEST_CASE("other control characters raise the epoch") {
    Ready t;
    CheckInputRefused(t.R, "\x03");
    CheckInputRefused(t.R, "\x01");
    CheckInputRefused(t.R, "\x1f");
}

TEST_CASE("a lone ESC raises the epoch") {
    Ready t;
    CheckInputRefused(t.R, "\x1b");
}

TEST_CASE("arrows raise the epoch, and their letters are not typed text") {
    Ready t;
    CheckInputRefused(t.R, "\x1b[D");
    CheckInputRefused(t.R, "\x1b[C");
    CheckInputRefused(t.R, "\x1bOA");
    CheckInputRefused(t.R, "\x1b[1;5D");
    CheckInputRefused(t.R, "\x1bx");  // alt-x
}

TEST_CASE("a wide character raises the epoch") {
    Ready t;
    CheckInputRefused(t.R, "\xe4\xb8\xad");  // U+4E2D
}

TEST_CASE("a zero-width character raises the epoch") {
    Ready t;
    CheckInputRefused(t.R, "\xcc\x81");  // U+0301 combining acute
}

TEST_CASE("a C1 control raises the epoch") {
    Ready t;
    CheckInputRefused(t.R, "\xc2\x85");  // U+0085
}

TEST_CASE("invalid UTF-8 raises the epoch") {
    Ready t;
    CheckInputRefused(t.R, "\xff");
    CheckInputRefused(t.R, "\x80");
    CheckInputRefused(t.R, "\xc0\x80");      // overlong
    CheckInputRefused(t.R, "\xed\xa0\x80");  // surrogate
}

TEST_CASE(
    "a lead byte without its continuation raises the epoch and the next "
    "key is its own") {
    Ready t;
    const auto before = t.R.Count();
    const auto epoch = t.R.E.PredictionEpoch();
    t.R.Type("\xc3(");
    CHECK(t.R.E.PredictionEpoch() > epoch);
    REQUIRE(t.R.Count() == before + 1);
    CHECK(t.R.E.Predictions()[before].Codepoint == U'(');
}

TEST_CASE("a width-1 non-ASCII character is predicted") {
    Ready t;
    const auto before = t.R.Count();
    t.R.Type("\xc3\xa9");  // é
    CHECK(t.R.Count() == before + 1);
    CHECK(t.R.E.Predictions()[before].Codepoint == U'é');
}

TEST_CASE("the last column raises the epoch") {
    Rig r(6, 3, EDisplayMode::Always);
    r.Prompt("\rabcd");  // cursor in column 4 of 6
    r.Type("e");         // column 4: predicted, the cursor moves to the last
    r.Type("f");         // the last column: refused
    CHECK(r.Count() == 1);
    CheckInputRefused(r, "g");
}

TEST_CASE("a pending wrap raises the epoch") {
    Rig r(6, 3, EDisplayMode::Always);
    r.Prompt(
        "\rabcdef");  // the sixth glyph is in the last column, wrap pending
    REQUIRE(r.G->PendingWrap());
    CheckInputRefused(r, "x");
}

TEST_CASE("text to the right of the cursor raises the epoch") {
    Rig r(20, 3, EDisplayMode::Always);
    r.Prompt("\rabc\r");  // cursor at column 0, text on the line
    CheckInputRefused(r, "x");
}

TEST_CASE("bracketed paste is not predicted, and ends where it ends") {
    Ready t;
    const auto before = t.R.Count();
    const auto epoch = t.R.E.PredictionEpoch();
    t.R.Type("\x1b[200~hel\nlo\x1b[201~");
    CHECK(t.R.Count() == before);
    CHECK(t.R.E.PredictionEpoch() > epoch);
    // Typing resumes after the end marker.
    t.R.Type("c");
    CHECK(t.R.Count() == before + 1);
}

TEST_CASE("a paste split between calls is still a paste") {
    Ready t;
    const auto before = t.R.Count();
    t.R.Type("\x1b[2");
    t.R.Type("00~ab");
    t.R.Type("c\x1b");
    t.R.Type("[201");
    t.R.Type("~");
    CHECK(t.R.Count() == before);
    t.R.Type("d");
    CHECK(t.R.Count() == before + 1);
}

TEST_CASE("alt screen is not predicted") {
    Rig r(20, 5, EDisplayMode::Always);
    r.Prompt("\x1b[?1049h\x1b[1;1H");
    CheckInputRefused(r, "a");
    CHECK(r.Count() == 0);
}

TEST_CASE("insert mode is not predicted") {
    Rig r(20, 5, EDisplayMode::Always);
    r.Prompt("\r$ \x1b[4h");
    CheckInputRefused(r, "a");
    CHECK(r.Count() == 0);
}

TEST_CASE("shift-out is not predicted") {
    Rig r(20, 5, EDisplayMode::Always);
    r.Prompt("\r$ \x0e");
    CheckInputRefused(r, "a");
    CHECK(r.Count() == 0);
}

TEST_CASE("a non-ASCII G0 is not predicted") {
    Rig r(20, 5, EDisplayMode::Always);
    r.Prompt("\r$ \x1b(0");
    CheckInputRefused(r, "a");
    CHECK(r.Count() == 0);
}

// --- Output ------------------------------------------------------------------

TEST_CASE("a change of cursor row resets") {
    Ready t;
    CheckOutputResets(t.R, "\n");
}

TEST_CASE("a scroll resets") {
    Rig r(20, 3, EDisplayMode::Always);
    r.Prompt("\r\n\n\r$ ");  // cursor on the bottom row
    r.Confirm("a");
    r.Type("b");
    REQUIRE(r.G->CursorRow() == 2);
    CheckOutputResets(r, "\x1b[S");  // scroll up; the cursor stays on its row
}

TEST_CASE("a scroll at the bottom by line feed resets") {
    Rig r(20, 3, EDisplayMode::Always);
    r.Prompt("\r\n\n\r$ ");
    r.Confirm("a");
    r.Type("b");
    CheckOutputResets(r, "\n");
}

TEST_CASE("a resize resets and unsettles the column") {
    Ready t;
    REQUIRE(t.R.Count() > 0);
    const auto epoch = t.R.E.PredictionEpoch();
    REQUIRE(t.R.G->Resize(30, 8).has_value());
    t.R.E.OnResize(t.R.Now, t.R.View());
    CHECK(t.R.Count() == 0);
    CHECK_FALSE(t.R.E.ColumnTrusted());
    CHECK(t.R.E.PredictionEpoch() > epoch);
}

TEST_CASE("a resize noticed only through the mirror's size resets too") {
    Ready t;
    REQUIRE(t.R.G->Resize(30, 8).has_value());
    t.R.Output("");
    CHECK(t.R.Count() == 0);
    CHECK_FALSE(t.R.E.ColumnTrusted());
}

TEST_CASE("a snapshot resets") {
    Ready t;
    const auto epoch = t.R.E.PredictionEpoch();
    t.R.Gen = 1;
    t.R.Out = 0;
    t.R.E.OnSnapshot(1, t.R.Now, t.R.View());
    CHECK(t.R.Count() == 0);
    CHECK_FALSE(t.R.E.Cursor().has_value());
    CHECK(t.R.E.PredictionEpoch() > epoch);
}

TEST_CASE("entering the alt screen resets") {
    Ready t;
    CheckOutputResets(t.R, "\x1b[?1049h");
}

TEST_CASE("leaving the alt screen raises the epoch") {
    Rig r(20, 5, EDisplayMode::Always);
    r.Prompt("\x1b[?1049h\x1b[1;1H");
    const auto epoch = r.E.PredictionEpoch();
    r.Output("\x1b[?1049l");
    CHECK(r.E.PredictionEpoch() > epoch);
}

TEST_CASE("DCS resets and unsettles the column") {
    Ready t;
    REQUIRE(t.R.E.ColumnTrusted());
    CheckOutputResets(t.R, "\x1bPq#0;2;0;0;0\x1b\\");
    CHECK_FALSE(t.R.E.ColumnTrusted());
}

TEST_CASE("APC resets and unsettles the column") {
    Ready t;
    CheckOutputResets(t.R, "\x1b_Gi=1;AAAA\x1b\\");
    CHECK_FALSE(t.R.E.ColumnTrusted());
}

TEST_CASE("OSC does not reset") {
    Ready t;
    const auto before = t.R.Count();
    t.R.Output("\x1b]0;title\x07");
    CHECK(t.R.Count() == before);
    CHECK(t.R.E.ColumnTrusted());
}

TEST_CASE("an unknown sequence resets and unsettles the column") {
    Ready t;
    const auto known = t.R.P.UnknownSeqCount();
    const auto epoch = t.R.E.PredictionEpoch();
    // The parser counts this one; the assertion keeps the case honest if the
    // grid ever learns it.
    t.R.Output("\x1b[1;2;3 !x");
    REQUIRE(t.R.P.UnknownSeqCount() > known);
    CHECK(t.R.Count() == 0);
    CHECK_FALSE(t.R.E.ColumnTrusted());
    CHECK(t.R.E.PredictionEpoch() > epoch);
}

TEST_CASE("output that changes nothing relevant leaves predictions alone") {
    Ready t;
    const auto before = t.R.Count();
    const auto epoch = t.R.E.PredictionEpoch();
    t.R.Output("\x1b[1m");  // SGR
    t.R.Output("b");        // the echo of the predicted key
    CHECK(t.R.Count() == before);
    CHECK(t.R.E.PredictionEpoch() == epoch);
}
