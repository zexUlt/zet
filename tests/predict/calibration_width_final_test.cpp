// Whitelist check of the C4 width verdicts (PR #148): code points whose width
// the mirror's table gives one way and a real terminal another. None is a
// reference glyph or a target class, so none gives a verdict, and a soft
// hyphen at column 0. The rig's model
// terminal is a grid with the mirror's table, so the answers are scripted and
// each comment names the terminal.
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
// U+E0B0, the powerline arrow (class P).
const std::string PL = "\xee\x82\xb0";
// U+00AD SOFT HYPHEN: zero width in the mirror, one cell on xterm and VTE.
const std::string SHY = "\xc2\xad";
// U+2630 TRIGRAM FOR HEAVEN (Unicode 1.1): N up to Unicode 15.1, W in 16. Two
// cells in the mirror's table, one on VTE with glib before 2.84, xterm, tmux
// on glibc before 2.41.
const std::string TRIGRAM = "\xe2\x98\xb0";
// U+1F93B MODERN PENTATHLON (Unicode 9): W up to Unicode 12.1, N from 13.
// One cell in the mirror's table, two on tmux on glibc 2.31 (Ubuntu 20.04).
const std::string PENTATHLON = "\xf0\x9f\xa4\xbb";
// U+1100 U+1161: "가" decomposed (NFD, as in file names from macOS). The
// vowel U+1161 is one cell in the mirror's table, none on glibc, xterm and
// VTE, which draw the syllable in the two cells of the leading consonant.
const std::string GA_NFD = "\xe1\x84\x80\xe1\x85\xa1";
// U+304B U+3099: "が" decomposed. The voiced sound mark is a nonspacing mark
// of East Asian Width W: two cells in the mirror's table, none on glibc,
// xterm and VTE.
const std::string GA_KANA_NFD = "\xe3\x81\x8b\xe3\x82\x99";
// U+1F1F7, a regional indicator on its own: one cell in the mirror's table,
// two on kitty.
const std::string RI_R = "\xf0\x9f\x87\xb7";
// U+2764 VS16: the red heart, two cells on kitty.
const std::string HEART16 = "\xe2\x9d\xa4\xef\xb8\x8f";
// U+0938 U+093E: "सा", a consonant and a spacing vowel sign (Mc). Two cells
// in the mirror's table, one on kitty, which draws marks in no cell.
const std::string SAA = "\xe0\xa4\xb8\xe0\xa4\xbe";
// U+0600 ARABIC NUMBER SIGN (Cf): one cell in the mirror's table, none on
// xterm and VTE.
const std::string NUMBER_SIGN = "\xd8\x80";

bool NoVerdict(CalClient& t) {
    for (const EWidthClass cls :
         {EWidthClass::Cyrillic, EWidthClass::PrivateUse}) {
        const EWidthVerdict v = t.Cal->Verdict(cls);
        if (v == EWidthVerdict::Narrow || v == EWidthVerdict::Wide)
            return false;
    }
    return true;
}

void StartScripted(CalClient& t) {
    t.Model.Reply = EReply::Never;
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Scripted(0, 2);
    REQUIRE_FALSE(t.Frozen());
}

}  // namespace

TEST_CASE("Final C4: a code point whose East Asian Width changed after 9.0") {
    SUBCASE("narrow on an older table hides wide A") {
        // VTE with ambiguous wide on glib 2.80 (Ubuntu 24.04): Ж two cells,
        // the trigram one. The cursor ends where the mirror's does.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + TRIGRAM);
        REQUIRE(t.R().G->CursorCol() == 3);
        CHECK_FALSE(t.Frozen());
        CHECK(NoVerdict(t));
    }
    SUBCASE("narrow on an older table hides wide P") {
        // tmux with codepoint-widths giving the private use area two cells,
        // on glibc before 2.41.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + PL + TRIGRAM);
        REQUIRE(t.R().G->CursorCol() == 3);
        CHECK_FALSE(t.Frozen());
        CHECK(NoVerdict(t));
    }
    SUBCASE("wide on an older table fakes wide A") {
        // tmux on glibc 2.31, ambiguous narrow: Ж one cell, the pentathlon
        // two, one more than the mirror's.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + PENTATHLON);
        REQUIRE(t.R().G->CursorCol() == 2);
        CHECK_FALSE(t.Frozen());
        CHECK(NoVerdict(t));
    }
}

TEST_CASE("Final C4: a code point the terminals draw in another width") {
    SUBCASE("decomposed Hangul hides wide A") {
        // VTE with ambiguous wide (a Korean locale): Ж two cells, the
        // syllable two; the mirror one and three.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + GA_NFD);
        REQUIRE(t.R().G->CursorCol() == 4);
        CHECK_FALSE(t.Frozen());
        CHECK(NoVerdict(t));
    }
    SUBCASE("decomposed kana hides wide A") {
        // xterm with cjkWidth: Ж and Ж two cells each, "が" two; the mirror
        // one, one and four.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + ZHE + GA_KANA_NFD);
        REQUIRE(t.R().G->CursorCol() == 6);
        CHECK_FALSE(t.Frozen());
        CHECK(NoVerdict(t));
    }
    SUBCASE("a format character hides wide A") {
        // VTE with ambiguous wide: Ж two cells, the number sign none.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + NUMBER_SIGN);
        REQUIRE(t.R().G->CursorCol() == 2);
        CHECK_FALSE(t.Frozen());
        CHECK(NoVerdict(t));
    }
    SUBCASE("a lone regional indicator fakes wide A") {
        // kitty, ambiguous always narrow: Ж one cell, the indicator two.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + RI_R);
        REQUIRE(t.R().G->CursorCol() == 2);
        CHECK_FALSE(t.Frozen());
        CHECK(NoVerdict(t));
    }
    SUBCASE("a spacing mark hides wide E") {
        // kitty widens the heart with VS16 and draws the vowel sign in no
        // cell: 2 + 1 + 1 + 0, the mirror 1 + 1 + 1 + 1.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + HEART16 + " " + SAA);
        REQUIRE(t.R().G->CursorCol() == 4);
        CHECK_FALSE(t.Frozen());
        CHECK(NoVerdict(t));
    }
}

TEST_CASE("Final C4: a soft hyphen at column 0 puts the cursor in doubt") {
    // xterm: the soft hyphen in the first cell, the cursor in the second; the
    // mirror's cursor is in the first. Nothing may be predicted or drawn
    // there.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + SHY);
    REQUIRE(t.R().G->CursorCol() == 0);
    const grid::Grid& g = *t.R().G;
    CHECK((g.WidthDoubtBefore(0, 0) || g.ColumnDoubt()));
}

TEST_CASE("Final C4: a glyph without a verdict keeps the path from one") {
    // No query is asked, and no later piece of the same path gives a verdict.
    const std::string trigram = "\xe2\x98\xb0";
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Agent("\r" + ZHE + trigram + ZHE + "> ");
    CHECK(t.Queries() == 1);
    CHECK(NoVerdict(t));
    // The next absolute column makes the path solvable again.
    t.Agent("\r" + ZHE + "> ");
    CHECK(t.Queries() == 2);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Wide);
}

TEST_CASE("Final C4: a prompt with powerline glyphs and Cyrillic, in turn") {
    // p10k: CR, powerline glyphs and ASCII, then Cyrillic text. P is solved
    // from its path, Cyr from its own, each with reference glyphs only.
    CalClient t(40, 6);
    t.SetTerminal(Terminal(1, 1, false));
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Agent("\r" + PL + " ~ " + PL + " git " + PL + " ");
    CHECK(t.Cal->Verdict(EWidthClass::PrivateUse) == EWidthVerdict::Narrow);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Unknown);
    t.Agent("\r" + ZHE + ZHE + "> ");
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Narrow);
    CHECK(t.R().G->CursorCol() == t.C.Real.G->CursorCol());
}

TEST_CASE("Final C4: a lone Cyrillic glyph and P on one path give no verdict") {
    CalClient t(40, 6);
    t.SetTerminal(Terminal(2, 1, false));
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Agent("\r" + ZHE + PL + "> ");
    CHECK(NoVerdict(t));
}
