// Inputs that give a width verdict the terminal contradicts, or rows the
// mirror calls known while the terminal shows something else. Where the model
// terminal of the rig (a grid with the mirror's own parser) cannot draw what a
// real one does, the answer is scripted and the comment names the terminal
// that gives it.
#include <doctest/doctest.h>

#include <string>

#include "calibration_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;

namespace {

using grid::EWidthClass;

// "Ж" (class Cyrillic).
const std::string ZHE = "\xd0\x96";
const std::string RU = "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82";
// U+E0B0, private use in the BMP (a Powerline glyph).
const std::string PL = "\xee\x82\xb0";
// U+F0001, private use in plane 15 (Nerd Fonts v3 moved the Material Design
// icons here, nf-md-*).
const std::string PL15 = "\xf3\xb0\x80\x81";

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

TEST_CASE(
    "Contradiction C4: a snapshot after a Wide verdict leaves rows below a "
    "wrapped row known and wrong") {
    // The terminal draws Cyrillic in two cells and the session knows it. A
    // snapshot paints a row of 12 Cyrillic glyphs in 20 columns with AWM on:
    // the terminal wraps it into two rows and everything below moves down one
    // row. The mirror lays the snapshot out with the default policy (12
    // cells, no wrap) and applies the verdict afterwards: it forgets row 0
    // only, and keeps row 1 ("abc") known where the terminal has "ЖЖ".
    CalClient t(20, 6);
    t.SetTerminal(Terminal(2, 1, false));
    Start(t);
    t.Agent("\r" + RU + "> ");
    REQUIRE(t.Cal->Verdict(EWidthClass::Cyrillic) == EWidthVerdict::Wide);
    std::string body = "\x1b[2J\x1b[H";
    for (int i = 0; i < 12; ++i) body += ZHE;
    body += "\r\nabc\r\n$ ";
    t.Snapshot(body);
    REQUIRE(t.C.Ref.G->CellAt(2, 0).Codepoint == U'a');
    CHECK(t.R().G->CursorRow() == t.C.Real.G->CursorRow());
    KnownAgree(t, 1);
    KnownAgree(t, 2);
}

TEST_CASE(
    "Contradiction C4: a truncated UTF-8 character before ESC gives a false "
    "Wide") {
    // Output cut by bytes in the middle of a character, then an SGR reset.
    // xterm, VTE and alacritty draw U+FFFD for the lone lead byte; the mirror's
    // parser drops it when ESC arrives and keeps the path solvable. Narrow
    // terminal: Ж + U+FFFD put the cursor at 2, the mirror expects 1, d = 1 =
    // the number of Cyrillic glyphs.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE + "\xd0\x1b[m");
    REQUIRE(t.Frozen());
    t.Scripted(0, 2);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Wide);
}

TEST_CASE(
    "Contradiction C4: a UTF-8 C1 control on the path gives a false verdict") {
    // VTE acts on C1 controls decoded from UTF-8; the mirror's parser drops
    // them and keeps the path solvable.
    SUBCASE("U+009B CSI on a narrow terminal: false Wide") {
        // Terminal: Ж (1) + CUF 3 = 4. Mirror: Ж + "3C" = 3. d = 1 = n.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + "\xc2\x9b" + "3C");
        REQUIRE(t.Frozen());
        t.Scripted(0, 4);
        CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Wide);
    }
    SUBCASE("U+009D OSC on a wide terminal: false Narrow") {
        // Terminal: Ж (2), "x" swallowed by the OSC that BEL ends = 2.
        // Mirror: Ж + "x" = 2. d = 0.
        CalClient t(40, 6);
        StartScripted(t);
        t.Agent("\r" + ZHE + "\xc2\x9d" + "x\x07");
        REQUIRE(t.Frozen());
        t.Scripted(0, 2);
        CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Narrow);
    }
}

TEST_CASE(
    "Contradiction C4: CSI with an intermediate is taken for CUF: false "
    "Narrow") {
    // `CSI 1 SP C` (GSS in ECMA-48) is unknown to xterm, which ignores it. The
    // mirror's dispatch ignores the intermediate and moves the cursor right,
    // so the sequence neither breaks the path nor matches the terminal. xterm
    // with cjkWidth: Ж (2) + "x" = 3. Mirror: Ж (1) + CUF 1 + "x" = 3. d = 0.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + ZHE + "\x1b[1 Cx");
    REQUIRE(t.Frozen());
    t.Scripted(0, 3);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Narrow);
}

TEST_CASE("Contradiction C4: DECLRMM margins move CR and give a false Wide") {
    // xterm: DECSET 69 enables left/right margins, DECSLRM sets the left one
    // to column 2, CR then goes to column 2 (index 1). The mirror does not
    // model margins: the path is not solvable while the mode is set, so there
    // is no query to answer with the terminal's 1 + Ж + "x" = 3 against the
    // mirror's 2 (d = 1 = n, a false Wide).
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\x1b[?69h\x1b[2;40s\x1b[1;10H\r" + ZHE + "x");
    CHECK_FALSE(t.Frozen());
    CHECK(t.Queries() == 1);
    CHECK(t.Cal->Verdict(EWidthClass::Cyrillic) != EWidthVerdict::Wide);
}

TEST_CASE(
    "Contradiction C4: a Wide verdict for BMP private use is taken for plane "
    "15 too") {
    // wezterm with cell_widths = {{first = 0xe000, last = 0xf8ff, width = 2}}
    // (or tmux codepoint-widths for the same range): BMP private use is two
    // cells, plane 15 stays one. The verdict from a BMP glyph closes the
    // whole class: the mirror lays U+F0001 out in two cells and holds the
    // column after it as certain.
    CalClient t(40, 6);
    StartScripted(t);
    t.Agent("\r" + PL + "> ");
    REQUIRE(t.Frozen());
    t.Scripted(0, 4);
    REQUIRE(t.Cal->Verdict(EWidthClass::PrivateUseBmp) == EWidthVerdict::Wide);
    t.Agent("\r\n" + PL15 + "x");
    const grid::Grid& g = *t.R().G;
    // The terminal's cursor is at column 2; the mirror's at 3. The column may
    // be wrong, but it must not be certain.
    CHECK(g.WidthDoubtBefore(g.CursorRow(), g.CursorCol()));
}
