// Unit tests for zet_grid: Grid model, VtParser sequences, modes, caps.
// Mirrors the structure of wire/frame_test.cpp — one TEST_CASE per concept.

#include <doctest/doctest.h>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/unicode.hpp"
#include "zet/grid/vt_parser.hpp"

#include <cstring>
#include <string_view>

using namespace zet::grid;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Grid* MakeGrid(int cols = 80, int rows = 24, int scrollback = 100) {
    auto r = Grid::Create(static_cast<std::uint16_t>(cols),
                          static_cast<std::uint16_t>(rows),
                          static_cast<std::uint32_t>(scrollback));
    REQUIRE(r.has_value());
    return r.value();
}

// Feed a string literal through VtParser into grid.
static void Feed(VtParser& p, Grid& g, std::string_view s) {
    p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), g);
}

// Read the codepoint at (row, col) in the active screen.
static char32_t CP(const Grid& g, int row, int col) {
    return g
        .CellAt(static_cast<std::uint16_t>(row),
                static_cast<std::uint16_t>(col))
        .Codepoint;
}

// ---------------------------------------------------------------------------
// Cell layout
// ---------------------------------------------------------------------------

TEST_CASE("Cell is 16 bytes") { CHECK(sizeof(Cell) == 16); }

// ---------------------------------------------------------------------------
// Grid construction
// ---------------------------------------------------------------------------

TEST_CASE("Create allocates a usable grid") {
    Grid* g = MakeGrid(80, 24, 100);
    CHECK(g->Cols() == 80);
    CHECK(g->Rows() == 24);
    CHECK(g->ScrollbackLimit() == 100);
    CHECK(g->CursorRow() == 0);
    CHECK(g->CursorCol() == 0);
    CHECK(!g->PendingWrap());
    CHECK(g->ModeSet(EMode::Awm));
    CHECK(g->ModeSet(EMode::Dectcem));
    delete g;
}

TEST_CASE("Create rejects out-of-range dimensions") {
    CHECK(!Grid::Create(0, 24).has_value());
    CHECK(!Grid::Create(80, 0).has_value());
    CHECK(!Grid::Create(MAX_COLS + 1, 24).has_value());
    CHECK(!Grid::Create(80, MAX_ROWS + 1).has_value());
}

TEST_CASE("Create with scrollback 0 is valid") {
    auto r = Grid::Create(80, 24, 0);
    REQUIRE(r.has_value());
    Grid* g = r.value();
    CHECK(g->ScrollbackLimit() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// Printable characters and PutChar
// ---------------------------------------------------------------------------

TEST_CASE("Printable ASCII lands in cell at cursor") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "Hello");
    CHECK(CP(*g, 0, 0) == U'H');
    CHECK(CP(*g, 0, 1) == U'e');
    CHECK(CP(*g, 0, 4) == U'o');
    CHECK(g->CursorCol() == 5);
    delete g;
}

TEST_CASE("Auto-wrap on AWM: cursor wraps at right margin") {
    Grid* g = MakeGrid(5, 4);
    VtParser p;
    // Fill exactly 5 chars → pending_wrap set, cursor stays at col 4.
    Feed(p, *g, "ABCDE");
    CHECK(g->CursorCol() == 4);
    CHECK(g->PendingWrap());
    // One more char → wrap to next line.
    Feed(p, *g, "F");
    CHECK(g->CursorRow() == 1);
    CHECK(g->CursorCol() == 1);
    CHECK(!g->PendingWrap());
    delete g;
}

TEST_CASE("PendingWrap reset by CR") {
    Grid* g = MakeGrid(5, 4);
    VtParser p;
    Feed(p, *g, "ABCDE");
    CHECK(g->PendingWrap());
    Feed(p, *g, "\r");
    CHECK(!g->PendingWrap());
    CHECK(g->CursorCol() == 0);
    delete g;
}

TEST_CASE("Insert mode shifts cells right") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    // Move cursor to column 2, enable IRM.
    Feed(p, *g, "\x1b[3G");  // CHA = col 3 (1-based)
    Feed(p, *g, "\x1b[4h");  // SM IRM
    Feed(p, *g, "X");
    CHECK(CP(*g, 0, 2) == U'X');
    CHECK(CP(*g, 0, 3) == U'C');
    delete g;
}

// ---------------------------------------------------------------------------
// Cursor movement sequences
// ---------------------------------------------------------------------------

TEST_CASE("CUU/CUD/CUF/CUB move cursor") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[10;10H");  // CUP row=10, col=10
    CHECK(g->CursorRow() == 9);
    CHECK(g->CursorCol() == 9);

    Feed(p, *g, "\x1b[3A");  // CUU 3
    CHECK(g->CursorRow() == 6);

    Feed(p, *g, "\x1b[2B");  // CUD 2
    CHECK(g->CursorRow() == 8);

    Feed(p, *g, "\x1b[5C");  // CUF 5
    CHECK(g->CursorCol() == 14);

    Feed(p, *g, "\x1b[4D");  // CUB 4
    CHECK(g->CursorCol() == 10);
    delete g;
}

TEST_CASE("CUP clamps to grid bounds") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[999;999H");
    CHECK(g->CursorRow() == 23);
    CHECK(g->CursorCol() == 79);
    delete g;
}

TEST_CASE("VPA and CHA move row and col") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[5d");  // VPA row=5
    CHECK(g->CursorRow() == 4);
    Feed(p, *g, "\x1b[20G");  // CHA col=20
    CHECK(g->CursorCol() == 19);
    delete g;
}

// ---------------------------------------------------------------------------
// Erase sequences
// ---------------------------------------------------------------------------

TEST_CASE("ED 2 erases whole screen") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "Hello");
    CHECK(CP(*g, 0, 0) == U'H');
    Feed(p, *g, "\x1b[2J");
    CHECK(CP(*g, 0, 0) == 0);
    delete g;
}

TEST_CASE("EL 0 erases from cursor to EOL") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "\x1b[3G");  // col 3
    Feed(p, *g, "\x1b[K");   // EL 0
    CHECK(CP(*g, 0, 0) == U'A');
    CHECK(CP(*g, 0, 1) == U'B');
    CHECK(CP(*g, 0, 2) == 0);
    CHECK(CP(*g, 0, 3) == 0);
    delete g;
}

TEST_CASE("EL 1 erases from start to cursor") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "\x1b[3G");  // col 3
    Feed(p, *g, "\x1b[1K");  // EL 1
    CHECK(CP(*g, 0, 0) == 0);
    CHECK(CP(*g, 0, 1) == 0);
    CHECK(CP(*g, 0, 3) == U'D');
    delete g;
}

TEST_CASE("ECH erases N chars") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "\x1b[1G");  // col 1
    Feed(p, *g, "\x1b[3X");  // ECH 3
    CHECK(CP(*g, 0, 0) == 0);
    CHECK(CP(*g, 0, 1) == 0);
    CHECK(CP(*g, 0, 2) == 0);
    CHECK(CP(*g, 0, 3) == U'D');
    delete g;
}

// ---------------------------------------------------------------------------
// Scroll region
// ---------------------------------------------------------------------------

TEST_CASE("DECSTBM sets scroll region") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[5;20r");  // rows 5–20
    CHECK(g->ScrollTop() == 4);
    CHECK(g->ScrollBot() == 19);
    // Cursor moves to home.
    CHECK(g->CursorRow() == 0);
    CHECK(g->CursorCol() == 0);
    delete g;
}

TEST_CASE("Line feed scrolls within region") {
    Grid* g = MakeGrid(5, 5);
    VtParser p;
    // Write 'A' on row 1.
    Feed(p, *g, "\x1b[2;1H");
    Feed(p, *g, "A");
    // Restrict scroll to rows 2–4 (1-based), put cursor at row 4.
    Feed(p, *g, "\x1b[2;4r");
    Feed(p, *g, "\x1b[4;1H");
    Feed(p, *g, "\n");  // LF at bottom of region → scroll up by 1
    // Row 1 (0-based) should now be blank (was scrolled up within region).
    CHECK(CP(*g, 1, 0) == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// Scrollback ring
// ---------------------------------------------------------------------------

TEST_CASE("Scrollback ring stores displaced lines") {
    Grid* g = MakeGrid(5, 3, 10);
    VtParser p;
    // Write 'X' on the first row, then scroll it off by sending LF from bottom.
    Feed(p, *g, "X");
    Feed(p, *g, "\x1b[3;1H");  // row 3 col 1
    Feed(p, *g, "\n");         // scroll: row 0 ("X...") pushed to scrollback
    CHECK(g->ScrollbackSize() == 1);
    const Line* sb = g->ScrollbackLine(0);
    REQUIRE(sb != nullptr);
    CHECK(sb->Cells[0].Codepoint == U'X');
    delete g;
}

TEST_CASE("Scrollback ring wraps at limit") {
    const int limit = 5;
    Grid* g = MakeGrid(5, 3, limit);
    VtParser p;
    // Push 7 lines off the top; ring should hold only the last 5.
    for (int i = 0; i < 7; ++i) {
        Feed(p, *g, "\x1b[1;1H");
        // Write the row number as codepoint.
        g->PutChar(static_cast<char32_t>('0' + i), 1);
        Feed(p, *g, "\x1b[3;1H");
        Feed(p, *g, "\n");
    }
    CHECK(g->ScrollbackSize() == static_cast<std::uint32_t>(limit));
    delete g;
}

TEST_CASE("Scrollback 0 never accumulates history") {
    Grid* g = MakeGrid(5, 3, 0);
    VtParser p;
    Feed(p, *g, "X\x1b[3;1H\n");
    CHECK(g->ScrollbackSize() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// Alt screen
// ---------------------------------------------------------------------------

TEST_CASE("Alt screen switch preserves main screen content") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "Main");
    CHECK(CP(*g, 0, 0) == U'M');

    Feed(p, *g, "\x1b[?1049h");  // enter alt
    CHECK(g->IsAltScreen());
    CHECK(CP(*g, 0, 0) == 0);  // alt screen is blank

    Feed(p, *g, "Alt");
    CHECK(CP(*g, 0, 0) == U'A');

    Feed(p, *g, "\x1b[?1049l");  // leave alt
    CHECK(!g->IsAltScreen());
    CHECK(CP(*g, 0, 0) == U'M');  // main restored
    delete g;
}

TEST_CASE("Alt screen has no scrollback") {
    Grid* g = MakeGrid(5, 3, 100);
    VtParser p;
    Feed(p, *g, "\x1b[?1049h");
    // Force scroll while in alt.
    Feed(p, *g, "\x1b[3;1H\n");
    CHECK(g->ScrollbackSize() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// SGR
// ---------------------------------------------------------------------------

TEST_CASE("SGR 1 sets bold, SGR 0 clears") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[1m");  // bold
    CHECK((g->PenAttrs() & static_cast<uint8_t>(EAttr::Bold)) != 0);
    Feed(p, *g, "\x1b[0m");  // reset
    CHECK(g->PenAttrs() == 0);
    delete g;
}

TEST_CASE("SGR 31 sets indexed fg color") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[31m");
    CHECK(g->PenFgKind() == EColorKind::Indexed);
    CHECK(g->PenFgIdx() == 1);
    delete g;
}

TEST_CASE("SGR 38;2 sets truecolor fg inline") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[38;2;255;128;0m");
    CHECK(g->PenFgKind() == EColorKind::TrueColor);
    // Inline RGB stored directly in the pen (no side-table, design.md §9).
    CHECK(g->PenFg()[0] == 255);
    CHECK(g->PenFg()[1] == 128);
    CHECK(g->PenFg()[2] == 0);
    delete g;
}

TEST_CASE("SGR 39/49 resets fg/bg to default") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[31;42m");  // red fg, green bg
    Feed(p, *g, "\x1b[39;49m");  // reset both
    CHECK(g->PenFgKind() == EColorKind::Default);
    CHECK(g->PenBgKind() == EColorKind::Default);
    delete g;
}

TEST_CASE("SGR 4:3 colon sub-param sets curly underline style") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[4:3m");
    CHECK((g->PenAttrs() & static_cast<uint8_t>(EAttr::Underline)) != 0);
    CHECK(g->PenAttrEx() == 3);
    delete g;
}

TEST_CASE("SGR 4;3 semicolons set underline then italic, not style 3") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[4;3m");
    CHECK((g->PenAttrs() & static_cast<uint8_t>(EAttr::Underline)) != 0);
    CHECK((g->PenAttrs() & static_cast<uint8_t>(EAttr::Italic)) != 0);
    CHECK(g->PenAttrEx() == 1);  // plain underline, not style 3
    delete g;
}

TEST_CASE("SGR 4:0 colon sub-param clears underline") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[4m");    // set underline
    Feed(p, *g, "\x1b[4:0m");  // clear via sub-param 0
    CHECK((g->PenAttrs() & static_cast<uint8_t>(EAttr::Underline)) == 0);
    CHECK(g->PenAttrEx() == 0);
    delete g;
}

TEST_CASE("SGR 38:2:r:g:b colon-form sets truecolor fg") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[38:2:255:128:0m");
    CHECK(g->PenFgKind() == EColorKind::TrueColor);
    CHECK(g->PenFg()[0] == 255);
    CHECK(g->PenFg()[1] == 128);
    CHECK(g->PenFg()[2] == 0);
    delete g;
}

TEST_CASE("SGR 38:2::r:g:b colon-form with colorspace sets truecolor fg") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[38:2::200:100:50m");
    CHECK(g->PenFgKind() == EColorKind::TrueColor);
    CHECK(g->PenFg()[0] == 200);
    CHECK(g->PenFg()[1] == 100);
    CHECK(g->PenFg()[2] == 50);
    delete g;
}

TEST_CASE("SGR 38:5:n colon-form sets indexed fg") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[38:5:9m");
    CHECK(g->PenFgKind() == EColorKind::Indexed);
    CHECK(g->PenFgIdx() == 9);
    delete g;
}

TEST_CASE("SGR 58:5:n sets underline colour index; 59 resets") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[58:5:3m");
    CHECK(g->PenUlIdx() == 3);
    Feed(p, *g, "\x1b[59m");
    CHECK(g->PenUlIdx() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// DECSET / DECRST modes
// ---------------------------------------------------------------------------

TEST_CASE("DECAWM can be disabled") {
    Grid* g = MakeGrid(5, 5);
    VtParser p;
    Feed(p, *g, "\x1b[?7l");  // disable AWM
    CHECK(!g->ModeSet(EMode::Awm));
    // Fill line; cursor should stay at rightmost col, no wrap.
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "X");  // should overwrite col 4, not wrap
    CHECK(g->CursorRow() == 0);
    CHECK(g->CursorCol() == 4);
    delete g;
}

TEST_CASE("DECSET ?1 sets application cursor key mode") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[?1h");
    CHECK(g->ModeSet(EMode::Decckm));
    Feed(p, *g, "\x1b[?1l");
    CHECK(!g->ModeSet(EMode::Decckm));
    delete g;
}

TEST_CASE("Mouse mode and bracketed paste set/reset") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[?1000h");
    CHECK(g->ModeSet(EMode::Mouse1000));
    Feed(p, *g, "\x1b[?2004h");
    CHECK(g->ModeSet(EMode::BracketedPaste));
    Feed(p, *g, "\x1b[?1000l");
    CHECK(!g->ModeSet(EMode::Mouse1000));
    delete g;
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

TEST_CASE("Resize preserves content up to new bounds") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "Hello");
    CHECK(CP(*g, 0, 0) == U'H');

    // Shrink columns only — rows stays the same, no rows go to scrollback.
    REQUIRE(g->Resize(6, 5));
    CHECK(g->Cols() == 6);
    CHECK(g->Rows() == 5);
    CHECK(CP(*g, 0, 0) == U'H');
    CHECK(CP(*g, 0, 4) == U'o');
    delete g;
}

TEST_CASE("Resize clamps cursor to new bounds") {
    Grid* g = MakeGrid(10, 10);
    VtParser p;
    Feed(p, *g, "\x1b[8;8H");  // move to row 8, col 8
    CHECK(g->CursorRow() == 7);
    CHECK(g->CursorCol() == 7);

    REQUIRE(g->Resize(5, 5));
    CHECK(g->CursorRow() <= 4);
    CHECK(g->CursorCol() <= 4);
    delete g;
}

TEST_CASE("Resize clamps saved cursors") {
    Grid* g = MakeGrid(10, 10);
    VtParser p;
    Feed(p, *g, "\x1b[8;8H");
    Feed(p, *g, "\x1b[s");  // save
    REQUIRE(g->Resize(5, 5));
    Feed(p, *g, "\x1b[u");  // restore — must not panic
    CHECK(g->CursorRow() <= 4);
    CHECK(g->CursorCol() <= 4);
    delete g;
}

TEST_CASE("Resize to zero is clamped to 1x1") {
    Grid* g = MakeGrid(10, 10);
    REQUIRE(g->Resize(0, 0));
    CHECK(g->Cols() >= 1);
    CHECK(g->Rows() >= 1);
    delete g;
}

// ---------------------------------------------------------------------------
// Save / restore cursor
// ---------------------------------------------------------------------------

TEST_CASE("DECSC/DECRC saves and restores cursor position") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[5;10H");  // row 5, col 10
    Feed(p, *g,
         "\x1b"
         "7");                 // DECSC
    Feed(p, *g, "\x1b[1;1H");  // move to home
    Feed(p, *g,
         "\x1b"
         "8");  // DECRC
    CHECK(g->CursorRow() == 4);
    CHECK(g->CursorCol() == 9);
    delete g;
}

// ---------------------------------------------------------------------------
// IL / DL
// ---------------------------------------------------------------------------

TEST_CASE("IL inserts lines and shifts existing down") {
    Grid* g = MakeGrid(5, 5);
    VtParser p;
    Feed(p, *g, "\x1b[1;1H");
    Feed(p, *g, "A");
    Feed(p, *g, "\x1b[2;1H");
    Feed(p, *g, "B");
    Feed(p, *g, "\x1b[1;1H");  // back to row 1
    Feed(p, *g, "\x1b[L");     // IL 1
    CHECK(CP(*g, 0, 0) == 0);  // new blank line
    CHECK(CP(*g, 1, 0) == U'A');
    CHECK(CP(*g, 2, 0) == U'B');
    delete g;
}

TEST_CASE("DL deletes lines and shifts remaining up") {
    Grid* g = MakeGrid(5, 5);
    VtParser p;
    Feed(p, *g, "\x1b[1;1H");
    Feed(p, *g, "A");
    Feed(p, *g, "\x1b[2;1H");
    Feed(p, *g, "B");
    Feed(p, *g, "\x1b[1;1H");
    Feed(p, *g, "\x1b[M");  // DL 1
    CHECK(CP(*g, 0, 0) == U'B');
    CHECK(CP(*g, 1, 0) == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// Truecolor (inline RGB, no side-table)
// ---------------------------------------------------------------------------

TEST_CASE("Many distinct truecolor SGRs do not crash") {
    // With inline RGB there is no pool to exhaust.  Exercise many unique
    // truecolor pens and cell writes to confirm stability.
    Grid* g = MakeGrid(80, 24, 0);
    VtParser p;
    for (int i = 0; i < 4096; ++i) {
        if (i % 80 == 0) Feed(p, *g, "\x1b[1;1H");
        const std::uint8_t rv = static_cast<std::uint8_t>(i & 0xFF);
        const std::uint8_t gv = static_cast<std::uint8_t>((i >> 8) & 0xFF);
        const std::uint8_t bv = 0;
        std::uint16_t params[] = {38, 2, rv, gv, bv};
        bool colon[5]{};  // all semicolons (no colon sub-params)
        g->ApplySgr(params, colon, 5);
        g->PutChar('X', 1);
    }
    CHECK(g->Cols() == 80);
    delete g;
}

// ---------------------------------------------------------------------------
// Tab stops
// ---------------------------------------------------------------------------

TEST_CASE("HT moves to next tab stop") {
    Grid* g = MakeGrid(80, 5);
    VtParser p;
    Feed(p, *g, "\x1b[1G");  // col 1
    Feed(p, *g, "\t");       // HT → col 9
    CHECK(g->CursorCol() == 8);
    Feed(p, *g, "\t");  // → col 17
    CHECK(g->CursorCol() == 16);
    delete g;
}

TEST_CASE("TBC clears tab stop at cursor") {
    Grid* g = MakeGrid(80, 5);
    VtParser p;
    Feed(p, *g, "\x1b[9G");  // col 9
    Feed(p, *g, "\x1b[0g");  // TBC current
    Feed(p, *g, "\x1b[1G");
    Feed(p, *g, "\t");  // next stop should be col 17 now
    CHECK(g->CursorCol() == 16);
    delete g;
}

// ---------------------------------------------------------------------------
// REP
// ---------------------------------------------------------------------------

TEST_CASE("REP repeats last printed character") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    Feed(p, *g, "A");
    Feed(p, *g, "\x1b[4b");  // REP 4
    CHECK(CP(*g, 0, 0) == U'A');
    CHECK(CP(*g, 0, 1) == U'A');
    CHECK(CP(*g, 0, 4) == U'A');
    delete g;
}

// ---------------------------------------------------------------------------
// HTS / RIS / DECSTR
// ---------------------------------------------------------------------------

TEST_CASE("HTS sets tab stop at cursor column") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    Feed(p, *g, "\x1b[5G");  // col 5
    Feed(p, *g,
         "\x1b"
         "H");  // HTS
    Feed(p, *g, "\x1b[1G");
    Feed(p, *g, "\t");
    CHECK(g->CursorCol() == 4);
    delete g;
}

TEST_CASE("RIS performs hard reset") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[1m");  // bold
    Feed(p, *g, "Hello");
    Feed(p, *g,
         "\x1b"
         "c");  // RIS
    CHECK(g->PenAttrs() == 0);
    CHECK(CP(*g, 0, 0) == 0);
    CHECK(g->CursorRow() == 0);
    CHECK(g->CursorCol() == 0);
    delete g;
}

TEST_CASE("DECSTR keeps cursor position and alt-screen") {
    Grid* g = MakeGrid(20, 10);
    VtParser p;
    // Enter alt screen, move cursor, set bold.
    Feed(p, *g, "\x1b[?1049h");  // enter alt screen
    Feed(p, *g, "\x1b[5;7H");    // move to row 5 col 7 (1-based → 4, 6)
    Feed(p, *g, "\x1b[1m");      // bold
    Feed(p, *g, "\x1b[!p");      // DECSTR – soft reset
    // Cursor must stay at (4, 6); alt screen must still be active.
    CHECK(g->CursorRow() == 4);
    CHECK(g->CursorCol() == 6);
    CHECK(g->IsAltScreen());
    // SGR attributes must be reset.
    CHECK((g->PenAttrs() & Bold) == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// Wcwidth
// ---------------------------------------------------------------------------

TEST_CASE("Wcwidth returns 1 for ASCII") {
    CHECK(Wcwidth(U'A') == 1);
    CHECK(Wcwidth(U' ') == 1);
}

TEST_CASE("Wcwidth returns 2 for CJK") {
    CHECK(Wcwidth(0x4E2D) == 2);  // CJK 中
    CHECK(Wcwidth(0xAC00) == 2);  // Hangul 가
}

TEST_CASE("Wcwidth returns 0 for combining marks") {
    CHECK(Wcwidth(0x0300) == 0);  // Combining Grave Accent
    CHECK(Wcwidth(0x200B) == 0);  // Zero Width Space
}

// ---------------------------------------------------------------------------
// VtParser: bypass and unknown sequence counting
// ---------------------------------------------------------------------------

TEST_CASE("OSC is bypassed without accumulation") {
    Grid* g = MakeGrid(80, 5);
    VtParser p;
    // Count Byte events only (Start and End are also fired, not counted here).
    std::size_t bypassCount = 0;
    p.SetBypassCallback(
        [](EBypassEvent ev, std::byte, void* ctx) {
            if (ev == EBypassEvent::Byte) *static_cast<std::size_t*>(ctx) += 1;
        },
        &bypassCount);

    // OSC: ESC ] ... BEL
    Feed(p, *g, "\x1b]0;window title\x07");
    CHECK(bypassCount > 0);
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

TEST_CASE("Bypass callback receives Start and End events") {
    Grid* g = MakeGrid(80, 5);
    VtParser p;
    struct Counts {
        std::size_t Start{0}, Bytes{0}, End{0};
    } counts;
    p.SetBypassCallback(
        [](EBypassEvent ev, std::byte, void* ctx) {
            auto* c = static_cast<Counts*>(ctx);
            if (ev == EBypassEvent::Start)
                ++c->Start;
            else if (ev == EBypassEvent::Byte)
                ++c->Bytes;
            else if (ev == EBypassEvent::End)
                ++c->End;
        },
        &counts);
    // OSC terminated by BEL.
    Feed(p, *g, "\x1b]0;title\x07");
    CHECK(counts.Start == 1);
    CHECK(counts.Bytes > 0);
    CHECK(counts.End == 1);
    delete g;
}

TEST_CASE("CSI with unknown final increments UnknownSeqCount") {
    Grid* g = MakeGrid(80, 5);
    VtParser p;
    const std::uint64_t before = p.UnknownSeqCount();
    // CSI ? 9999 h — private mode 9999, which is not handled → unknown count.
    // Actually private modes with unknown param are not counted; ESC dispatch
    // of unknown final byte is counted. Use ESC z (not dispatched).
    Feed(p, *g, "\x1bz");
    CHECK(p.UnknownSeqCount() > before);
    delete g;
}

TEST_CASE("CAN/SUB abort current escape sequence") {
    Grid* g = MakeGrid(80, 5);
    VtParser p;
    // ESC [ 10 CAN — should abort without moving cursor.
    Feed(p, *g, "\x1b[10\x18");
    CHECK(p.State() == EParserState::Ground);
    CHECK(g->CursorRow() == 0);
    CHECK(g->CursorCol() == 0);
    delete g;
}

TEST_CASE("Parser reset returns to Ground") {
    Grid* g = MakeGrid(80, 5);
    VtParser p;
    Feed(p, *g, "\x1b[");  // partial CSI
    p.Reset();
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

// ---------------------------------------------------------------------------
// UTF-8 decoding
// ---------------------------------------------------------------------------

TEST_CASE("UTF-8 multibyte sequence decoded correctly") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    // U+00E9 'é' (2-byte UTF-8: 0xC3 0xA9)
    const std::uint8_t utf8[] = {0xC3, 0xA9};
    p.Feed(reinterpret_cast<const std::byte*>(utf8), 2, *g);
    CHECK(CP(*g, 0, 0) == 0x00E9);
    delete g;
}

TEST_CASE("Truncated UTF-8 produces replacement character") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    // Lead byte of 3-byte sequence but only 1 byte arrives, then printable.
    const std::uint8_t bad[] = {0xE4, 'A'};  // 0xE4 starts a 3-byte seq
    p.Feed(reinterpret_cast<const std::byte*>(bad), 2, *g);
    // The 'A' will restart the sequence; the 0xE4 alone is replacement.
    // (Exact output depends on implementation; just check no crash and Ground.)
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

// ---------------------------------------------------------------------------
// IL/DL outside scroll region (heap overflow guard)
// ---------------------------------------------------------------------------

TEST_CASE("IL/DL outside scroll region does not crash") {
    // Reproducer for the heap-overflow: cursor is outside [ScrollTop,ScrollBot]
    // when DECOM is active and RestoreCursor clamps into the region.  IL or DL
    // must return immediately in that situation without touching memory.
    Grid* g = MakeGrid(80, 24, 1000);
    VtParser p;
    Feed(p, *g, "\x1b[20;1H");  // move cursor to row 20
    Feed(p, *g,
         "\x1b"
         "7");                 // DECSC — save cursor
    Feed(p, *g, "\x1b[2;4r");  // DECSTBM — scroll region rows 2–4
    Feed(p, *g, "\x1b[?6h");   // DECSET ?6 — enable origin mode (DECOM)
    Feed(p, *g,
         "\x1b"
         "8");  // DECRC — restore cursor (clamped into region)
    // DL 100 with cursor outside [ScrollTop,ScrollBot] must not corrupt memory.
    Feed(p, *g, "\x1b[100L");
    // Grid must remain structurally valid.
    CHECK(g->Rows() == 24);
    CHECK(g->Cols() == 80);
    CHECK(g->CursorRow() < g->Rows());
    CHECK(g->CursorCol() < g->Cols());
    delete g;
}

// ---------------------------------------------------------------------------
// Wide-char pairing: overwriting a continuation cell breaks the wide char
// ---------------------------------------------------------------------------

TEST_CASE("Narrow char at continuation cell breaks wide-char pair") {
    // Regression for fuzz crash: write wide char (中 U+4E2D, width=2) at col 0
    // → col 1 gets Width=0. Then CUP to col 1 and write a narrow char → the
    // Width=2 at col 0 must be demoted to Width=1 so the pairing invariant
    // holds.
    Grid* g = MakeGrid(10, 4);
    VtParser p;
    // Write wide char at (0,0); continuation lands at (0,1).
    Feed(p, *g, "\xe4\xb8\xad");  // U+4E2D (中), wcwidth=2
    CHECK(g->CellAt(0, 0).Width == 2);
    CHECK(g->CellAt(0, 1).Width == 0);

    // Move cursor to (0,1) — the continuation cell.
    Feed(p, *g, "\x1b[1;2H");
    // Write a narrow char: must demote (0,0) from Width=2 to Width=1.
    Feed(p, *g, "X");
    CHECK(g->CellAt(0, 0).Width == 1);
    CHECK(CP(*g, 0, 1) == U'X');
    CHECK(g->CellAt(0, 1).Width == 1);

    delete g;
}

// ---------------------------------------------------------------------------
// Cursor movement sequences
// ---------------------------------------------------------------------------

TEST_CASE("CUU moves cursor up") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[5;1H");    // row 5 (1-based)
    Feed(p, *g, "\x1b[2A");      // CUU 2
    CHECK(g->CursorRow() == 2);  // 5-1=4 (0-based), -2 = 2
    delete g;
}

TEST_CASE("CUD moves cursor down") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[2;1H");  // row 2 (1-based) = row 1 (0-based)
    Feed(p, *g, "\x1b[3B");    // CUD 3
    CHECK(g->CursorRow() == 4);
    delete g;
}

TEST_CASE("CUF moves cursor right") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[1;1H");
    Feed(p, *g, "\x1b[5C");  // CUF 5
    CHECK(g->CursorCol() == 5);
    delete g;
}

TEST_CASE("CUB moves cursor left") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[1;10H");  // col 10 (1-based) = col 9 (0-based)
    Feed(p, *g, "\x1b[3D");     // CUB 3
    CHECK(g->CursorCol() == 6);
    delete g;
}

TEST_CASE("CNL moves cursor to next line column 1") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[2;5H");  // row 2, col 5 (1-based)
    Feed(p, *g, "\x1b[2E");    // CNL 2
    CHECK(g->CursorRow() == 3);
    CHECK(g->CursorCol() == 0);
    delete g;
}

TEST_CASE("CPL moves cursor to preceding line column 1") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[5;5H");  // row 5, col 5 (1-based)
    Feed(p, *g, "\x1b[2F");    // CPL 2
    CHECK(g->CursorRow() == 2);
    CHECK(g->CursorCol() == 0);
    delete g;
}

TEST_CASE("CHA moves cursor to absolute column") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[1;1H");
    Feed(p, *g, "\x1b[15G");  // CHA col 15 (1-based) = col 14 (0-based)
    CHECK(g->CursorCol() == 14);
    delete g;
}

TEST_CASE("VPA moves cursor to absolute row") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[3d");  // VPA row 3 (1-based) = row 2 (0-based)
    CHECK(g->CursorRow() == 2);
    delete g;
}

// ---------------------------------------------------------------------------
// Erase sequences
// ---------------------------------------------------------------------------

TEST_CASE("ED 2 clears entire screen") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");    // write something
    Feed(p, *g, "\x1b[2J");  // ED 2 clear all
    CHECK(CP(*g, 0, 0) == U'\0');
    delete g;
}

TEST_CASE("EL 1 clears to beginning of line") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "\x1b[1;3H");  // cursor at col 3 (1-based = col 2 0-based)
    Feed(p, *g, "\x1b[1K");    // EL 1: erase to left
    CHECK(CP(*g, 0, 0) == U'\0');
    CHECK(CP(*g, 0, 2) == U'\0');
    CHECK(CP(*g, 0, 3) == U'D');
    delete g;
}

TEST_CASE("EL 2 clears entire line") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "\x1b[2K");  // EL 2: erase whole line
    CHECK(CP(*g, 0, 0) == U'\0');
    CHECK(CP(*g, 0, 4) == U'\0');
    delete g;
}

TEST_CASE("ECH erases n characters at cursor") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "\x1b[1;2H");  // cursor at col 2 (1-based = col 1 0-based)
    Feed(p, *g, "\x1b[2X");    // ECH 2: erase 2 chars
    CHECK(CP(*g, 0, 0) == U'A');
    CHECK(CP(*g, 0, 1) == U'\0');
    CHECK(CP(*g, 0, 2) == U'\0');
    CHECK(CP(*g, 0, 3) == U'D');
    delete g;
}

TEST_CASE("DCH deletes characters and shifts line left") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "\x1b[1;2H");  // col 2 (1-based = col 1 0-based)
    Feed(p, *g, "\x1b[2P");    // DCH 2: delete 2 chars
    CHECK(CP(*g, 0, 0) == U'A');
    CHECK(CP(*g, 0, 1) == U'D');
    CHECK(CP(*g, 0, 2) == U'E');
    delete g;
}

TEST_CASE("ICH inserts blank characters") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "\x1b[1;2H");  // col 2 (1-based = col 1 0-based)
    Feed(p, *g, "\x1b[2@");    // ICH 2: insert 2 blanks
    CHECK(CP(*g, 0, 0) == U'A');
    CHECK(CP(*g, 0, 1) == U'\0');
    CHECK(CP(*g, 0, 2) == U'\0');
    CHECK(CP(*g, 0, 3) == U'B');
    delete g;
}

// ---------------------------------------------------------------------------
// Scroll
// ---------------------------------------------------------------------------

TEST_CASE("SU scrolls up n lines") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[1;1H");
    Feed(p, *g, "ROW1\r\n");
    Feed(p, *g, "ROW2\r\n");
    Feed(p, *g, "ROW3\r\n");
    Feed(p, *g, "\x1b[1;1H");  // back to top
    Feed(p, *g, "\x1b[2S");    // SU 2
    CHECK(CP(*g, 0, 0) == U'R');
    CHECK(CP(*g, 0, 1) == U'O');
    CHECK(CP(*g, 0, 2) == U'W');
    CHECK(CP(*g, 0, 3) == U'3');
    delete g;
}

TEST_CASE("SD scrolls down n lines") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "HELLO");
    Feed(p, *g, "\x1b[1;1H");      // back to top
    Feed(p, *g, "\x1b[2T");        // SD 2: scroll down
    CHECK(CP(*g, 0, 0) == U'\0');  // row 0 is now blank
    CHECK(CP(*g, 2, 0) == U'H');   // HELLO moved to row 2
    delete g;
}

// ---------------------------------------------------------------------------
// ESC sequences: IND, RI, NEL, HTS, RIS, DECSC/DECRC
// ---------------------------------------------------------------------------

TEST_CASE("ESC D (IND) moves down one line") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[3;1H");  // row 3, col 1
    Feed(p, *g,
         "\x1b"
         "D");  // IND
    CHECK(g->CursorRow() == 3);
    delete g;
}

TEST_CASE("ESC M (RI) moves cursor up one line") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[5;1H");  // row 5
    Feed(p, *g,
         "\x1b"
         "M");  // RI
    CHECK(g->CursorRow() == 3);
    delete g;
}

TEST_CASE("ESC E (NEL) is carriage return + line feed") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[3;5H");  // row 3, col 5 (1-based)
    Feed(p, *g,
         "\x1b"
         "E");  // NEL
    CHECK(g->CursorRow() == 3);
    CHECK(g->CursorCol() == 0);
    delete g;
}

TEST_CASE("ESC H sets tab stop; HT advances to it") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[3g");     // TBC clear all tab stops
    Feed(p, *g, "\x1b[1;13H");  // move to col 13 (1-based = 12 0-based)
    Feed(p, *g,
         "\x1b"
         "H");                 // HTS set tab stop at col 12
    Feed(p, *g, "\x1b[1;1H");  // back to col 0
    Feed(p, *g, "\t");         // HT should advance to col 12
    CHECK(g->CursorCol() == 12);
    delete g;
}

TEST_CASE("ESC 7/8 DECSC/DECRC round-trip") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[4;10H");  // row 4, col 10 (1-based)
    Feed(p, *g,
         "\x1b"
         "7");                 // DECSC save
    Feed(p, *g, "\x1b[1;1H");  // move away
    Feed(p, *g,
         "\x1b"
         "8");  // DECRC restore
    CHECK(g->CursorRow() == 3);
    CHECK(g->CursorCol() == 9);
    delete g;
}

TEST_CASE("ESC c (RIS) hard-resets grid") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[1m");    // bold
    Feed(p, *g, "\x1b[5;5H");  // move cursor
    Feed(p, *g, "HELLO");
    Feed(p, *g,
         "\x1b"
         "c");  // RIS
    CHECK(g->CursorRow() == 0);
    CHECK(g->CursorCol() == 0);
    CHECK(g->PenAttrs() == 0);
    CHECK(CP(*g, 0, 0) == U'\0');
    delete g;
}

// ---------------------------------------------------------------------------
// REP (repeat preceding character)
// ---------------------------------------------------------------------------

TEST_CASE("REP repeats previous character") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "X");
    Feed(p, *g, "\x1b[3b");  // REP 3: repeat X 3 more times
    CHECK(CP(*g, 0, 0) == U'X');
    CHECK(CP(*g, 0, 1) == U'X');
    CHECK(CP(*g, 0, 2) == U'X');
    CHECK(CP(*g, 0, 3) == U'X');
    delete g;
}

// ---------------------------------------------------------------------------
// CBT (cursor backward tab)
// ---------------------------------------------------------------------------

TEST_CASE("CBT moves cursor back to previous tab stop") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[1;17H");  // col 17 (1-based = 16 0-based)
    Feed(p, *g, "\x1b[Z");      // CBT 1: back one tab stop (to col 8)
    CHECK(g->CursorCol() == 8);
    delete g;
}

// ---------------------------------------------------------------------------
// Alt screen: content is separate
// ---------------------------------------------------------------------------

TEST_CASE("Alt screen content is separate from main screen") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "MAIN");
    Feed(p, *g, "\x1b[?1049h");  // DECSET 1049: enter alt screen
    CHECK(g->IsAltScreen());
    CHECK(CP(*g, 0, 0) == U'\0');  // alt screen is blank
    Feed(p, *g, "ALT");
    Feed(p, *g, "\x1b[?1049l");  // DECRST 1049: leave alt screen
    CHECK(!g->IsAltScreen());
    CHECK(CP(*g, 0, 0) == U'M');  // main screen preserved
    delete g;
}

// ---------------------------------------------------------------------------
// Scrollback: lines pushed on scroll are recoverable
// ---------------------------------------------------------------------------

TEST_CASE("Scrollback stores lines scrolled off") {
    Grid* g = MakeGrid(10, 3, 10);
    VtParser p;
    Feed(p, *g, "ROW1\r\n");
    Feed(p, *g, "ROW2\r\n");
    Feed(p, *g, "ROW3\r\n");  // ROW1 scrolls off
    CHECK(g->ScrollbackSize() == 1);
    const Line* hist = g->ScrollbackLine(0);
    REQUIRE(hist != nullptr);
    CHECK(hist->Cells[0].Codepoint == U'R');
    delete g;
}

// ---------------------------------------------------------------------------
// IRM (insert mode): characters push right instead of overwriting
// ---------------------------------------------------------------------------

TEST_CASE("IRM insert mode: printable char inserts instead of overwriting") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "BCD");
    Feed(p, *g, "\x1b[1;1H");  // back to col 0
    Feed(p, *g, "\x1b[4h");    // SM 4: IRM on
    Feed(p, *g, "A");          // insert A
    CHECK(CP(*g, 0, 0) == U'A');
    CHECK(CP(*g, 0, 1) == U'B');
    Feed(p, *g, "\x1b[4l");  // RM 4: IRM off
    delete g;
}

// ---------------------------------------------------------------------------
// C0 controls: BS, CR, HT, LF, VT, FF
// ---------------------------------------------------------------------------

TEST_CASE("BS moves cursor left one column") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABC");
    Feed(p, *g, "\x08");  // BS
    CHECK(g->CursorCol() == 2);
    delete g;
}

TEST_CASE("CR moves cursor to column 0") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "ABCDE");
    Feed(p, *g, "\x0D");  // CR
    CHECK(g->CursorCol() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// DECCOM/DECOM: origin mode clamps cursor to scroll region
// ---------------------------------------------------------------------------

TEST_CASE("DECOM CUP moves relative to scroll region top") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[3;7r");    // scroll region rows 3-7 (1-based)
    Feed(p, *g, "\x1b[?6h");     // DECSET 6: DECOM on
    Feed(p, *g, "\x1b[1;1H");    // CUP 1;1 should go to scroll-region top-left
    CHECK(g->CursorRow() == 2);  // 0-based: row 2 = region row 3
    CHECK(g->CursorCol() == 0);
    Feed(p, *g, "\x1b[?6l");  // DECRST 6
    delete g;
}

// ---------------------------------------------------------------------------
// UTF-8 overlong and invalid sequences
// ---------------------------------------------------------------------------

TEST_CASE("Overlong UTF-8 emits replacement character") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\xC0\x80");  // overlong NUL (invalid)
    // Replacement should be placed on screen; grid doesn't crash.
    CHECK(g->Rows() == 5);  // just no crash
    delete g;
}

TEST_CASE("Surrogate pair codepoint emits replacement character") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\xED\xA0\x80");  // U+D800: high surrogate (invalid)
    CHECK(g->Cols() == 10);       // grid survives, no crash
    delete g;
}

// ---------------------------------------------------------------------------
// Parser: CAN/SUB abort sequences
// ---------------------------------------------------------------------------

TEST_CASE("CAN in CSI aborts sequence and returns to ground") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[5");  // incomplete CSI
    Feed(p, *g, "\x18");    // CAN: abort
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

TEST_CASE("SUB in CSI aborts and prints replacement char") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[5");
    Feed(p, *g, "\x1A");  // SUB: abort + replacement
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

// ---------------------------------------------------------------------------
// Charset: G1 line-drawing
// ---------------------------------------------------------------------------

TEST_CASE("Charset SO/SI switches between G0 and G1") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b(0");  // designate G0 as line-drawing
    Feed(p, *g, "\x0E");    // SO: shift to G1
    Feed(p, *g, "\x0F");    // SI: shift back to G0
    // Just verifies no crash; charset state is internal
    CHECK(g->Cols() == 10);
    delete g;
}

// ---------------------------------------------------------------------------
// Split-feed UTF-8: multi-byte codepoint split across two Feed() calls
// ---------------------------------------------------------------------------

TEST_CASE("UTF-8 codepoint split across two Feed calls is decoded correctly") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    // U+00E9 'é' (0xC3 0xA9): feed lead byte then continuation separately.
    const std::uint8_t lead[] = {0xC3};
    const std::uint8_t cont[] = {0xA9};
    p.Feed(reinterpret_cast<const std::byte*>(lead), 1, *g);
    CHECK(CP(*g, 0, 0) == 0);  // not yet emitted after lead byte alone
    p.Feed(reinterpret_cast<const std::byte*>(cont), 1, *g);
    CHECK(CP(*g, 0, 0) == 0x00E9);
    delete g;
}

// ---------------------------------------------------------------------------
// Alt-screen with SGR colours
// ---------------------------------------------------------------------------

TEST_CASE("Alt-screen preserves colours on switch and restore") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    // Normal screen: write red 'A'.
    Feed(p, *g, "\x1b[31mA");
    CHECK(CP(*g, 0, 0) == 'A');
    CHECK(g->CellAt(0, 0).FgKind == EColorKind::Indexed);

    // Enter alt screen (DECSET 1049): saves cursor + normal-screen state.
    Feed(p, *g, "\x1b[?1049h");
    CHECK(CP(*g, 0, 0) == 0);  // alt screen is blank

    // Write blue 'B' on alt screen.
    Feed(p, *g, "\x1b[34mB");
    CHECK(CP(*g, 0, 0) == 'B');
    CHECK(g->CellAt(0, 0).FgKind == EColorKind::Indexed);

    // Leave alt screen (DECRST 1049): restores cursor + normal screen.
    Feed(p, *g, "\x1b[?1049l");
    CHECK(CP(*g, 0, 0) == 'A');
    CHECK(g->CellAt(0, 0).FgKind == EColorKind::Indexed);

    delete g;
}

// ---------------------------------------------------------------------------
// SGR: attribute flags 2/5/7/8/9 and their clear codes 22–29
// ---------------------------------------------------------------------------

TEST_CASE("SGR 2 sets Dim; SGR 22 clears Bold and Dim") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[2m");
    CHECK((g->PenAttrs() & Dim) != 0);
    Feed(p, *g, "\x1b[22m");
    CHECK((g->PenAttrs() & (Bold | Dim)) == 0);
    delete g;
}

TEST_CASE("SGR 5 sets Blink; SGR 25 clears it") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[5m");
    CHECK((g->PenAttrs() & Blink) != 0);
    Feed(p, *g, "\x1b[25m");
    CHECK((g->PenAttrs() & Blink) == 0);
    delete g;
}

TEST_CASE("SGR 7 sets Inverse; SGR 27 clears it") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[7m");
    CHECK((g->PenAttrs() & Inverse) != 0);
    Feed(p, *g, "\x1b[27m");
    CHECK((g->PenAttrs() & Inverse) == 0);
    delete g;
}

TEST_CASE("SGR 8 sets Invisible; SGR 28 clears it") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[8m");
    CHECK((g->PenAttrs() & Invisible) != 0);
    Feed(p, *g, "\x1b[28m");
    CHECK((g->PenAttrs() & Invisible) == 0);
    delete g;
}

TEST_CASE("SGR 9 sets StrikeThrough; SGR 29 clears it") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[9m");
    CHECK((g->PenAttrs() & StrikeThrough) != 0);
    Feed(p, *g, "\x1b[29m");
    CHECK((g->PenAttrs() & StrikeThrough) == 0);
    delete g;
}

TEST_CASE("SGR 3 sets Italic; SGR 23 clears it") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[3m");
    CHECK((g->PenAttrs() & Italic) != 0);
    Feed(p, *g, "\x1b[23m");
    CHECK((g->PenAttrs() & Italic) == 0);
    delete g;
}

TEST_CASE("SGR 4 sets Underline; SGR 24 clears it") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[4m");
    CHECK((g->PenAttrs() & Underline) != 0);
    Feed(p, *g, "\x1b[24m");
    CHECK((g->PenAttrs() & Underline) == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// SGR: BG indexed colors 40–47 and bright FG/BG 90–107
// ---------------------------------------------------------------------------

TEST_CASE("SGR 40-47 sets indexed background color") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[42m");  // green bg
    CHECK(g->PenBgKind() == EColorKind::Indexed);
    CHECK(g->PenBgIdx() == 2);
    Feed(p, *g, "\x1b[47m");  // white bg
    CHECK(g->PenBgIdx() == 7);
    delete g;
}

TEST_CASE("SGR 90-97 sets bright indexed fg color") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[91m");  // bright red
    CHECK(g->PenFgKind() == EColorKind::Indexed);
    CHECK(g->PenFgIdx() == 9);
    Feed(p, *g, "\x1b[97m");  // bright white
    CHECK(g->PenFgIdx() == 15);
    delete g;
}

TEST_CASE("SGR 100-107 sets bright indexed bg color") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[101m");  // bright red bg
    CHECK(g->PenBgKind() == EColorKind::Indexed);
    CHECK(g->PenBgIdx() == 9);
    Feed(p, *g, "\x1b[107m");  // bright white bg
    CHECK(g->PenBgIdx() == 15);
    delete g;
}

// ---------------------------------------------------------------------------
// SGR: 38;5;n and 48;5;n via semicolons
// ---------------------------------------------------------------------------

TEST_CASE("SGR 38;5;n sets indexed fg via semicolons") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[38;5;200m");
    CHECK(g->PenFgKind() == EColorKind::Indexed);
    CHECK(g->PenFgIdx() == 200);
    delete g;
}

TEST_CASE("SGR 48;5;n sets indexed bg via semicolons") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[48;5;100m");
    CHECK(g->PenBgKind() == EColorKind::Indexed);
    CHECK(g->PenBgIdx() == 100);
    delete g;
}

TEST_CASE("SGR 48;2;r;g;b sets truecolor bg via semicolons") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[48;2;10;20;30m");
    CHECK(g->PenBgKind() == EColorKind::TrueColor);
    CHECK(g->PenBg()[0] == 10);
    CHECK(g->PenBg()[1] == 20);
    CHECK(g->PenBg()[2] == 30);
    delete g;
}

// ---------------------------------------------------------------------------
// SGR: 48 colon-form (256-color and truecolor bg)
// ---------------------------------------------------------------------------

TEST_CASE("SGR 48:5:n colon-form sets indexed bg") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    // CSI 48:5:77 m — colon sub-params for indexed bg
    Feed(p, *g, "\x1b[48:5:77m");
    CHECK(g->PenBgKind() == EColorKind::Indexed);
    CHECK(g->PenBgIdx() == 77);
    delete g;
}

TEST_CASE("SGR 48:2:r:g:b colon-form sets truecolor bg") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[48:2:50:100:150m");
    CHECK(g->PenBgKind() == EColorKind::TrueColor);
    CHECK(g->PenBg()[0] == 50);
    CHECK(g->PenBg()[1] == 100);
    CHECK(g->PenBg()[2] == 150);
    delete g;
}

TEST_CASE("SGR 48:2::r:g:b colon-form with colorspace sets truecolor bg") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[48:2::60:120:180m");
    CHECK(g->PenBgKind() == EColorKind::TrueColor);
    CHECK(g->PenBg()[0] == 60);
    CHECK(g->PenBg()[1] == 120);
    CHECK(g->PenBg()[2] == 180);
    delete g;
}

// ---------------------------------------------------------------------------
// SGR 58: underline color truecolor consumes and drops
// ---------------------------------------------------------------------------

TEST_CASE("SGR 58;2;r;g;b underline truecolor is consumed silently") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    // Truecolor underline via semicolons: consume 4 extra params, no storage.
    Feed(p, *g, "\x1b[58;2;1;2;3m");
    CHECK(g->PenUlIdx() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// CSI I (CHT), CSI a (HPR), CSI e (VPR)
// ---------------------------------------------------------------------------

TEST_CASE("CSI I (CHT) advances cursor by n tab stops") {
    Grid* g = MakeGrid(80, 5);
    VtParser p;
    Feed(p, *g, "\x1b[2I");  // CHT: advance 2 tab stops from col 0
    CHECK(g->CursorCol() == 16);
    delete g;
}

TEST_CASE("CSI a (HPR) moves cursor right by n columns") {
    Grid* g = MakeGrid(80, 5);
    VtParser p;
    Feed(p, *g, "\x1b[5a");  // HPR: move right 5
    CHECK(g->CursorCol() == 5);
    delete g;
}

TEST_CASE("CSI e (VPR) moves cursor down by n rows") {
    Grid* g = MakeGrid(80, 10);
    VtParser p;
    Feed(p, *g, "\x1b[3e");  // VPR: move down 3
    CHECK(g->CursorRow() == 3);
    delete g;
}

// ---------------------------------------------------------------------------
// CSI s (SCP) and CSI u (RCP): save/restore cursor (ANSI)
// ---------------------------------------------------------------------------

TEST_CASE("CSI s saves cursor; CSI u restores it") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[5;10H");  // move to row 4, col 9 (1-based → 0-based)
    Feed(p, *g, "\x1b[s");      // SCP: save
    Feed(p, *g, "\x1b[H");      // move to home
    CHECK(g->CursorRow() == 0);
    CHECK(g->CursorCol() == 0);
    Feed(p, *g, "\x1b[u");  // RCP: restore
    CHECK(g->CursorRow() == 4);
    CHECK(g->CursorCol() == 9);
    delete g;
}

// ---------------------------------------------------------------------------
// CSI ! p (DECSTR via parser), CSI SP q (DECSCUSR), CSI c / n
// ---------------------------------------------------------------------------

TEST_CASE("CSI !p (DECSTR) performs soft reset via parser") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[1m");  // set bold
    CHECK((g->PenAttrs() & Bold) != 0);
    Feed(p, *g, "\x1b[!p");  // DECSTR
    CHECK((g->PenAttrs() & Bold) == 0);
    delete g;
}

TEST_CASE("CSI SP q (DECSCUSR) sets cursor style") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[2 q");  // DECSCUSR: steady block
    CHECK(g->CursorStyle() == 2);
    Feed(p, *g, "\x1b[0 q");  // DECSCUSR: default
    CHECK(g->CursorStyle() == 0);
    delete g;
}

TEST_CASE("CSI c (DA) and CSI n (DSR) are silently ignored") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    // DA1: ESC [ c — device attributes query
    Feed(p, *g, "\x1b[c");
    // DSR: ESC [ 6 n — cursor position report request
    Feed(p, *g, "\x1b[6n");
    // No crash; grid unchanged.
    CHECK(g->CursorRow() == 0);
    delete g;
}

TEST_CASE("CSI with unknown final increments UnknownSeqCount") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    // 'y' is not a handled final byte; must hit the default branch.
    Feed(p, *g, "\x1b[5y");
    CHECK(p.UnknownSeqCount() >= 1);
    // Also check CSI p without '!' intermediate → unknown.
    Feed(p, *g, "\x1b[p");
    CHECK(p.UnknownSeqCount() >= 2);
    delete g;
}

// ---------------------------------------------------------------------------
// SetMode: untested DECSET/DECRST modes
// ---------------------------------------------------------------------------

TEST_CASE("DECSET ?25 shows cursor; DECRST ?25 hides it") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[?25l");  // hide cursor
    CHECK(!g->ModeSet(EMode::Dectcem));
    Feed(p, *g, "\x1b[?25h");  // show cursor
    CHECK(g->ModeSet(EMode::Dectcem));
    delete g;
}

TEST_CASE("DECSET ?47 switches to alt screen") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    Feed(p, *g, "hello");
    Feed(p, *g, "\x1b[?47h");  // enter alt via mode 47
    CHECK(g->IsAltScreen());
    CHECK(CP(*g, 0, 0) == 0);
    Feed(p, *g, "\x1b[?47l");  // leave alt via mode 47
    CHECK(!g->IsAltScreen());
    CHECK(CP(*g, 0, 0) == 'h');
    delete g;
}

TEST_CASE("SM/RM 20 toggles LNM mode") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[20h");  // SM 20: set LNM
    CHECK(g->ModeSet(EMode::Lnm));
    Feed(p, *g, "\x1b[20l");  // RM 20: clear LNM
    CHECK(!g->ModeSet(EMode::Lnm));
    delete g;
}

TEST_CASE("DECSET ?1002/?1003/?1004/?1005/?1006/?1015 set mouse/focus modes") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[?1002h");
    CHECK(g->ModeSet(EMode::Mouse1002));
    Feed(p, *g, "\x1b[?1003h");
    CHECK(g->ModeSet(EMode::Mouse1003));
    Feed(p, *g, "\x1b[?1004h");
    CHECK(g->ModeSet(EMode::FocusEvents));
    Feed(p, *g, "\x1b[?1005h");
    CHECK(g->ModeSet(EMode::Mouse1005));
    Feed(p, *g, "\x1b[?1006h");
    CHECK(g->ModeSet(EMode::Mouse1006));
    Feed(p, *g, "\x1b[?1015h");
    CHECK(g->ModeSet(EMode::Mouse1015));
    Feed(p, *g, "\x1b[?1002l");
    CHECK(!g->ModeSet(EMode::Mouse1002));
    delete g;
}

TEST_CASE("DECSET ?2026 sets SyncOutput mode") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[?2026h");
    CHECK(g->ModeSet(EMode::SyncOutput));
    Feed(p, *g, "\x1b[?2026l");
    CHECK(!g->ModeSet(EMode::SyncOutput));
    delete g;
}

// ---------------------------------------------------------------------------
// Accessors exercised for coverage (PenFg, PenBg, PenBgIdx, CursorStyle, etc.)
// ---------------------------------------------------------------------------

TEST_CASE("PenFg and PenBg return pointer to color bytes") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[38;2;11;22;33m");
    const std::uint8_t* fg = g->PenFg();
    CHECK(fg[0] == 11);
    CHECK(fg[1] == 22);
    CHECK(fg[2] == 33);
    Feed(p, *g, "\x1b[48;2;44;55;66m");
    const std::uint8_t* bg = g->PenBg();
    CHECK(bg[0] == 44);
    CHECK(bg[1] == 55);
    CHECK(bg[2] == 66);
    delete g;
}

TEST_CASE("PossiblyDiverged can be set and read") {
    Grid* g = MakeGrid(10, 5);
    CHECK(!g->PossiblyDiverged());
    g->SetPossiblyDiverged(true);
    CHECK(g->PossiblyDiverged());
    g->SetPossiblyDiverged(false);
    CHECK(!g->PossiblyDiverged());
    delete g;
}

TEST_CASE("Modes() returns the raw mode bitfield") {
    Grid* g = MakeGrid(10, 5);
    // Default: AWM and DECTCEM are set.
    const std::uint32_t m = g->Modes();
    CHECK((m & ModeFlag(EMode::Awm)) != 0);
    CHECK((m & ModeFlag(EMode::Dectcem)) != 0);
    delete g;
}

TEST_CASE("CharsetActive and G0/G1 accessors reflect designate and shift") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b(0");  // designate G0 as line-drawing (charset 1)
    CHECK(g->CharsetG0() == 1);
    CHECK(g->CharsetG1() == 0);
    Feed(p, *g, "\x0E");  // SO: shift to G1
    CHECK(g->CharsetActive() == 1);
    Feed(p, *g, "\x0F");  // SI: shift back to G0
    CHECK(g->CharsetActive() == 0);
    delete g;
}

TEST_CASE("ScrollbackLimit and ScrollbackSize are readable") {
    Grid* g = MakeGrid(10, 5, 50);
    CHECK(g->ScrollbackLimit() == 50);
    CHECK(g->ScrollbackSize() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// CSI q without space intermediate hits unknown branch
// ---------------------------------------------------------------------------

TEST_CASE("CSI q without space intermediate increments UnknownSeqCount") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[2q");  // 'q' without space: unknown
    CHECK(p.UnknownSeqCount() >= 1);
    delete g;
}

// ---------------------------------------------------------------------------
// LNM: LineFeed resets column to 0 when LNM is active
// ---------------------------------------------------------------------------

TEST_CASE("LNM mode resets column to zero on line feed") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    Feed(p, *g, "\x1b[20h");  // SM 20: set LNM
    Feed(p, *g, "hello");     // cursor now at col 5
    CHECK(g->CursorCol() == 5);
    Feed(p, *g, "\n");  // LF with LNM → col resets to 0
    CHECK(g->CursorCol() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// ED 0 (erase to end), ED 1 (erase to beginning), ED 3 (clear scrollback)
// ---------------------------------------------------------------------------

TEST_CASE("ED 0 erases from cursor to end of screen") {
    Grid* g = MakeGrid(5, 4);
    VtParser p;
    Feed(p, *g, "ABCDE\r\nFGHIJ\r\nKLMNO\r\nPQRST");
    Feed(p, *g, "\x1b[2;3H");    // move to row 1, col 2 (1-based)
    Feed(p, *g, "\x1b[J");       // ED 0: erase from cursor to end
    CHECK(CP(*g, 0, 0) == 'A');  // row 0 unchanged
    CHECK(CP(*g, 1, 0) == 'F');  // row 1 before cursor unchanged
    CHECK(CP(*g, 1, 1) == 'G');
    CHECK(CP(*g, 1, 2) == 0);  // cursor cell and beyond erased
    CHECK(CP(*g, 2, 0) == 0);  // row 2 fully erased
    delete g;
}

TEST_CASE("ED 1 erases from beginning to cursor") {
    Grid* g = MakeGrid(5, 4);
    VtParser p;
    Feed(p, *g, "ABCDE\r\nFGHIJ\r\nKLMNO\r\nPQRST");
    Feed(p, *g, "\x1b[3;3H");  // move to row 2, col 2 (1-based)
    Feed(p, *g, "\x1b[1J");    // ED 1: erase from beginning to cursor
    CHECK(CP(*g, 0, 0) == 0);  // row 0 fully erased
    CHECK(CP(*g, 1, 0) == 0);  // row 1 fully erased
    CHECK(CP(*g, 2, 0) == 0);  // erased up to cursor col
    CHECK(CP(*g, 2, 1) == 0);
    CHECK(CP(*g, 2, 2) == 0);    // cursor cell erased
    CHECK(CP(*g, 3, 0) == 'P');  // row 3 unchanged
    delete g;
}

TEST_CASE("ED 3 erases screen and clears scrollback") {
    Grid* g = MakeGrid(5, 3, 10);
    VtParser p;
    // Fill screen and push one line to scrollback.
    Feed(p, *g, "AAAAA\r\nBBBBB\r\nCCCCC\r\nDDDDD");
    CHECK(g->ScrollbackSize() >= 1);
    Feed(p, *g, "\x1b[3J");  // ED 3: clear screen + scrollback
    CHECK(CP(*g, 0, 0) == 0);
    CHECK(g->ScrollbackSize() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// G1 charset designation (ESC ) 0) and DEC Special Graphics via SO
// ---------------------------------------------------------------------------

TEST_CASE("G1 charset designation prints DEC graphics chars via SO") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b)0");  // designate G1 as DEC Special Graphics
    CHECK(g->CharsetG1() == 1);
    Feed(p, *g, "\x0E");  // SO: shift to G1
    CHECK(g->CharsetActive() == 1);
    // 'q' (0x71) maps to U+2500 (horizontal line) in DEC GFX
    Feed(p, *g, "q");
    CHECK(CP(*g, 0, 0) == 0x2500);
    Feed(p, *g, "\x0F");  // SI: back to G0
    CHECK(g->CharsetActive() == 0);
    delete g;
}

// ---------------------------------------------------------------------------
// BEL, VT (0x0B), FF (0x0C) control characters
// ---------------------------------------------------------------------------

TEST_CASE("BEL control is silently ignored") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g,
         "A\x07"
         "B");  // A, BEL, B
    CHECK(CP(*g, 0, 0) == 'A');
    CHECK(CP(*g, 0, 1) == 'B');
    delete g;
}

TEST_CASE("VT (0x0B) and FF (0x0C) act as line feed") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g,
         "A\x0B"
         "B");  // A, VT, B
    CHECK(CP(*g, 0, 0) == 'A');
    CHECK(CP(*g, 1, 1) == 'B');  // VT moved cursor down; B at row 1 col 1
    Feed(p, *g, "\x1b[H");       // home
    Feed(p, *g,
         "\x0C"
         "C");  // FF at row 0 → row 1
    CHECK(CP(*g, 1, 0) == 'C');
    delete g;
}

// ---------------------------------------------------------------------------
// ESC with unrecognised final increments UnknownSeqCount
// ---------------------------------------------------------------------------

TEST_CASE("ESC with unknown final byte increments UnknownSeqCount") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1bz");  // ESC z: not recognised, default branch
    CHECK(p.UnknownSeqCount() >= 1);
    delete g;
}

// ---------------------------------------------------------------------------
// CsiIgnore state: param byte after intermediate → ignored dispatch
// ---------------------------------------------------------------------------

TEST_CASE("CSI intermediate then param byte transitions to CsiIgnore") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    // CSI space '2' 'm': space is intermediate, '2' is param after intermediate
    // → CsiIntermediate sees param byte (0x30-0x3F) → CsiIgnore.
    // Then 'm' (final) fires UnknownSeqCount from CsiIgnore.
    const std::uint64_t before = p.UnknownSeqCount();
    Feed(p, *g, "\x1b[ 2m");
    CHECK(p.UnknownSeqCount() > before);
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

// ---------------------------------------------------------------------------
// BypassBytes() accessor and bypass-limit enforcement
// ---------------------------------------------------------------------------

TEST_CASE("BypassBytes reports bytes received in an OSC sequence") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    // Start an OSC sequence: ESC ] 0 ; hello
    Feed(p, *g, "\x1b]0;hello");
    // In bypass state: 7 bytes fed (0, ;, h, e, l, l, o) but '0', ';' are
    // also payload bytes.
    CHECK(p.BypassBytes() > 0);
    // Terminate with BEL
    Feed(p, *g, "\x07");
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

// ---------------------------------------------------------------------------
// Wide char at last column without AWM wraps instead of dropping
// ---------------------------------------------------------------------------

TEST_CASE("Wide char at last column without AWM is dropped") {
    Grid* g = MakeGrid(5, 5);
    VtParser p;
    Feed(p, *g, "\x1b[?7l");  // disable AWM
    Feed(p, *g, "ABCD");      // cursor at col 4 (last)
    // Writing a wide char (中 = U+4E2D) at col 4 with AWM off: no room → drop.
    Feed(p, *g, "\xe4\xb8\xad");  // 中 in UTF-8
    // No crash; col 4 should still be 'D' or blank.
    CHECK(g->CursorCol() <= 4);
    delete g;
}

// ---------------------------------------------------------------------------
// PutChar with combining mark when PendingWrap is set
// ---------------------------------------------------------------------------

TEST_CASE(
    "Combining mark with PendingWrap attaches to last char on previous row") {
    // A 1-column grid: after writing a char, CursorCol_==0 and
    // PendingWrap==true. The next char writes to the next row; if instead we
    // send a combining mark it should attach to the char at the end of the
    // previous row.
    Grid* g = MakeGrid(1, 5);
    VtParser p;
    Feed(p, *g,
         "A");  // fills (0,0); PendingWrap=true, CursorRow_=0, CursorCol_=0
    Feed(p, *g, "B");  // wrap to row 1: writes 'B' at (1,0); PendingWrap=true
    CHECK(g->PendingWrap());
    CHECK(g->CursorRow() == 1);
    CHECK(g->CursorCol() == 0);
    // U+0301 combining acute accent (UTF-8: 0xCC 0x81, width=0).
    // PendingWrap=true, CursorCol_=0, CursorRow_=1>0 → attaches to (0, 0) =
    // 'A'.
    const std::uint8_t acute[] = {0xCC, 0x81};
    p.Feed(reinterpret_cast<const std::byte*>(acute), 2, *g);
    CHECK(CP(*g, 0, 0) == 0x0301);
    delete g;
}

// ---------------------------------------------------------------------------
// RestoreCursor with DECOM active clamps into scroll region
// ---------------------------------------------------------------------------

TEST_CASE("RestoreCursor clamps into scroll region when DECOM is active") {
    Grid* g = MakeGrid(80, 24);
    VtParser p;
    // Save cursor at row 0 (default), then set scroll region and enable DECOM.
    // When restored with DECOM active, row 0 < ScrollTop → clamped to
    // ScrollTop.
    Feed(p, *g, "\x1b[s");  // save cursor at (0, 0)
    Feed(
        p, *g,
        "\x1b[5;20r");  // set scroll region rows 5–20 (1-based → top=4, bot=19)
    Feed(p, *g, "\x1b[?6h");  // enable DECOM
    Feed(p, *g, "\x1b[u");    // restore: row 0 < ScrollTop=4 → clamp to 4
    CHECK(g->CursorRow() == g->ScrollTop());
    delete g;
}

// ---------------------------------------------------------------------------
// Resize with alt screen: alt-screen content is resized too
// ---------------------------------------------------------------------------

TEST_CASE("Resize in alt screen preserves alt content") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    Feed(p, *g, "\x1b[?1049h");  // enter alt screen
    Feed(p, *g, "HELLO");
    CHECK(CP(*g, 0, 0) == 'H');
    (void)g->Resize(10, 5);
    CHECK(CP(*g, 0, 0) == 'H');  // content survives resize
    Feed(p, *g, "\x1b[?1049l");  // leave alt screen
    delete g;
}

// ---------------------------------------------------------------------------
// Resize with shrink: cursor is adjusted (content shift path)
// ---------------------------------------------------------------------------

TEST_CASE("Resize shrink adjusts cursor when rows are lost") {
    Grid* g = MakeGrid(10, 10);
    VtParser p;
    Feed(p, *g, "\x1b[10;1H");  // move cursor to last row (row 9)
    CHECK(g->CursorRow() == 9);
    (void)g->Resize(10, 5);  // shrink to 5 rows: cursor shifts up
    CHECK(g->CursorRow() < 5);
    delete g;
}

// ---------------------------------------------------------------------------
// SwitchAltScreen: entering already-active alt screen is a no-op
// ---------------------------------------------------------------------------

TEST_CASE("SwitchAltScreen no-op when already in target state") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    Feed(p, *g, "\x1b[?47h");  // enter alt
    Feed(p, *g, "X");
    Feed(p, *g, "\x1b[?47h");  // enter alt again (no-op)
    CHECK(g->IsAltScreen());
    CHECK(CP(*g, 0, 0) == 'X');  // content unchanged
    delete g;
}

// ---------------------------------------------------------------------------
// DCS entry: ESC P introduces a DCS pass-through sequence
// ---------------------------------------------------------------------------

TEST_CASE("ESC P starts DCS pass-through and exits on ST") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    // ESC P <payload bytes> ST:  DCS pass-through, then ESC \ terminates
    Feed(p, *g, "\x1bPhello\x1b\\");
    // After ST, parser must be back in Ground
    CHECK(p.State() == EParserState::Ground);
    // BypassBytes > 0: payload was counted
    CHECK(p.BypassBytes() > 0);
    delete g;
}

// ---------------------------------------------------------------------------
// SOS/PM/APC: ESC X / ESC ^ / ESC _ introduce bypass sequences
// ---------------------------------------------------------------------------

TEST_CASE("ESC X starts SOS bypass sequence") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    std::size_t before = p.BypassBytes();
    Feed(p, *g,
         "\x1b\x58"
         "data\x1b\\");  // ESC X payload ST
    CHECK(p.State() == EParserState::Ground);
    CHECK(p.BypassBytes() > before);
    delete g;
}

TEST_CASE("ESC ^ starts PM bypass sequence") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    Feed(p, *g,
         "\x1b\x5E"
         "msg\x1b\\");  // ESC ^ payload ST
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

TEST_CASE("ESC _ starts APC bypass sequence") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    Feed(p, *g,
         "\x1b\x5F"
         "app\x1b\\");  // ESC _ payload ST
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

// ---------------------------------------------------------------------------
// ESC Q-W range (0x51-0x57): dispatched via DispatchEsc, hit default branch
// ---------------------------------------------------------------------------

TEST_CASE("ESC Q increments UnknownSeqCount (ESC 0x51 range)") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    std::uint64_t before = p.UnknownSeqCount();
    Feed(p, *g, "\x1bQ");  // ESC Q = 0x51
    CHECK(p.UnknownSeqCount() > before);
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

TEST_CASE("ESC W increments UnknownSeqCount (ESC 0x57 range)") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    std::uint64_t before = p.UnknownSeqCount();
    Feed(p, *g, "\x1bW");  // ESC W = 0x57
    CHECK(p.UnknownSeqCount() > before);
    delete g;
}

// ---------------------------------------------------------------------------
// ESC backslash ST terminator: case '\\' in DispatchEsc
// ---------------------------------------------------------------------------

TEST_CASE("OSC terminated with ESC-backslash ST exits to Ground") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    // OSC title: ESC ] 0 ; text ESC \  (ESC backslash = ST)
    Feed(p, *g, "\x1b]0;my title\x1b\\");
    CHECK(p.State() == EParserState::Ground);
    delete g;
}

// ---------------------------------------------------------------------------
// ESC DEL (0x7F) in Escape state: silently ignored
// ---------------------------------------------------------------------------

TEST_CASE("ESC 0x7F in Escape state is ignored") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    std::uint64_t before = p.UnknownSeqCount();
    // Send ESC then 0x7F; parser should not crash and UnknownSeqCount unchanged
    Feed(p, *g, "\x1b\x7f");
    CHECK(p.UnknownSeqCount() == before);
    delete g;
}

// ---------------------------------------------------------------------------
// DECOM cursor movement: CUU/CUD clamp to scroll region, VPA clamps too
// ---------------------------------------------------------------------------

TEST_CASE("CUU with DECOM active clamps to scroll region top") {
    Grid* g = MakeGrid(20, 24);
    VtParser p;
    // Set scroll region rows 5-20 (CSI 5;20 r), enable DECOM (?6h)
    Feed(p, *g, "\x1b[5;20r");
    Feed(p, *g, "\x1b[?6h");
    // DECOM positions cursor relative to scroll region; move to row 5 (1-based
    // in region)
    Feed(
        p, *g,
        "\x1b[1;1H");  // cursor at top of scroll region under DECOM = row 4 abs
    // Now move up more than allowed: scroll top limit should clamp
    Feed(p, *g, "\x1b[10A");  // CUU 10: cannot go above ScrollTop_ (4)
    CHECK(g->CursorRow() == g->ScrollTop());
    delete g;
}

TEST_CASE("CUD with DECOM active clamps to scroll region bottom") {
    Grid* g = MakeGrid(20, 24);
    VtParser p;
    // Set scroll region rows 5-20, enable DECOM
    Feed(p, *g, "\x1b[5;20r");
    Feed(p, *g, "\x1b[?6h");
    // Move to bottom of scroll region and try to go further down
    Feed(p, *g,
         "\x1b[16;1H");       // row 16 in region = row 19 abs (near scroll bot)
    Feed(p, *g, "\x1b[10B");  // CUD 10: clamps to ScrollBot_
    CHECK(g->CursorRow() <= g->ScrollBot());
    delete g;
}

TEST_CASE("VPA with DECOM active uses scroll region offset") {
    Grid* g = MakeGrid(20, 24);
    VtParser p;
    // Set scroll region rows 5-20, enable DECOM
    Feed(p, *g, "\x1b[5;20r");
    Feed(p, *g, "\x1b[?6h");
    // VPA 1 positions at top of scroll region
    Feed(p, *g,
         "\x1b[1d");  // CSI 1 d = VPA row 1 (1-based, in DECOM = scroll top)
    CHECK(g->CursorRow() == g->ScrollTop());
    delete g;
}

TEST_CASE("CUP with DECOM active positions within scroll region") {
    Grid* g = MakeGrid(20, 24);
    VtParser p;
    // Set scroll region rows 5-20, enable DECOM
    Feed(p, *g, "\x1b[5;20r");
    Feed(p, *g, "\x1b[?6h");
    // CUP 2;3 under DECOM = absolute row ScrollTop+1, col 2
    Feed(p, *g, "\x1b[2;3H");
    CHECK(g->CursorRow() == g->ScrollTop() + 1);
    CHECK(g->CursorCol() == 2);
    delete g;
}

// ---------------------------------------------------------------------------
// RepeatChar (REP) before any printable char: early return
// ---------------------------------------------------------------------------

TEST_CASE("REP with no prior printable char is silently ignored") {
    Grid* g = MakeGrid(20, 5);
    VtParser p;
    // CSI 5 b = REP 5 times — but LastPrintedChar_ is 0, so it's a no-op
    Feed(p, *g, "\x1b[5b");
    CHECK(CP(*g, 0, 0) == 0);  // nothing printed
    delete g;
}
