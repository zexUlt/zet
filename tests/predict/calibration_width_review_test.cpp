// Review of round C4 (PR #148): reachable inputs that give a width verdict the
// terminal contradicts, or that leave cells of a row known to the mirror and
// different from the terminal's. Where the model terminal of the rig (a grid)
// cannot draw what a real one does (grapheme clusters, DECBI, the soft
// hyphen), the answer is scripted and the comment says which terminal gives it.
#include <doctest/doctest.h>

#include <string>

#include "calibration_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;

namespace {

using grid::EWidthClass;

// "Ж" (class A).
const std::string ZHE = "\xd0\x96";
// U+1F468 ZWJ U+1F4BB, "technologist": one two-cell glyph on kitty, wezterm,
// iTerm2, Terminal.app, ghostty and Windows Terminal; 2 + 2 cells here.
const std::string TECH = "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb";
// U+00AD SOFT HYPHEN: East Asian ambiguous, zero width in the mirror's table,
// one cell on xterm (mk_wcwidth) and VTE.
const std::string SHY = "\xc2\xad";

void Start(CalClient& t) {
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
}

void StartScripted(CalClient& t) {
    t.Model.Reply = EReply::Never;
    Start(t);
    t.Scripted(0, 2);
    REQUIRE_FALSE(t.Frozen());
}

/// Every cell of `row` the mirror calls known is the terminal's.
void KnownAgree(CalClient& t, std::uint16_t row) {
    const grid::Grid& m = *t.R().G;
    const grid::Grid& ref = *t.C.Ref.G;
    for (std::uint16_t c = m.KnownFrom(row); c < m.Cols(); ++c) {
        CAPTURE(c);
        const char32_t a = m.CellAt(row, c).Codepoint;
        const char32_t b = ref.CellAt(row, c).Codepoint;
        CHECK((a == b || ((a == 0 || a == U' ') && (b == 0 || b == U' '))));
    }
}

}  // namespace

TEST_CASE("Review C4: a ZWJ sequence drawn as one glyph hides wide A") {
    // kitty or wezterm with ambiguous wide: the pair is 2 cells, not 4, and
    // the two Ж take 4 cells, not 2. The column comes out where the mirror's
    // is.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + TECH + ZHE + ZHE);
    REQUIRE(t.R().G->CursorCol() == 6);
    // A cluster is no reference glyph: no query, no verdict.
    CHECK_FALSE(t.Frozen());
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unknown);
}

TEST_CASE("Review C4: a matched answer after an unknown ESC leaves the path") {
    // xterm with cjkWidth: Ж is two cells, DECBI (ESC 6) moves the cursor one
    // column left. The mirror does not know DECBI: it counts it as unknown,
    // but the path stays whole.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE +
            "\x1b"
            "6");
    REQUIRE(t.Frozen());
    // The query for the unknown sequence: the terminal is at 0 + 2 - 1 = 1,
    // where the mirror is.
    t.Scripted(0, 1);
    CHECK(t.Cal->Matched() == 1);
    // The width reason is still due on the same path and asks again at once.
    if (t.Frozen()) t.Scripted(0, 1);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Narrow);
}

TEST_CASE("Review C4: REP after a resync alone gives no verdict (guarded)") {
    // The author's hole: the mirror's REP register is empty after Resync, the
    // terminal repeats its own last glyph (the space of "$ "). The unknown REP
    // makes the query's reasons more than widths, so the column is not read as
    // widths, and the column fix restarts the path.
    CalClient t(40, 6);
    Start(t);
    t.Agent("\x1b[b" + ZHE);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unknown);
    CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
    t.Agent("\r" + ZHE + "> ");
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Narrow);
}

TEST_CASE("Review C4: a Wide verdict keeps the row known where it is not") {
    // The row was laid out under the old width before the path began: the
    // terminal has "x" at column 8, the mirror had it at 4 and overwrote it.
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.Agent("\r\n" + ZHE + ZHE + ZHE + ZHE + "x\r");
    REQUIRE(t.Queries() == 1);
    t.Agent("abcde" + ZHE);
    REQUIRE(t.Queries() == 2);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Wide);
    CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
    REQUIRE(t.C.Ref.G->CellAt(1, 8).Codepoint == U'x');
    KnownAgree(t, 1);
}

TEST_CASE("Review C4: a column left of the expected one erases nothing known") {
    SUBCASE("an unknown sequence moved the cursor left (xterm DECBI)") {
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent(
            "\rabcd\x1b"
            "6");
        REQUIRE(t.Frozen());
        // The terminal shows "abcd" with its cursor on the "d".
        t.Scripted(0, 3);
        const grid::Grid& g = *t.R().G;
        CHECK(g.CursorCol() == 3);
        CHECK((g.KnownFrom(0) > 3 || g.CellAt(0, 3).Codepoint == U'd'));
    }
    SUBCASE("a ZWJ sequence drawn as one glyph over a longer line (kitty)") {
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r\nabcdefg\r");
        REQUIRE_FALSE(t.Frozen());
        t.Agent(TECH + "xyz");
        // kitty: the pair at 0-1, "xyz" at 2-4, cursor 5. No query is asked:
        // the cursor stays in doubt.
        CHECK_FALSE(t.Frozen());
        const grid::Grid& g = *t.R().G;
        CHECK(g.WidthDoubtBefore(1, g.CursorCol()));
    }
}

TEST_CASE("Review C4: a match does not vouch for a cluster before the path") {
    // The cluster was laid out on an earlier path; the query is about a path
    // that only moved right over it (CR, CUF), as zsh does coming back from
    // RPROMPT. A terminal that draws the pair in 2 cells answers the same
    // column, its "xyz" is at 2-4 and the mirror's at 4-6.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r\n" + TECH + "xyz\r");
    REQUIRE_FALSE(t.Frozen());
    t.Agent("\x1b[10Cq");
    // No cluster on this path: there is nothing to ask. If asked, the terminal
    // answers the mirror's column.
    if (t.Frozen()) t.Scripted(1, 11);
    const grid::Grid& g = *t.R().G;
    const bool clusterMarked = (g.WidthDoubtClasses(1) &
                                grid::WidthClassBit(EWidthClass::Cluster)) != 0;
    CHECK((clusterMarked || g.KnownFrom(1) > 6));
}

TEST_CASE("Review C4: a soft hyphen the terminal draws gives no Wide") {
    // xterm without cjkWidth: Ж is one cell, U+00AD one cell (mk_wcwidth),
    // the mirror's table gives the hyphen none and does not count it.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE + SHY);
    REQUIRE(t.R().G->CursorCol() == 1);
    CHECK_FALSE(t.Frozen());
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unknown);
}

TEST_CASE(
    "Review C4: a cluster in the prompt keeps A from a verdict to the CR") {
    // A prompt with a ZWJ sequence, then a typed Ж on the same line: no
    // query and no verdict on this path. The next line is solvable.
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.Agent("\r" + TECH + "> ");
    CHECK(t.Queries() == 1);
    t.Agent(ZHE);
    CHECK(t.Queries() == 1);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unknown);
    t.Agent("\r" + ZHE + "> ");
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Wide);
    CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
}
