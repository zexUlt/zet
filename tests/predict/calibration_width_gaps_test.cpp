// Width solver cases the first C4 tests left open, found by mutating the
// solver (review of C4): the difference above the glyph count, a second open
// class that shares the path, a class given up on, the fail counter of a class
// that is not on the path, and the typed-glyph ban that only A lifts.
#include <doctest/doctest.h>

#include <string>

#include "calibration_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;

namespace {

using grid::EWidthClass;

const std::string RU = "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
const std::string PL = "\xee\x82\xb0";
const std::string CLOUD = "\xe2\x98\x81\xef\xb8\x8f";
const std::string ZH = "\xd0\xb6";

void Start(CalClient& t) {
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
}

void Confirm(CalClient& t) {
    t.Key("a");
    t.Agent("a");
    t.Ack();
    t.Key("b");
    t.Agent("b");
    t.Ack();
}

/// A terminal that answers by script: the start is answered at column 2.
CalClient* ScriptedStart(CalClient& t) {
    t.Model.Reply = EReply::Never;
    Start(t);
    t.Scripted(0, 2);
    return &t;
}

}  // namespace

TEST_CASE("A difference above the glyph count gives no verdict") {
    CalClient t(40, 6);
    ScriptedStart(t);
    t.Agent("\r" + RU + "> ");  // the mirror stands at column 8
    REQUIRE(t.Frozen());
    t.Scripted(0, 8 + 6 + 1);  // d = n + 1: no width explains it
    CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Unknown);
    CHECK(t.R().G->Policy().Ambiguous == 1);
}

TEST_CASE(
    "A difference equal to the glyphs of the other open class: no verdict") {
    // A is two cells, P one. The path holds one glyph of each, d is 1: that is
    // A's width, but one glyph of P could be the one.
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.Agent("\r" + ZH + PL + "> ");
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Unknown);
    CHECK(t.Cal->Verdict(EWidthClass::PrivateUse) == EWidthVerdict::Unknown);
    // The same with E in the place of P.
    CalClient u(40, 6);
    u.SetTerminal(Terminal(2, 1, false));
    Start(u);
    u.Agent("\r" + ZH + CLOUD + "> ");
    CHECK(u.Queries() == 2);
    CHECK(u.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Unknown);
    CHECK(u.Cal->Verdict(EWidthClass::Emoji) == EWidthVerdict::Unknown);
}

TEST_CASE("A class given up on keeps another from a Wide verdict") {
    CalClient t(40, 6);
    ScriptedStart(t);
    // Three answers that fit no width of A (d = 1 for six glyphs).
    for (int i = 0; i < CPR_SOLVE_FAILS_MAX; ++i) {
        t.Agent("\r" + RU + "> ");
        REQUIRE(t.Frozen());
        t.Scripted(0, 8 + 1);
    }
    REQUIRE(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Unsolved);
    // One glyph of A (given up on) and one of P: d = 1 is P's width or A's.
    const std::size_t asked = t.Queries();
    t.Agent("\r" + ZH + PL + "> ");
    REQUIRE(t.Queries() == asked + 1);
    REQUIRE(t.Frozen());
    t.Scripted(0, 4 + 1);
    CHECK(t.Cal->Verdict(EWidthClass::PrivateUse) == EWidthVerdict::Unknown);
    CHECK(t.R().G->Policy().PrivateUse == 1);
}

TEST_CASE(
    "Answers about A leave the fail count of a class off the path alone") {
    CalClient t(40, 6);
    ScriptedStart(t);
    for (int i = 0; i < CPR_SOLVE_FAILS_MAX; ++i) {
        t.Agent("\r" + RU + "> ");
        REQUIRE(t.Frozen());
        t.Scripted(0, 8 + 1);
    }
    REQUIRE(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Unsolved);
    CHECK(t.Cal->Verdict(EWidthClass::PrivateUse) == EWidthVerdict::Unknown);
    CHECK(t.Cal->Verdict(EWidthClass::Emoji) == EWidthVerdict::Unknown);
}

TEST_CASE("Narrow for A does not lift the ban on a typed private use glyph") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(1, 1, false));
    Start(t);
    t.Agent("\r" + RU + "> ");
    REQUIRE(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Narrow);
    Confirm(t);
    t.Key(PL);
    CHECK(t.R().Count() == 0);
}

TEST_CASE("A cursor left of the mirror's gives no verdict and erases nothing") {
    CalClient t(40, 6);
    ScriptedStart(t);
    for (int i = 0; i < CPR_SOLVE_FAILS_MAX + 1; ++i) {
        t.Agent("\r" + RU + "> ");  // the mirror stands at column 8
        REQUIRE(t.Frozen());
        t.Scripted(0, 4);
        // Not a width's doing, so not a failed solve either.
        CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Unknown);
    }
    CHECK(t.Cal->ColumnFixed() == CPR_SOLVE_FAILS_MAX + 1);
    const auto& g = *t.R().G;
    CHECK(g.CursorCol() == 4);
    // Cells right of the cursor may be on the terminal's screen still: the
    // row is foreign, nothing is erased.
    CHECK(g.KnownFrom(0) == g.Cols());
    CHECK(g.CellAt(0, 3).Codepoint != 0);
    CHECK(g.CellAt(0, 6).Codepoint == U'>');
}

TEST_CASE(
    "A cursor right of the mirror's over a blank tail erases from there") {
    // Case 2 of the design: the tail is blank on the terminal too. A blank
    // cell with a background colour is blank by the mirror's reckoning and
    // stays such a cell only if the erase leaves it alone.
    CalClient t(40, 6);
    ScriptedStart(t);
    t.Agent("\x1b[1;13H\x1b[41m   \x1b[0m\r" + RU + "> ");
    REQUIRE(t.Frozen());
    REQUIRE(t.R().G->CellAt(0, 13).BgKind != grid::EColorKind::Default);
    t.Scripted(0, 8 + 3);
    const auto& g = *t.R().G;
    CHECK(t.Cal->ColumnFixed() == 1);
    CHECK(g.CursorCol() == 11);
    CHECK(g.KnownFrom(0) == 11);
    CHECK(g.CellAt(0, 3).Codepoint != 0);  // left of the answer: kept
    CHECK(g.CellAt(0, 7).Codepoint == U' ');
    CHECK(g.CellAt(0, 13).BgKind == grid::EColorKind::Default);
}
