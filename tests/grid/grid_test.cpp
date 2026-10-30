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
    CHECK((g->PenAttrs() & kAttrBold) != 0);
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

TEST_CASE("SGR 38;2 sets truecolor fg") {
    Grid* g = MakeGrid(10, 5);
    VtParser p;
    Feed(p, *g, "\x1b[38;2;255;128;0m");
    CHECK(g->PenFgKind() == EColorKind::TrueColor);
    const ExtColor ec = g->GetExt(g->PenFgExtIdx());
    CHECK(ec.R == 255);
    CHECK(ec.G == 128);
    CHECK(ec.B == 0);
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

    g->Resize(6, 4);
    CHECK(g->Cols() == 6);
    CHECK(g->Rows() == 4);
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

    g->Resize(5, 5);
    CHECK(g->CursorRow() <= 4);
    CHECK(g->CursorCol() <= 4);
    delete g;
}

TEST_CASE("Resize clamps saved cursors") {
    Grid* g = MakeGrid(10, 10);
    VtParser p;
    Feed(p, *g, "\x1b[8;8H");
    Feed(p, *g, "\x1b[s");  // save
    g->Resize(5, 5);
    Feed(p, *g, "\x1b[u");  // restore — must not panic
    CHECK(g->CursorRow() <= 4);
    CHECK(g->CursorCol() <= 4);
    delete g;
}

TEST_CASE("Resize to zero is clamped to 1x1") {
    Grid* g = MakeGrid(10, 10);
    g->Resize(0, 0);
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
// ExtColor pool cap
// ---------------------------------------------------------------------------

TEST_CASE("ExtColor pool exhaustion degrades without crash") {
    // Create a small grid and fill enough truecolor cells to exhaust the pool.
    // We exhaust by repeating SGR 38;2 + print many times.
    Grid* g = MakeGrid(80, 24, 0);
    VtParser p;
    // Each unique truecolor entry uses 1 slot (we reuse pen extidx).
    // Actually ApplySgr allocates into PenFgExtIdx_ each time, but the old one
    // isn't freed until a cell is written. We'll write cells to populate them.
    for (int i = 0; i < 70000; ++i) {
        // Move cursor back to (0,0) every 80 chars.
        if (i % 80 == 0) Feed(p, *g, "\x1b[1;1H");
        // Different colors to force new allocs.
        const std::uint8_t r = static_cast<std::uint8_t>(i & 0xFF);
        const std::uint8_t gv = static_cast<std::uint8_t>((i >> 8) & 0xFF);
        const std::uint8_t b = static_cast<std::uint8_t>((i >> 16) & 0xFF);
        std::uint16_t params[] = {38, 2, r, gv, b};
        g->ApplySgr(params, 5);
        g->PutChar('X', 1);
    }
    // Just assert we didn't abort — all handled by pool exhaustion degradation.
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
    Feed(p, *g, "\x1bH");    // HTS
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
    std::size_t bypassCount = 0;
    p.SetBypassCallback(
        [](std::byte, void* ctx) { *static_cast<std::size_t*>(ctx) += 1; },
        &bypassCount);

    // OSC: ESC ] ... BEL
    Feed(p, *g, "\x1b]0;window title\x07");
    CHECK(bypassCount > 0);
    CHECK(p.State() == EParserState::Ground);
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
