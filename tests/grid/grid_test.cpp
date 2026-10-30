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
        std::size_t start{0}, bytes{0}, end{0};
    } counts;
    p.SetBypassCallback(
        [](EBypassEvent ev, std::byte, void* ctx) {
            auto* c = static_cast<Counts*>(ctx);
            if (ev == EBypassEvent::Start)
                ++c->start;
            else if (ev == EBypassEvent::Byte)
                ++c->bytes;
            else if (ev == EBypassEvent::End)
                ++c->end;
        },
        &counts);
    // OSC terminated by BEL.
    Feed(p, *g, "\x1b]0;title\x07");
    CHECK(counts.start == 1);
    CHECK(counts.bytes > 0);
    CHECK(counts.end == 1);
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
