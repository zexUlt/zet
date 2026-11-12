// Verification of the C4 fix round (PR #148): inputs that still give a width
// verdict the terminal contradicts or leave a row the mirror calls settled
// while the terminal laid it out otherwise, and pins for the fixes that hold.
// Where the model terminal of the rig (a grid) cannot draw what a real one
// does, the answer is scripted and the comment names the terminal.
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
const std::string MAN = "\xf0\x9f\x91\xa8";       // U+1F468
const std::string ZWJ = "\xe2\x80\x8d";           // U+200D
const std::string COMPUTER = "\xf0\x9f\x92\xbb";  // U+1F4BB
// U+1FAE9 FACE WITH BAGS UNDER EYES, new in Unicode 16: two cells in the
// mirror's table, one on a terminal whose table is older (VTE on glib before
// 2.84, iTerm2 set to Unicode 8 widths, Terminal.app on an older macOS).
const std::string FACE16 = "\xf0\x9f\xab\xa9";
// U+1FAEA, an emoji of Unicode 17: one cell in the mirror's table (16), two
// on a terminal with a newer one (kitty, wezterm, ghostty).
const std::string FACE17 = "\xf0\x9f\xab\xaa";
// U+1F680 ROCKET (Unicode 6): two cells on every terminal that has emoji.
const std::string ROCKET = "\xf0\x9f\x9a\x80";
// U+1FAFF, unassigned in Unicode 16 inside the emoji block of U+1FA70..1FAFF.
const std::string UNASSIGNED_EMOJI = "\xf0\x9f\xab\xbf";
// "1" VS16 U+20E3: the keycap one. Two cells on kitty, one here.
const std::string KEYCAP = "1\xef\xb8\x8f\xe2\x83\xa3";
// U+2601 VS15: the cloud in text presentation.
const std::string CLOUD15 = "\xe2\x98\x81\xef\xb8\x8e";

void StartScripted(CalClient& t) {
    t.Model.Reply = EReply::Never;
    t.Before("$ ");
    t.Begin();
    REQUIRE(t.Queries() == 1);
    t.Scripted(0, 2);
    REQUIRE_FALSE(t.Frozen());
}

bool NoVerdict(CalClient& t) {
    for (const EWidthClass cls :
         {EWidthClass::Ambiguous, EWidthClass::PrivateUse,
          EWidthClass::Emoji}) {
        const EWidthVerdict v = t.Cal->Verdict(cls);
        if (v == EWidthVerdict::Narrow || v == EWidthVerdict::Wide) {
            return false;
        }
    }
    return true;
}

}  // namespace

// --- Defects ---------------------------------------------------------------

TEST_CASE("Verify C4: REP after a soft hyphen gives no verdict") {
    // predict_fuzz found this (A and P wide, VS16 widening). xterm with
    // cjkWidth draws the arrow in two cells and the soft hyphen in one; REP
    // then repeats the soft hyphen, the last graphic character it printed.
    // The mirror drops the soft hyphen and repeats the arrow: a P glyph on
    // the path that the terminal never drew, one cell wide on both sides.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + PL + SHY);
    REQUIRE(t.Frozen());
    t.Scripted(0, 3);
    REQUIRE(t.Cal->ColumnFixed() == 1);
    t.Agent("\x1b[b");
    if (t.Frozen()) t.Scripted(0, 4);
    CHECK(t.Cal->Verdict(EWidthClass::PrivateUse) != EWidthVerdict::Narrow);
}

TEST_CASE("Verify C4: an emoji of another Unicode version than the mirror's") {
    SUBCASE("older terminal table hides wide A") {
        // VTE with ambiguous wide and an older glib: Ж takes two cells, the
        // new emoji one. The cursor ends where the mirror's does.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + FACE16);
        REQUIRE(t.R().G->CursorCol() == 3);
        if (t.Frozen()) t.Scripted(0, 3);
        CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) != EWidthVerdict::Narrow);
    }
    SUBCASE("newer terminal table fakes wide A") {
        // kitty with the default narrow A: Ж one cell, the Unicode 17 emoji
        // two, one more than the mirror's: read as Ж's.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + FACE17);
        REQUIRE(t.R().G->CursorCol() == 2);
        if (t.Frozen()) t.Scripted(0, 3);
        CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) != EWidthVerdict::Wide);
    }
}

TEST_CASE("Verify C4: an old emoji does not block the verdict") {
    // kitty with ambiguous wide: Ж two cells, the rocket two in the mirror
    // and on the terminal alike. The difference is Ж's.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE + ROCKET);
    REQUIRE(t.R().G->PathVolatile() == 0);
    REQUIRE(t.Frozen());
    t.Scripted(0, 4);
    CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Wide);
}

TEST_CASE("Verify C4: an unassigned emoji-block code point blocks") {
    // A terminal with a newer table draws it in two cells, the mirror in one.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE + UNASSIGNED_EMOJI);
    REQUIRE(t.R().G->PathVolatile() == 1);
    if (t.Frozen()) t.Scripted(0, 3);
    CHECK(NoVerdict(t));
}

TEST_CASE("Verify C4: a match does not settle a ZWJ the next piece completes") {
    // kitty: the man and the joiner arrive in one piece, the computer in the
    // next. Asked between them, kitty answers two columns, as the mirror
    // has. Then it glues the computer to the pair: still two columns, the
    // mirror four. The row must stay in doubt.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + MAN + ZWJ);
    REQUIRE(t.Frozen());
    t.Agent(COMPUTER);  // held for the wait
    t.Scripted(0, 2);
    REQUIRE(t.Cal->Matched() >= 1);
    REQUIRE(t.R().G->CursorCol() == 4);
    CHECK(t.R().G->WidthDoubtBefore(0, t.R().G->CursorCol()));
}

TEST_CASE("Verify C4: a soft hyphen at the cursor puts the cursor in doubt") {
    // xterm: "ab", then the soft hyphen in the third cell, the cursor in the
    // fourth; the mirror's cursor is in the third. Nothing may be predicted
    // there and the cluster is worth a query.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\rab" + SHY);
    REQUIRE(t.R().G->CursorCol() == 2);
    CHECK(t.R().G->WidthDoubtBefore(0, 2));
    CHECK(t.Frozen());
}

TEST_CASE("Verify C4: two clusters that cancel out leave their marks") {
    // A terminal that glues the ZWJ pair (two cells, not four) and draws each
    // soft hyphen in one cell (not none): the cursor ends where the mirror's
    // does, but the cells left of it are laid out otherwise.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + MAN + ZWJ + COMPUTER + SHY + SHY);
    REQUIRE(t.R().G->CursorCol() == 4);
    REQUIRE(t.Frozen());
    t.Scripted(0, 4);
    REQUIRE(t.Cal->Matched() >= 1);
    CHECK(t.R().G->WidthDoubtBefore(0, t.R().G->CursorCol()));
}

// --- Variants that hold ----------------------------------------------------

TEST_CASE("Verify C4: a ZWJ pair split mid-character gives no verdict") {
    // kitty with ambiguous wide; the joiner's bytes are cut in two pieces.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE + MAN + ZWJ.substr(0, 1));
    if (t.Frozen()) t.Scripted(0, 4);
    t.Agent(ZWJ.substr(1) + COMPUTER);
    if (t.Frozen()) t.Scripted(0, 4);
    CHECK(NoVerdict(t));
}

TEST_CASE("Verify C4: a cluster at the right edge gives no verdict") {
    // Ten columns: Ж, six letters, then the pair, which the mirror wraps.
    CalClient t(10, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE + "abcdef" + MAN + ZWJ + COMPUTER);
    // kitty with ambiguous wide: 2 + 6 + 2 cells, the cursor pending a wrap
    // in the last column of row 0.
    if (t.Frozen()) t.Scripted(0, 9);
    CHECK(NoVerdict(t));
}

TEST_CASE("Verify C4: keycap and VS15 keep a verdict away") {
    SUBCASE("keycap: kitty, ambiguous narrow, keycap two cells") {
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + KEYCAP);
        REQUIRE(t.Frozen());
        t.Scripted(0, 3);  // d = 1, one A glyph: Wide if read as A's
        CHECK(NoVerdict(t));
    }
    SUBCASE("VS15: a terminal drawing the text cloud wide anyway") {
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + CLOUD15);
        REQUIRE(t.Frozen());
        t.Scripted(0, 3);
        CHECK(NoVerdict(t));
    }
}

TEST_CASE(
    "Verify C4: an unknown sequence held in the wait breaks the next path") {
    // xterm with cjkWidth. The answer is about Ж alone; DECBI and the second
    // Ж come after it and move the terminal's cursor left by one.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE);
    REQUIRE(t.Frozen());
    t.Agent(
        "\x1b"
        "6" +
        ZHE);
    t.Scripted(0, 2);
    CHECK(t.Cal->Verdict(EWidthClass::Ambiguous) == EWidthVerdict::Wide);
    CHECK(t.R().G->PathBroken());
}

TEST_CASE("Verify C4: a resize while waiting reads no width from the answer") {
    // The terminal shrank before it answered: its cursor is clamped left of
    // the mirror's (d < 0), or anywhere else.
    for (const int col : {1, 3, 5}) {
        CAPTURE(col);
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + ZHE + ZHE);
        REQUIRE(t.Frozen());
        t.Resize(20, 6);
        if (t.Frozen()) t.Scripted(0, col);
        CHECK(NoVerdict(t));
    }
}

// --- Pins --------------------------------------------------------------------

TEST_CASE("Verify C4: SD moves a row's path epoch with its marks") {
    // Row 0 holds a cluster mark from an earlier path, row 1 one from the
    // current path; SD moves row 0 into row 1. The moved marks are not the
    // current path's.
    auto g = MakeGrid(10, 4);
    REQUIRE(g != nullptr);
    grid::VtParser p;
    const std::string bytes = "\ra" + std::string("\xef\xb8\x8e") +  // row 0
                              "\r\n"                                 // restart
                              "b" +
                              std::string("\xef\xb8\x8e") +  // row 1
                              "\x1b[A"                       // up, same path
                              "\x1b[T";                      // SD 1
    p.Feed(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size(), *g);
    REQUIRE(g->CursorRow() == 0);
    REQUIRE(g->WidthDoubtBefore(1, 2));
    CHECK_FALSE(g->MarksOnPath(1));
}
