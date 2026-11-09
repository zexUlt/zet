// The overlay against a terminal that is not a copy of the mirror (see
// overlay_rig.hpp): zet starts below the local shell's lines, so the mirror's
// rows are not the terminal's until an absolute row meets a cleared screen,
// and what REP repeats and how wide a glyph is are the terminal's own.
#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "overlay_rig.hpp"
#include "predict_rig.hpp"

using namespace zet::predict;
using namespace zet::predict::test;

namespace {

constexpr std::string_view SHELL =
    "local$ ls\r\nfile1 file2 file3\r\nlocal$ zet host\r\n";

/// A terminal whose cursor is on its bottom row, below `rows - 1` shell lines.
void FillToBottom(Client& c, int rows) {
    std::string s;
    for (int i = 1; i < rows; ++i)
        s += "shell line " + std::to_string(i) + "\r\n";
    c.Before(s);
}

}  // namespace

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

TEST_CASE("desync: with equal screens a draw is restored exactly (control)") {
    Client c(30, 6, EDisplayMode::Always);
    c.Start();
    c.Key("x");
    REQUIRE(c.Draws > 0);
    c.Agent("");
}

TEST_CASE("desync: typing under a shell's lines is drawn and restored") {
    Client c(30, 6, EDisplayMode::Always);
    c.Before(SHELL);
    c.Start();
    c.Key("x");
    CHECK(c.Draws > 0);
    c.Agent("x");
    c.Agent("");
}

TEST_CASE("desync: cursor-up past the first row loses the rows") {
    Client c(30, 6, EDisplayMode::Always);
    c.Before(SHELL);
    c.Start();
    c.Agent("\x1b[A\x1b[5G");
    CHECK_FALSE(c.R.G->RowsTrusted());
    const std::size_t drawsBefore = c.Draws;
    c.Key("x");
    CHECK(c.Draws == drawsBefore);
    c.Agent("");
}

TEST_CASE("desync: predictions are dropped when the rows are lost") {
    Client c(30, 6, EDisplayMode::Always);
    c.Before(SHELL);
    c.Start();
    c.Key("c");
    REQUIRE(c.R.Count() == 1);
    c.Agent("\x1b[A");  // the cursor stays on mirror row 0
    CHECK(c.R.Count() == 0);
    c.Key("x");  // and none is made while they are lost
    CHECK(c.R.Count() == 0);
}

TEST_CASE("desync: the overlay draws nothing on rows that are lost") {
    Client c(30, 6, EDisplayMode::Always);
    c.Before(SHELL);
    c.Start();
    c.Key("c");
    REQUIRE(c.O->Drawn());
    c.Undo();
    // The mirror alone is told, as if the engine had not looked yet.
    c.R.P.Feed(Bytes("\x1b[A").data(), 3, *c.R.G);
    REQUIRE_FALSE(c.R.G->RowsTrusted());
    CHECK(c.O->BuildUpdate(c.R.E, c.R.View()).empty());
}

TEST_CASE("desync: cursor-up inside the mirror keeps the rows") {
    Client c(30, 6, EDisplayMode::Always);
    c.Before(SHELL);
    c.Agent("one\r\ntwo\r\n$ ");
    c.Agent("\x1b[A");  // mirror row 2 -> 1: the terminal moves too
    CHECK(c.R.G->RowsTrusted());
    c.Agent("\x1b[B");
}

TEST_CASE("desync: reverse index, CPL and a scroll region lose the rows") {
    for (const char* seq : {"\x1bM", "\x1b[F", "\x1b[2;5r", "\x1b[S", "\x1b[T",
                            "\x1b[L", "\x1b[M", "\x1b[?6h"}) {
        CAPTURE(std::string_view{seq});
        Client c(30, 6, EDisplayMode::Always);
        c.Before(SHELL);
        c.Start();
        c.Agent(seq);
        CHECK_FALSE(c.R.G->RowsTrusted());
    }
}

TEST_CASE(
    "desync: cursor-down loses the rows, the terminal being at its bottom") {
    Client c(30, 6, EDisplayMode::Always);
    FillToBottom(c, 6);  // the terminal's cursor is on its last row
    c.Start();
    // The terminal's cursor stays on its last row, the mirror's goes down.
    c.Agent("\x1b[2B\x1b[3G");
    CHECK_FALSE(c.R.G->RowsTrusted());
    const std::size_t drawsBefore = c.Draws;
    c.Key("x");  // over the 'a' of the terminal's prompt, if drawn
    CHECK(c.Draws == drawsBefore);
    c.Agent("");
}

TEST_CASE("desync: line feeds keep the rows, at the bottom and short of it") {
    Client c(30, 6, EDisplayMode::Always);
    FillToBottom(c, 6);
    c.Start();
    // The terminal scrolls on every one while the mirror moves down, and then
    // both scroll: the difference between their rows only shrinks.
    c.Agent("\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n$ ");
    CHECK(c.R.G->RowsTrusted());
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Key("c");
    CHECK(c.Draws > 0);
    c.Agent("");
}

TEST_CASE("desync: a cursor position over a screen with the shell's lines") {
    Client c(30, 6, EDisplayMode::Always);
    c.Before(SHELL);
    c.Start();
    // An absolute row alone does not make the rows the terminal's: the
    // terminal's row 0 is the shell's line, not the mirror's.
    c.Agent("\x1b[2;1H");
    CHECK_FALSE(c.R.G->RowsTrusted());
    c.Agent("\x1b[2J\x1b[H");  // both screens are blank and at home
    CHECK(c.R.G->RowsTrusted());
}

TEST_CASE("desync: a position then a clear, in either order, is a sync") {
    for (const char* seq : {"\x1b[2J\x1b[H", "\x1b[H\x1b[2J", "\x1b[H\x1b[J",
                            "\x1b"
                            "c",
                            "\x1b[2J\x1b[3;3H"}) {
        CAPTURE(std::string_view{seq});
        Client c(30, 6, EDisplayMode::Always);
        c.Before(SHELL);
        c.Start();
        c.Agent("\x1b[A");  // lose them
        REQUIRE_FALSE(c.R.G->RowsTrusted());
        c.Agent(seq);
        CHECK(c.R.G->RowsTrusted());
        c.Agent("\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A\x1b[A");
        c.Agent("\r$ ");
    }
}

TEST_CASE("desync: the alternate screen leaves the rows as they were") {
    Client c(30, 6, EDisplayMode::Always);
    c.Before(SHELL);
    c.Start();
    c.Agent("\x1b[?1049h\x1b[3;3H\x1b[A\x1b[H\x1b[?1049l");
    CHECK(c.R.G->RowsTrusted());
    c.Agent("\x1b[A");
    CHECK_FALSE(c.R.G->RowsTrusted());
    c.Agent("\x1b[?1049h\x1b[2J\x1b[H\x1b[?1049l");
    CHECK_FALSE(c.R.G->RowsTrusted());
}

// ---------------------------------------------------------------------------
// Widths
// ---------------------------------------------------------------------------

namespace {

/// A prompt with `glyphs` in it, then the usual start, then a key typed.
std::size_t DrawsAfterGlyph(std::string_view glyphs) {
    Client c(30, 5, EDisplayMode::Always);
    c.Agent(std::string{"\r"} + std::string{glyphs} + " $ ");
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Key("c");
    c.Agent("");
    return c.Draws;
}

}  // namespace

TEST_CASE("width: a plain prompt is predicted (control)") {
    CHECK(DrawsAfterGlyph("ok") > 0);
}

TEST_CASE(
    "width: nothing is predicted after a glyph the width of which varies") {
    CHECK(DrawsAfterGlyph("\xe2\x98\x81\xef\xb8\x8f") == 0);  // cloud + VS16
    CHECK(DrawsAfterGlyph("\xe2\x9a\xa0\xef\xb8\x8f") == 0);  // warning + VS16
    CHECK(DrawsAfterGlyph("\xc2\xb1") == 0);                  // plus-minus
    CHECK(DrawsAfterGlyph("\xe2\x80\xa6") == 0);              // ellipsis
    CHECK(DrawsAfterGlyph("\xd0\x96") == 0);                  // Cyrillic Zhe
    CHECK(DrawsAfterGlyph("\xee\x82\xb0") == 0);              // powerline
    CHECK(DrawsAfterGlyph("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd") ==
          0);                                      // modifier
    CHECK(DrawsAfterGlyph("a\xe2\x80\x8d") == 0);  // ZWJ
}

TEST_CASE("width: a glyph to the right of the cursor does not matter") {
    Client c(30, 5, EDisplayMode::Always);
    c.Agent("\r$ ");
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Agent("\x1b[1;20H\xc2\xb1\x1b[1;5H");
    c.Key("c");
    c.Agent("");
}

TEST_CASE("width: a typed glyph of doubtful width is not predicted") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("\xc2\xb1");
    c.Key("\xd0\x96");
    c.Key("\xe2\x80\xa6");
    CHECK(c.R.Count() == 0);
    CHECK(c.Draws == 0);
}

namespace {

/// Bytes the mirror takes before the engine has looked at them: the layers
/// below the engine's observation each hold on their own.
void MirrorOnly(Client& c, std::string_view s) {
    c.R.P.Feed(Bytes(s).data(), s.size(), *c.R.G);
}

}  // namespace

TEST_CASE("width: the engine refuses a key on a row with a doubt left of it") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    MirrorOnly(c, "\x1b[1G\xc2\xb1\x1b[5G");
    c.R.Type("c");
    CHECK(c.R.Count() == 0);
}

TEST_CASE("width: the engine refuses a backspace on such a row") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    MirrorOnly(c, "\x1b[1G\xc2\xb1\x1b[5G");
    c.R.Type("\x7f");
    CHECK(c.R.Count() == 0);
}

TEST_CASE("width: the overlay draws nothing with a doubt left of the cursor") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");
    REQUIRE(c.O->Drawn());
    MirrorOnly(c, "\x1b[1G\xc2\xb1\x1b[5G");
    CHECK(c.O->BuildUpdate(c.R.E, c.R.View()).empty());
}

TEST_CASE("width: the overlay skips a cell with a doubt left of it") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");
    c.Key("d");  // predicted at columns 4 and 5
    c.Undo();
    // The doubt is at column 4, the cursor's: before the cell at 5, not
    // before the cursor.
    MirrorOnly(c, "\xc2\xb1\x1b[5G");
    const auto bytes = c.O->BuildUpdate(c.R.E, c.R.View());
    const std::string out{reinterpret_cast<const char*>(bytes.data()),
                          bytes.size()};
    CHECK(out.find('d') == std::string::npos);
}

TEST_CASE("width: the doubt goes with the row it is on") {
    Client c(30, 5, EDisplayMode::Always);
    c.Agent("\r\xc2\xb1 $ ");
    c.Agent("\r\n\r$ ");  // another row: predicted again
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Key("c");
    CHECK(c.Draws > 0);
    c.Agent("");
    c.Agent("\x1b[A\x1b[2K\r$ ");  // the row with the doubt is cleared
}

// ---------------------------------------------------------------------------
// REP
// ---------------------------------------------------------------------------

TEST_CASE("rep: after a draw the program's last glyph is what REP repeats") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");  // drawn: the terminal printed a 'c'
    REQUIRE(c.Draws > 0);
    // REP: 'B' three times on the terminal, not 'c'. The next restore compares
    // the two screens, and the character REP would repeat.
    c.Agent("\x1b[3b");
    c.Agent("");
}

TEST_CASE("rep: after the overlay is taken off") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("\x7f");
    c.Key("\x7f");  // erases the 'B' and the 'a': nothing printed to draw it
    REQUIRE(c.Draws > 0);
    // Taking it off prints the 'B' and then the 'a'; REP must still repeat
    // the 'B', the program's last.
    c.Agent("\x1b[2b");
    c.Agent("");
}

TEST_CASE("rep: taking off an erasure needs the program's glyph back") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("\x7f");  // erases the 'B': nothing printed to draw it
    REQUIRE(c.Draws > 0);
    // The program prints a glyph and a mark after it, so that its glyph cannot
    // be printed again, and returns to the column. Taking the erasure off
    // prints the 'B', which REP would then repeat: the erasure is not drawn.
    // (Found by predict_fuzz as a trap that left no input behind.)
    c.Agent("\xef\xbf\xbd\b\xcd\x9d");
    c.Agent("\x1b[2b");
    c.Agent("");
}

TEST_CASE("rep: after a cursor move on the row") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");
    c.Agent("\x1b[10G");
    c.Key("d");
    c.Agent("\x1b[2b");
    c.Agent("");
}

TEST_CASE("rep: with nothing printed in the mirror nothing is drawn") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");
    REQUIRE(c.O->Drawn());
    c.Undo();
    // The mirror is a fresh one, as after a snapshot of a screen that holds
    // text the parser has not printed: the cursor at column 4, no last glyph.
    c.R.G = MakeGrid(30, 5);
    MirrorOnly(c, "\x1b[5G");
    REQUIRE(c.R.E.ShowPredictions());
    CHECK(c.O->BuildUpdate(c.R.E, c.R.View()).empty());
}

namespace {

/// The start, a key typed (predicted, shown), then `bytes` from the program,
/// which leave the cursor where it was: whether the overlay draws the
/// prediction after them, when what REP repeats is what the program printed
/// last in them.
bool DrawnAfter(std::string_view bytes) {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();
    c.Key("c");  // at column 4
    const std::size_t before = c.Draws;
    c.Agent(bytes);
    REQUIRE(c.R.AnyVisible());  // the engine shows it in every case
    const bool drawn = c.Draws != before;
    c.Agent("\x1b[2b");  // what REP repeats is compared at this restore
    c.Agent("");
    return drawn;
}

}  // namespace

TEST_CASE("rep: a glyph the program printed last is not printed twice") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();  // the last printed is the 'B' at column 3
    const std::size_t before = c.Written;
    c.Key("B");  // the overlay's glyph is the program's
    REQUIRE(c.Draws > 0);
    // One 'B' is the prediction's; a reprint would be the second.
    CHECK(c.Written > before);
    CHECK(c.LastDrawn.find("B") == c.LastDrawn.rfind("B"));
}

TEST_CASE("rep: with the program's last glyph in reach it is drawn (control)") {
    CHECK(DrawnAfter("\x1b[1Gx\x1b[5G"));
}

TEST_CASE("rep: with the program's glyph followed by a mark nothing is drawn") {
    CHECK_FALSE(DrawnAfter("\x1b[1Ge\xcc\x81\x1b[5G"));
}

TEST_CASE("rep: with the program's glyph on another row nothing is drawn") {
    CHECK_FALSE(DrawnAfter("\r\nx\x1b[A\x1b[5G"));
}

TEST_CASE("rep: with the program's glyph erased nothing is drawn") {
    CHECK_FALSE(DrawnAfter("\x1b[1Gx\x1b[D\x1b[K\x1b[5G"));
}

TEST_CASE("rep: with the program's glyph moved nothing is drawn") {
    CHECK_FALSE(DrawnAfter("\x1b[1Gx\x1b[1G\x1b[@\x1b[5G"));
}

TEST_CASE(
    "rep: with the program's glyph under a predicted glyph nothing is drawn") {
    Client c(30, 5, EDisplayMode::Always);
    c.Start();  // the last printed is the 'B' at column 3
    c.Key("\x7f");
    const std::size_t before = c.Draws;
    c.Key("z");  // predicted over the 'B'
    REQUIRE(c.R.AnyVisible());
    CHECK(c.Draws == before);
    c.Agent("\x1b[2b");
    c.Agent("");
}

TEST_CASE("rep: before any glyph a REP is not modelled, the column is lost") {
    Client c(30, 5, EDisplayMode::Always);
    c.Agent("\r\x1b[2b");
    CHECK_FALSE(c.R.E.ColumnTrusted());
}
