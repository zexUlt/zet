// Width verdicts on the overlay rig (design.md §9, round C, C4): a terminal
// that draws the three glyph classes as its model says answers the query, and
// the mirror settles A, P and E from the difference of the columns.
#include <doctest/doctest.h>

#include <string>

#include "calibration_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;

namespace {

using grid::EWidthClass;

// "привет": six East Asian ambiguous glyphs (class A).
const std::string RU = "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
// U+E0B0, the powerline arrow (class P).
const std::string PL = "\xee\x82\xb0";
// U+2601 with VS16: a text glyph that has an emoji presentation (class E).
const std::string CLOUD = "\xe2\x98\x81\xef\xb8\x8f";
// "ж", typed.
const std::string ZH = "\xd0\xb6";
const std::string ZWJ = "\xe2\x80\x8d";

/// The shell's prompt is on row 0 when the session starts.
void Start(CalClient& t) {
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
}

/// Two echoed keys confirm an epoch.
void Confirm(CalClient& t) {
    t.Key("a");
    t.Agent("a");
    t.Ack();
    t.Key("b");
    t.Agent("b");
    t.Ack();
}

bool Open(const CalClient& t, EWidthClass cls) {
    return t.Cal->Verdict(cls) == EWidthVerdict::Unknown;
}

/// The mirror's cursor is where the terminal's is.
void SameCursor(CalClient& t) {
    CHECK(t.R().G->CursorRow() == t.C.Real.G->CursorRow());
    CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
}

}  // namespace

TEST_CASE("The glyphs the tests use are of the classes they are meant for") {
    CalClient t(40, 6);
    t.Agent("\r" + RU);
    CHECK(t.R().G->PathGlyphs(EWidthClass::Cyrillic) == 6);
    t.Agent("\r" + PL);
    CHECK(t.R().G->PathGlyphs(EWidthClass::PrivateUse) == 1);
    t.Agent("\r" + CLOUD);
    CHECK(t.R().G->PathGlyphs(EWidthClass::Emoji) == 1);
    t.Agent("\rx" + ZWJ);
    CHECK(t.R().G->PathGlyphs(EWidthClass::Cluster) == 1);
}

TEST_CASE("A is one cell: Cyrillic in the prompt gives Narrow in one query") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(1, 1, false));
    Start(t);
    // A row printed before the query keeps its mark until the verdict.
    t.Agent("\r\n" + RU + "\r\n");
    CHECK(t.Queries() == 1);
    CHECK(t.R().G->WidthDoubtClasses(1) != 0);
    t.Agent("\r" + RU + "> ");
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Narrow);
    CHECK(Open(t, EWidthClass::PrivateUse));
    CHECK(Open(t, EWidthClass::Emoji));
    CHECK(t.Cal->Matched() == 1);
    const grid::Grid& g = *t.R().G;
    // The marks of A are off every row, and the policy no longer doubts A.
    CHECK(g.WidthDoubtClasses(1) == 0);
    CHECK(g.WidthDoubtClasses(2) == 0);
    CHECK(g.Policy().Ambiguous == 1);
    CHECK((g.Policy().DoubtClasses &
           grid::WidthClassBit(EWidthClass::Cyrillic)) == 0);
    CHECK(g.KnownFrom(1) == 0);
    SameCursor(t);
    // Typing in Russian is predicted and drawn.
    Confirm(t);
    const auto row = g.CursorRow();
    const auto col = g.CursorCol();
    t.Key(ZH);
    REQUIRE(t.R().AnyVisible());
    CHECK(t.C.Real.G->CellAt(row, col).Codepoint == U'ж');
    CHECK(t.Queries() == 2);
    // The next Russian line asks nothing.
    t.Agent("\r\n" + RU + "> ");
    CHECK(t.Queries() == 2);
}

TEST_CASE(
    "A is two cells: Wide, rows with the mark turn foreign, the next is "
    "right") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.Agent("\r\n" + RU + "\r\n");
    CHECK(t.Queries() == 1);
    t.Agent("\r" + RU + "> ");
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Wide);
    const grid::Grid& g = *t.R().G;
    CHECK(g.Policy().Cyrillic == 2);
    CHECK(g.Policy().Ambiguous == 1);
    CHECK((g.Policy().DoubtClasses &
           grid::WidthClassBit(EWidthClass::Cyrillic)) == 0);
    // The row printed under the old width is the terminal's now.
    CHECK(g.KnownFrom(1) == 40);
    CHECK(g.WidthDoubtClasses(1) == 0);
    // The cursor row follows the terminal: its cursor, its blank tail.
    CHECK(t.Cal->ColumnFixed() == 1);
    CHECK(g.CursorRow() == 2);
    CHECK(g.CursorCol() == 14);
    CHECK(g.KnownFrom(2) == 14);
    CHECK(g.WidthDoubtClasses(2) == 0);
    SameCursor(t);
    // The line after it is laid out with two-cell glyphs, and nothing is asked.
    t.Agent("\r\n" + RU + "> ");
    CHECK(t.Queries() == 2);
    CHECK(g.CellAt(3, 0).Width == 2);
    CHECK(g.KnownFrom(3) == 0);
    SameCursor(t);
    // A typed two-cell glyph is not predicted, a letter is.
    Confirm(t);
    t.Key("c");
    CHECK(t.R().AnyVisible());
    t.Agent("c");
    t.Ack();
    t.Key(ZH);
    CHECK(t.R().Count() == 0);
}

TEST_CASE("A and P each get their verdict, whatever the terminal's modes") {
    for (const int a : {1, 2}) {
        for (const int p : {1, 2}) {
            CAPTURE(a);
            CAPTURE(p);
            CalClient t(40, 6);
            t.SetTerminal(Terminal(a, p, false));
            Start(t);
            t.Agent("\r" + RU + "> ");
            CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) ==
                  (a == 2 ? EWidthVerdict::Wide : EWidthVerdict::Narrow));
            CHECK(Open(t, EWidthClass::PrivateUse));
            SameCursor(t);
            t.Agent("\r" + PL + "> ");
            CHECK(t.Cal->Verdict(EWidthClass::PrivateUse) ==
                  (p == 2 ? EWidthVerdict::Wide : EWidthVerdict::Narrow));
            CHECK(t.Queries() == 3);
            SameCursor(t);
            // Both are known now: a line with both asks nothing and agrees.
            t.Agent("\r" + RU + PL + RU + "> ");
            CHECK(t.Queries() == 3);
            SameCursor(t);
        }
    }
}

TEST_CASE("E: VS16 gets no verdict and no query, whatever the terminal does") {
    for (const bool widens : {false, true}) {
        CAPTURE(widens);
        CalClient t(40, 6);
        t.SetTerminal(Terminal(1, 1, widens));
        Start(t);
        t.Agent("\r" + CLOUD + "> ");
        CHECK(t.Queries() == 1);
        CHECK(t.Cal->Verdict(EWidthClass::Emoji) == EWidthVerdict::Unknown);
        CHECK_FALSE(t.R().G->Policy().Vs16Widens);
    }
}

TEST_CASE("A path with two open classes and a difference gives no verdict") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.Agent("\r" + RU + PL + "> ");
    CHECK(t.Queries() == 2);
    CHECK(Open(t, EWidthClass::Cyrillic));
    CHECK(Open(t, EWidthClass::PrivateUse));
    // The column differs all the same: the row case, on the answer's column.
    CHECK(t.Cal->ColumnFixed() == 1);
    CHECK(t.R().G->CursorCol() == 15);
    CHECK(t.R().G->KnownFrom(0) == 15);
    SameCursor(t);
    // Without the other class the same terminal is solved.
    t.Agent("\r" + RU + "> ");
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Wide);
    SameCursor(t);
}

TEST_CASE("Three answers without a verdict make the class Unsolved for good") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    for (int i = 1; i <= CPR_SOLVE_FAILS_MAX; ++i) {
        CAPTURE(i);
        t.Agent("\r" + RU + PL + "> ");
        CHECK(t.Queries() == static_cast<std::size_t>(1 + i));
        const auto want = i == CPR_SOLVE_FAILS_MAX ? EWidthVerdict::Unsolved
                                                   : EWidthVerdict::Unknown;
        CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == want);
        CHECK(t.Cal->Verdict(EWidthClass::PrivateUse) == want);
    }
    // The classes keep the rules of step 5: still marked, never asked about.
    const std::size_t asked = t.Queries();
    t.Agent("\r" + RU + "> ");
    t.Agent("\r" + RU + PL + "> ");
    CHECK(t.Queries() == asked);
    CHECK(t.R().G->WidthDoubtBefore(0, t.R().G->CursorCol()));
    CHECK(t.R().G->Policy().Ambiguous == 1);
    // And Russian is not predicted.
    t.Agent("\r\n$ ");
    Confirm(t);
    t.Key(ZH);
    CHECK(t.R().Count() == 0);
}

TEST_CASE("A cluster mark left of the cursor is not asked about") {
    CalClient t(40, 6);
    Start(t);
    t.Agent("\rx" + ZWJ + "> ");
    CHECK(t.Queries() == 1);
    CHECK(t.R().G->WidthDoubtClasses(0) != 0);
    // The prompt is not predictable until a CR.
    Confirm(t);
    t.Key("c");
    CHECK_FALSE(t.R().AnyVisible());
}

TEST_CASE("A width mismatch on a row of marks: cursor, blank tail, KnownFrom") {
    // Case 2: a width-only query, the answer right of the mirror's column, the
    // path laid out on a row it started.
    CalClient t(40, 6);
    t.Model.Reply = EReply::Never;
    Start(t);
    t.Scripted(0, 2);
    t.Agent("\r" + ZH + "> ");
    REQUIRE(t.Frozen());
    // The terminal drew the glyph one cell wider.
    t.Scripted(0, 4 + 0);
    const grid::Grid& g = *t.R().G;
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Wide);
    CHECK(g.CursorRow() == 0);
    CHECK(g.CursorCol() == 4);
    CHECK(g.KnownFrom(0) == 4);
    for (int c = 4; c < 40; ++c) {
        CHECK(g.CellAt(0, static_cast<std::uint16_t>(c)).Codepoint == 0);
    }
}

TEST_CASE(
    "A column mismatch over a tail that is not blank turns the row foreign") {
    CalClient t(40, 6);
    t.Model.Reply = EReply::Never;
    Start(t);
    t.Scripted(0, 2);
    t.Agent("0123456789\r");
    CHECK(t.Queries() == 1);
    t.Agent("ab\x1b[99z");
    REQUIRE(t.Frozen());
    t.Scripted(0, 3);
    const grid::Grid& g = *t.R().G;
    CHECK(g.CursorCol() == 3);
    CHECK(g.KnownFrom(0) == 40);
    CHECK(g.WidthDoubtClasses(0) == 0);
    // Nothing is erased: what stands right of the cursor is only unknown.
    CHECK(g.CellAt(0, 5).Codepoint == U'3');
}
