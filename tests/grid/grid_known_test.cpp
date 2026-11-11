// Line::KnownFrom and Grid::Resync (design.md §9, round C): which cells of the
// mirror the terminal's own contents are known to match.
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include "zet/grid/grid.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

struct Rig {
    std::unique_ptr<Grid> G;
    VtParser P;

    explicit Rig(int cols = 10, int rows = 4) {
        auto r = Grid::Create(static_cast<std::uint16_t>(cols),
                              static_cast<std::uint16_t>(rows), 0);
        REQUIRE(r.has_value());
        G.reset(r.value());
    }

    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }

    [[nodiscard]] std::uint16_t Known(int row) const {
        return G->KnownFrom(static_cast<std::uint16_t>(row));
    }
    [[nodiscard]] char32_t Cp(int row, int col) const {
        return G
            ->CellAt(static_cast<std::uint16_t>(row),
                     static_cast<std::uint16_t>(col))
            .Codepoint;
    }
};

// Rows: 0 and 1 foreign (10), row 2 known from 5, row 3 ordinary (0). Cursor
// at (2,5), the way a session start leaves a mirror.
Rig Started() {
    Rig r;
    r.G->Resync(2, 5, true);
    return r;
}

}  // namespace

TEST_CASE("a fresh grid knows every cell") {
    Rig r;
    for (int row = 0; row < 4; ++row) CHECK(r.Known(row) == 0);
    r.Feed("hello\r\nworld");
    for (int row = 0; row < 4; ++row) CHECK(r.Known(row) == 0);
}

TEST_CASE("Resync at the start: foreign rows above, blank known rows below") {
    Rig r;
    r.Feed(
        "\x1b[1;1Haaaaaaaaaa\x1b[2;1Hbbbbbbbbbb\x1b[3;1Hcccccccccc"
        "\x1b[4;1Hdddddddddd");
    const auto before = r.G->ColumnSetCount();
    REQUIRE(r.G->Resync(2, 5, true));
    CHECK(r.G->CursorRow() == 2);
    CHECK(r.G->CursorCol() == 5);
    CHECK_FALSE(r.G->PendingWrap());
    CHECK(r.Known(0) == 10);
    CHECK(r.Known(1) == 10);
    CHECK(r.Known(2) == 5);
    CHECK(r.Known(3) == 0);
    // Foreign rows keep what the mirror held; the tail of the cursor row and
    // the rows below are blank.
    CHECK(r.Cp(0, 0) == U'a');
    CHECK(r.Cp(1, 9) == U'b');
    CHECK(r.Cp(2, 4) == U'c');
    CHECK(r.Cp(2, 5) == 0);
    CHECK(r.Cp(2, 9) == 0);
    for (int c = 0; c < 10; ++c) CHECK(r.Cp(3, c) == 0);
    CHECK(r.G->ColumnSetCount() == before + 1);
}

TEST_CASE("Resync after another event leaves the row tail foreign") {
    Rig r;
    r.Feed("\x1b[3;1Hcccccccccc\x1b[4;1Hdddddddddd");
    REQUIRE(r.G->Resync(2, 5, false));
    CHECK(r.Known(2) == 10);
    CHECK(r.Cp(2, 7) == U'c');  // the tail is not erased
    CHECK(r.Known(3) == 0);
    CHECK(r.Cp(3, 0) == 0);
}

TEST_CASE("Resync clamps the position") {
    Rig r;
    REQUIRE(r.G->Resync(99, 99, true));
    CHECK(r.G->CursorRow() == 3);
    CHECK(r.G->CursorCol() == 9);
    CHECK(r.Known(3) == 9);
    CHECK(r.Known(0) == 10);
}

TEST_CASE("Resync is for the main screen") {
    Rig r;
    r.Feed("\x1b[?1049h\x1b[2;3Hx");
    const auto row = r.G->CursorRow();
    const auto col = r.G->CursorCol();
    CHECK_FALSE(r.G->Resync(0, 0, true));
    CHECK(r.G->CursorRow() == row);
    CHECK(r.G->CursorCol() == col);
    CHECK(r.Cp(1, 2) == U'x');
    CHECK(r.Known(0) == 0);
}

TEST_CASE("Resync makes the rows trusted for good") {
    Rig r;
    r.Feed("\x1b[A");  // CUU on the first row: the terminal may be lower
    REQUIRE_FALSE(r.G->RowsTrusted());
    r.G->Resync(1, 0, true);
    CHECK(r.G->RowsTrusted());
    r.Feed("\x1b[B\x1b[3S\x1b[2;3r\x1b[2;1H");  // CUD, SU, DECSTBM, CUP
    CHECK(r.G->RowsTrusted());
}

TEST_CASE("Resync forgets the saved cursor, REP, width marks and the path") {
    Rig r;
    r.Feed(
        "\x1b[3;4H\x1b"
        "7");            // DECSC at (2,3)
    r.Feed("\xD0\xB6");  // Cyrillic zhe: ambiguous width
    REQUIRE(r.G->WidthDoubtBefore(2, 9));
    REQUIRE(r.G->PathGlyphs(EWidthClass::Ambiguous) == 1);
    r.G->Resync(1, 2, true);
    CHECK_FALSE(r.G->WidthDoubtBefore(2, 9));
    CHECK(r.G->WidthDoubtClasses(2) == 0);
    CHECK(r.G->PathGlyphs(EWidthClass::Ambiguous) == 0);
    CHECK_FALSE(r.G->RepeatChar(1));
    CHECK(r.G->LastPrinted().Codepoint == 0);
    r.Feed(
        "\x1b"
        "8");  // DECRC: nothing saved, home
    CHECK(r.G->CursorRow() == 0);
    CHECK(r.G->CursorCol() == 0);
}

TEST_CASE("Resync keeps the scroll region and the pending wrap is cleared") {
    Rig r;
    r.Feed("\x1b[2;3r\x1b[4;10Hx");
    REQUIRE(r.G->PendingWrap());
    r.G->Resync(1, 1, true);
    CHECK_FALSE(r.G->PendingWrap());
    CHECK(r.G->ScrollTop() == 1);
    CHECK(r.G->ScrollBot() == 2);
}

TEST_CASE("a glyph makes cells known when it reaches the known part") {
    Rig r = Started();  // row 2 known from 5
    r.Feed("\x1b[3;1Hab");
    CHECK(r.Known(2) == 5);  // x=0,1: not touching
    r.Feed("\x1b[3;4Hx");    // x=3: 3+1 < 5
    CHECK(r.Known(2) == 5);
    r.Feed("y");  // x=4: 4+1 >= 5
    CHECK(r.Known(2) == 4);
    r.Feed("\x1b[3;7Hz");  // inside the known part
    CHECK(r.Known(2) == 4);
}

TEST_CASE("a glyph at the cursor of a fresh start stays known") {
    Rig r = Started();
    r.Feed("abc");  // x=5,6,7
    CHECK(r.Known(2) == 5);
}

TEST_CASE("a wide glyph counts both of its cells") {
    Rig r = Started();
    r.Feed("\x1b[3;4H");
    r.Feed("\xE6\x97\xA5");  // x=3, width 2: 3+2 >= 5
    CHECK(r.Known(2) == 3);
}

TEST_CASE("a foreign row is known again only from its last cell") {
    Rig r;
    r.G->Resync(1, 2, false);
    REQUIRE(r.Known(1) == 10);
    r.Feed("\x1b[2;9Hab");  // x=8: 8+1 < 10; x=9: 9+1 >= 10
    CHECK(r.Known(1) == 9);
}

TEST_CASE("a glyph that wraps writes the next row") {
    Rig r;
    r.G->Resync(0, 0, false);
    r.Feed("\x1b[1;10Hab");  // x=9 on row 0, then x=0 on row 1
    CHECK(r.Known(0) == 9);
    CHECK(r.Known(1) == 0);
}

TEST_CASE("EL 0 makes the cells from the cursor known") {
    Rig r;
    r.G->Resync(0, 0, false);  // row 0 foreign
    r.Feed("\x1b[1;5H\x1b[K");
    CHECK(r.Known(0) == 4);
    r.Feed("\x1b[1;7H\x1b[K");  // already known there
    CHECK(r.Known(0) == 4);
    r.Feed("\x1b[1;2H\x1b[K");
    CHECK(r.Known(0) == 1);
}

TEST_CASE("EL 1 makes the cells up to the cursor known") {
    Rig r = Started();           // row 2 known from 5
    r.Feed("\x1b[3;3H\x1b[1K");  // [0,3): a hole [3,5) stays foreign
    CHECK(r.Known(2) == 5);
    r.Feed("\x1b[3;5H\x1b[1K");  // [0,5) reaches the known part
    CHECK(r.Known(2) == 0);
}

TEST_CASE("EL 2 makes the whole row known") {
    Rig r = Started();
    r.Feed("\x1b[2K");
    CHECK(r.Known(2) == 0);
}

TEST_CASE("ED 0 makes the cursor row tail and the rows below known") {
    Rig r;
    r.G->Resync(3, 0, false);  // rows 0..3 foreign
    r.Feed("\x1b[2;4H\x1b[J");
    CHECK(r.Known(0) == 10);
    CHECK(r.Known(1) == 3);
    CHECK(r.Known(2) == 0);
    CHECK(r.Known(3) == 0);
}

TEST_CASE("ED 1 makes the rows above and the head of the cursor row known") {
    Rig r;
    r.G->Resync(3, 0, false);
    r.Feed("\x1b[3;10H\x1b[1J");
    CHECK(r.Known(0) == 0);
    CHECK(r.Known(1) == 0);
    CHECK(r.Known(2) == 0);  // [0,10) reaches the end
    CHECK(r.Known(3) == 10);
}

TEST_CASE("ED 2 makes everything known") {
    Rig r;
    r.G->Resync(3, 0, false);
    r.Feed("\x1b[2J");
    for (int row = 0; row < 4; ++row) CHECK(r.Known(row) == 0);
}

TEST_CASE(
    "ECH makes the cells it erases known when they touch the known part") {
    Rig r;
    r.G->Resync(0, 0, false);
    r.Feed("\x1b[1;3H\x1b[3X");  // [2,5) of a foreign row: apart from 10
    CHECK(r.Known(0) == 10);
    r.Feed("\x1b[1;3H\x1b[8X");  // [2,10) reaches the end
    CHECK(r.Known(0) == 2);
    r.Feed("\x1b[1;1H\x1b[1X");  // [0,1): the known part starts at 2, a gap
    CHECK(r.Known(0) == 2);
    r.Feed("\x1b[1;2H\x1b[1X");  // [1,2) touches it
    CHECK(r.Known(0) == 1);
}

TEST_CASE("ECH past the end is clamped") {
    Rig r;
    r.G->Resync(0, 0, false);
    r.Feed("\x1b[1;4H\x1b[999X");
    CHECK(r.Known(0) == 3);
}

TEST_CASE("ICH pushes the known part right") {
    Rig r = Started();  // row 2 known from 5
    r.Feed("\x1b[3;3H\x1b[2@");
    CHECK(r.Known(2) == 7);
    r.Feed("\x1b[3;3H\x1b[99@");  // clamped to the cells that exist
    CHECK(r.Known(2) == 10);
}

TEST_CASE("ICH inside the known part changes nothing") {
    Rig r = Started();
    r.Feed("\x1b[3;7H\x1b[2@");
    CHECK(r.Known(2) == 5);
    r.Feed("\x1b[3;6H\x1b[2@");  // x == KnownFrom
    CHECK(r.Known(2) == 5);
}

TEST_CASE("DCH pulls the known part left, not past the cursor") {
    Rig r = Started();
    r.Feed("\x1b[3;1H\x1b[2P");
    CHECK(r.Known(2) == 3);
    r.Feed("\x1b[3;2H\x1b[99P");  // x=1, deletes to the end
    CHECK(r.Known(2) == 1);
}

TEST_CASE("DCH inside the known part changes nothing") {
    Rig r = Started();
    r.Feed("\x1b[3;8H\x1b[2P");
    CHECK(r.Known(2) == 5);
}

TEST_CASE("the value goes with its row on a scroll at the bottom") {
    Rig r;
    r.G->Resync(1, 3, true);  // 10, 3, 0, 0
    r.Feed("\x1b[4;1H\n");    // LF at the bottom row scrolls
    CHECK(r.Known(0) == 3);
    CHECK(r.Known(1) == 0);
    CHECK(r.Known(2) == 0);
    CHECK(r.Known(3) == 0);
}

TEST_CASE("the value goes with its row on SU and SD") {
    Rig r;
    r.G->Resync(1, 3, true);  // 10, 3, 0, 0
    r.Feed("\x1b[1S");
    CHECK(r.Known(0) == 3);
    CHECK(r.Known(1) == 0);
    r.Feed("\x1b[2T");  // [0,0,3,0]: the new rows are ordinary
    CHECK(r.Known(0) == 0);
    CHECK(r.Known(1) == 0);
    CHECK(r.Known(2) == 3);
    CHECK(r.Known(3) == 0);
}

TEST_CASE("the value goes with its row on IL and DL") {
    Rig r;
    r.G->Resync(1, 3, true);  // 10, 3, 0, 0
    r.Feed("\x1b[1L");        // at row 1: [10, 0, 3, 0]
    CHECK(r.Known(0) == 10);
    CHECK(r.Known(1) == 0);
    CHECK(r.Known(2) == 3);
    r.Feed("\x1b[1;1H\x1b[1M");  // at row 0: [0, 3, 0, 0]
    CHECK(r.Known(0) == 0);
    CHECK(r.Known(1) == 3);
    CHECK(r.Known(2) == 0);
}

TEST_CASE("the value goes with its row inside a scroll region") {
    Rig r;
    r.G->Resync(1, 3, true);         // 10, 3, 0, 0
    r.Feed("\x1b[2;3r\x1b[3;1H\n");  // rows 1..2 scroll up
    CHECK(r.Known(0) == 10);         // outside the region
    CHECK(r.Known(1) == 0);          // row 2's value
    CHECK(r.Known(2) == 0);          // the new row
}

TEST_CASE("RIS makes everything known, on both screens") {
    Rig r = Started();
    r.Feed(
        "\x1b"
        "c");
    for (int row = 0; row < 4; ++row) CHECK(r.Known(row) == 0);
    CHECK(r.G->RowsTrusted());
}

TEST_CASE("the alt screen starts known and the main screen keeps its values") {
    Rig r = Started();
    r.Feed("\x1b[?1049h");
    for (int row = 0; row < 4; ++row) CHECK(r.Known(row) == 0);
    r.Feed("\x1b[2;4Hq");
    r.Feed("\x1b[?1049l");
    CHECK(r.Known(0) == 10);
    CHECK(r.Known(1) == 10);
    CHECK(r.Known(2) == 5);
    CHECK(r.Known(3) == 0);
}

TEST_CASE("writes on the alt screen leave the main screen's values alone") {
    Rig r = Started();
    r.Feed("\x1b[?47h\x1b[1;1H\x1b[2K\x1b[3;1H\x1b[2K\x1b[?47l");
    CHECK(r.Known(0) == 10);
    CHECK(r.Known(2) == 5);
}

TEST_CASE("Resize widening keeps a foreign row foreign whole") {
    Rig r = Started();  // 10, 10, 5, 0
    REQUIRE(r.G->Resize(14, 4).has_value());
    CHECK(r.Known(0) == 14);
    CHECK(r.Known(1) == 14);
    CHECK(r.Known(2) == 5);
    CHECK(r.Known(3) == 0);
}

TEST_CASE("Resize narrowing clamps the value to the new width") {
    Rig r = Started();
    REQUIRE(r.G->Resize(4, 4).has_value());
    CHECK(r.Known(0) == 4);
    CHECK(r.Known(1) == 4);
    CHECK(r.Known(2) == 4);  // was 5: the known part is cut off
    CHECK(r.Known(3) == 0);
}

TEST_CASE("Resize in rows moves the values with the rows that stay") {
    Rig r;
    r.G->Resync(1, 3, true);  // 10, 3, 0, 0
    r.Feed("\x1b[4;1H");
    REQUIRE(r.G->Resize(10, 2).has_value());  // the bottom two rows stay
    CHECK(r.Known(0) == 0);
    CHECK(r.Known(1) == 0);

    Rig q;
    q.G->Resync(2, 3, true);                  // 10, 10, 3, 0
    REQUIRE(q.G->Resize(10, 3).has_value());  // drops row 0
    CHECK(q.Known(0) == 10);
    CHECK(q.Known(1) == 3);
    CHECK(q.Known(2) == 0);
    REQUIRE(q.G->Resize(10, 5).has_value());
    CHECK(q.Known(3) == 0);
    CHECK(q.Known(4) == 0);
}

TEST_CASE("Resize reaches the alt screen's rows too") {
    Rig r;
    r.Feed("\x1b[?1049h");
    r.G->SetPossiblyDiverged(false);
    REQUIRE(r.G->Resize(12, 4).has_value());
    for (int row = 0; row < 4; ++row) CHECK(r.Known(row) == 0);
}

TEST_CASE("KnownFrom is not part of a snapshot") {
    // The same cells and cursor; only the rows' bookkeeping differs.
    Rig a;
    a.Feed("\x1b[1;1Habc\x1b[2;1Hdef\x1b[3;1Hghi\x1b[4;1H");
    Rig b;
    b.Feed("\x1b[1;1Habc\x1b[2;1Hdef\x1b[3;1Hghi");
    b.G->Resync(3, 0, false);
    REQUIRE(b.Known(0) == 10);
    REQUIRE(a.Known(0) == 0);
    auto sa = Snapshot::Create(*a.G);
    auto sb = Snapshot::Create(*b.G);
    REQUIRE(sa.has_value());
    REQUIRE(sb.has_value());
    std::unique_ptr<Snapshot> oa(sa.value());
    std::unique_ptr<Snapshot> ob(sb.value());
    const auto ba = oa->Bytes();
    const auto bb = ob->Bytes();
    REQUIRE(ba.size() == bb.size());
    CHECK(std::equal(ba.begin(), ba.end(), bb.begin()));
}

// R7: ED 3 erases the scrollback on a terminal, not the screen (the grid's
// own comment); a foreign row stays foreign.
TEST_CASE("R7 ED 3 does not make foreign rows known") {
    Rig r(10, 4);
    r.Feed("one\r\ntwo\r\nthree");
    REQUIRE(r.G->Resync(2, 5, true));
    r.Feed("\x1b[3J");
    CHECK(r.G->KnownFrom(0) == 10);
}

TEST_CASE("R10 resync blanks the rows below with the default background") {
    Rig r(10, 3);
    r.Feed("\x1b[41m");
    REQUIRE(r.G->Resync(0, 0, true));
    CHECK(r.G->CellAt(1, 0).BgKind == EColorKind::Default);
    CHECK(r.G->CellAt(0, 5).BgKind == EColorKind::Default);
}
