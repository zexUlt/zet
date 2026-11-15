// WidthPolicy, width classes and the widening VS16
// (design.md §9, round C). The default policy is the width table as it always
// was.
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

#include "zet/grid/grid.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/unicode.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

std::string Utf8(char32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

constexpr char32_t ALPHA = 0x03B1;  // Greek, ambiguous: class Ambiguous
constexpr char32_t BOX = 0x2500;    // box drawing, ambiguous
constexpr char32_t PUA = 0xE0B0;    // powerline, private use
constexpr char32_t CLOUD = 0x2601;  // text glyph with an emoji form, width 1
constexpr char32_t WATCH = 0x231A;  // emoji form already wide
constexpr char32_t VS16 = 0xFE0F;
constexpr char32_t VS15 = 0xFE0E;
constexpr char32_t ZWJ = 0x200D;
constexpr char32_t MAN = 0x1F468;
constexpr char32_t WOMAN = 0x1F469;
constexpr char32_t TONE = 0x1F3FD;
constexpr char32_t THUMB = 0x1F44D;
constexpr char32_t KEYCAP = 0x20E3;
constexpr char32_t HEART = 0x2665;  // ambiguous, with an emoji form
constexpr char32_t CJK = 0x4E2D;

struct Rig {
    std::unique_ptr<Grid> G;
    VtParser P;

    explicit Rig(int cols = 10, int rows = 3) {
        auto r = Grid::Create(static_cast<std::uint16_t>(cols),
                              static_cast<std::uint16_t>(rows), 0);
        REQUIRE(r.has_value());
        G.reset(r.value());
    }

    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }
    void Put(std::initializer_list<char32_t> cps) {
        for (const char32_t cp : cps) Feed(Utf8(cp));
    }
    [[nodiscard]] const Cell& At(int row, int col) const {
        return G->CellAt(static_cast<std::uint16_t>(row),
                         static_cast<std::uint16_t>(col));
    }
    [[nodiscard]] std::uint8_t Classes(int row) const {
        return G->WidthDoubtClasses(static_cast<std::uint16_t>(row));
    }
    [[nodiscard]] std::uint16_t Known(int row) const {
        return G->KnownFrom(static_cast<std::uint16_t>(row));
    }
};

constexpr std::uint8_t A = WidthClassBit(EWidthClass::Ambiguous);
constexpr std::uint8_t CY = WidthClassBit(EWidthClass::Cyrillic);
constexpr std::uint8_t P = WidthClassBit(EWidthClass::PrivateUseBmp);
constexpr std::uint8_t PL = WidthClassBit(EWidthClass::PrivateUsePlanes);
constexpr std::uint8_t E = WidthClassBit(EWidthClass::Emoji);
constexpr std::uint8_t C = WidthClassBit(EWidthClass::Cluster);

WidthPolicy Wide(int a, int p, bool vs16) {
    WidthPolicy w;
    w.Ambiguous = static_cast<std::uint8_t>(a);
    w.PrivateUseBmp = static_cast<std::uint8_t>(p);
    w.Vs16Widens = vs16;
    return w;
}

// The pairing invariant of wide cells, on every row.
void CheckPairing(const Grid& g) {
    for (std::uint16_t r = 0; r < g.Rows(); ++r) {
        for (std::uint16_t c = 0; c < g.Cols(); ++c) {
            const Cell& cell = g.CellAt(r, c);
            if (cell.Width == 2) {
                REQUIRE(c + 1 < g.Cols());
                CHECK(g.CellAt(r, static_cast<std::uint16_t>(c + 1)).Width ==
                      0);
            } else if (cell.Width == 0) {
                REQUIRE(c > 0);
                CHECK(g.CellAt(r, static_cast<std::uint16_t>(c - 1)).Width ==
                      2);
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The default policy
// ---------------------------------------------------------------------------

TEST_CASE("the default policy is the table as it was") {
    Rig r;
    CHECK(r.G->Policy() == WidthPolicy{});
    CHECK(r.G->Policy().Ambiguous == 1);
    CHECK(r.G->Policy().PrivateUseBmp == 1);
    CHECK_FALSE(r.G->Policy().Vs16Widens);
    r.Put({ALPHA, BOX, PUA});
    CHECK(r.At(0, 0).Width == 1);
    CHECK(r.At(0, 1).Width == 1);
    CHECK(r.At(0, 2).Width == 1);
    CHECK(r.G->CursorCol() == 3);
}

TEST_CASE("the default policy marks each class on its row") {
    {
        Rig r;
        r.Put({ALPHA});
        CHECK(r.Classes(0) == A);
        CHECK(r.G->WidthDoubtBefore(0, 1));
        CHECK_FALSE(r.G->WidthDoubtBefore(0, 0));
    }
    {
        Rig r;
        r.Put({'a', PUA});
        CHECK(r.Classes(0) == P);
        CHECK(r.G->WidthDoubtBefore(0, 2));
        CHECK_FALSE(r.G->WidthDoubtBefore(0, 1));
    }
    {
        Rig r;
        r.Put({CLOUD, VS16});
        CHECK(r.Classes(0) == E);
        CHECK(r.At(0, 0).Width == 1);
        CHECK(r.G->CursorCol() == 1);  // VS16 dropped, nothing moves
        CHECK(r.G->WidthDoubtBefore(0, 1));
    }
    {
        Rig r;
        r.Put({'a'});
        CHECK(r.Classes(0) == 0);
        CHECK(r.G->WidthDoubtClasses(0) == 0);
    }
}

TEST_CASE("clusters without a verdict are flagged") {
    {
        Rig r;  // ZWJ sequence
        r.Put({MAN, ZWJ, WOMAN});
        CHECK(r.Classes(0) == C);
        CHECK(r.G->WidthDoubtBefore(0, 2));
    }
    {
        Rig r;  // skin tone modifier: marked at its own cell
        r.Put({THUMB, TONE});
        CHECK(r.Classes(0) == C);
        CHECK(r.G->WidthDoubtBefore(0, 4));
        CHECK_FALSE(r.G->WidthDoubtBefore(0, 2));
    }
    {
        Rig r;  // VS15
        r.Put({CLOUD, VS15});
        CHECK(r.Classes(0) == C);
    }
    {
        Rig r;  // keycap: the VS16 on a digit has no verdict
        r.Put({'1', VS16, KEYCAP});
        CHECK(r.Classes(0) == C);
    }
    {
        Rig r;  // VS16 after a glyph with no emoji form
        r.Put({'a', VS16});
        CHECK(r.Classes(0) == C);
    }
    {
        Rig r;  // VS16 after a glyph that is wide already
        r.Put({WATCH, VS16});
        CHECK(r.Classes(0) == C);
    }
}

TEST_CASE("the leftmost mark of all classes is the row's WidthDoubt") {
    Rig r;
    r.Put({'a', 'b', ALPHA, PUA});
    CHECK(r.Classes(0) == (A | P));
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 2));
    CHECK(r.G->WidthDoubtBefore(0, 3));
    CHECK(r.G->WidthDoubtBefore(0, 3, A));
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 3, P));
    CHECK(r.G->WidthDoubtBefore(0, 4, P));
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 4, E | C));
}

TEST_CASE("the default policy leaves the snapshot alone") {
    const auto bytes = [](bool touchPolicy) {
        Rig r(20, 4);
        if (touchPolicy) {
            r.G->SetWidthPolicy(Wide(2, 2, true));
            r.G->SetWidthPolicy(WidthPolicy{});
        }
        r.Put({'a', ALPHA, PUA, CLOUD, VS16, MAN, ZWJ, WOMAN, 0x65E5});
        r.Feed("\r\nline two");
        auto s = Snapshot::Create(*r.G);
        REQUIRE(s.has_value());
        std::unique_ptr<Snapshot> owned(s.value());
        const auto b = owned->Bytes();
        return std::string(reinterpret_cast<const char*>(b.data()), b.size());
    };
    CHECK(bytes(false) == bytes(true));
}

TEST_CASE("SetWidthPolicy normalises widths") {
    Rig r;
    WidthPolicy w;
    w.Ambiguous = 7;
    w.PrivateUseBmp = 0;
    r.G->SetWidthPolicy(w);
    CHECK(r.G->Policy().Ambiguous == 1);
    CHECK(r.G->Policy().PrivateUseBmp == 1);
    r.G->SetWidthPolicy(Wide(2, 2, false));
    CHECK(r.G->Policy().Ambiguous == 2);
    CHECK(r.G->Policy().PrivateUseBmp == 2);
}

// ---------------------------------------------------------------------------
// Ambiguous and private use, narrow vs wide
// ---------------------------------------------------------------------------

TEST_CASE("wide ambiguous makes class A two columns, not private use") {
    Rig r;
    r.G->SetWidthPolicy(Wide(2, 1, false));
    r.Put({ALPHA, PUA, 'x'});
    CHECK(r.At(0, 0).Codepoint == ALPHA);
    CHECK(r.At(0, 0).Width == 2);
    CHECK(r.At(0, 1).Width == 0);
    CHECK(r.At(0, 2).Codepoint == PUA);
    CHECK(r.At(0, 2).Width == 1);
    CHECK(r.At(0, 3).Codepoint == U'x');
    CHECK(r.G->CursorCol() == 4);
    CheckPairing(*r.G);
}

TEST_CASE("wide private use makes class P two columns, not ambiguous") {
    Rig r;
    r.G->SetWidthPolicy(Wide(1, 2, false));
    r.Put({PUA, ALPHA, BOX});
    CHECK(r.At(0, 0).Width == 2);
    CHECK(r.At(0, 2).Codepoint == ALPHA);
    CHECK(r.At(0, 2).Width == 1);
    CHECK(r.At(0, 3).Width == 1);
    CHECK(r.G->CursorCol() == 4);
    CheckPairing(*r.G);
}

TEST_CASE("the policy does not widen what the table makes wide or narrow") {
    Rig r;
    r.G->SetWidthPolicy(Wide(2, 2, true));
    r.Put({'a', 0x65E5, 0x0301});  // narrow, CJK wide, combining mark
    CHECK(r.At(0, 0).Width == 1);
    CHECK(r.At(0, 1).Width == 2);
    CHECK(r.G->CursorCol() == 3);
}

TEST_CASE("the policy applies to glyphs printed afterwards only") {
    Rig r;
    r.Put({ALPHA});
    r.G->SetWidthPolicy(Wide(2, 2, false));
    r.Put({ALPHA});
    CHECK(r.At(0, 0).Width == 1);
    CHECK(r.At(0, 1).Width == 2);
    r.G->SetWidthPolicy(WidthPolicy{});
    r.Put({ALPHA});
    CHECK(r.At(0, 1).Width == 2);
    CHECK(r.At(0, 3).Width == 1);
}

TEST_CASE("a wide ambiguous glyph wraps like any wide one") {
    Rig r(4, 3);
    r.G->SetWidthPolicy(Wide(2, 2, false));
    r.Put({'a', 'b', 'c', ALPHA});
    CHECK(r.At(0, 3).Codepoint == 0);  // no room: the glyph goes to row 1
    CHECK(r.At(1, 0).Codepoint == ALPHA);
    CHECK(r.At(1, 0).Width == 2);
    CHECK(r.G->CursorRow() == 1);
    CHECK(r.G->CursorCol() == 2);
    CheckPairing(*r.G);
}

TEST_CASE("in a one-column grid a wide ambiguous glyph is a space") {
    Rig r(1, 2);
    r.G->SetWidthPolicy(Wide(2, 2, false));
    r.Put({ALPHA});
    CHECK(r.At(0, 0).Width == 1);
    CHECK(r.At(0, 0).Codepoint == U' ');
}

TEST_CASE("Special Graphics line drawing is not widened") {
    Rig r;
    r.G->SetWidthPolicy(Wide(2, 2, false));
    r.Feed("\x1b(0q\x1b(B");  // ESC ( 0, 'q' = U+2500
    CHECK(r.At(0, 0).Codepoint == BOX);
    CHECK(r.At(0, 0).Width == 1);
    r.Put({BOX});  // the same glyph from UTF-8 is widened
    CHECK(r.At(0, 1).Width == 2);
}

TEST_CASE("REP repeats the glyph at the width of the table, not the policy") {
    Rig r;
    r.G->SetWidthPolicy(Wide(2, 2, false));
    r.Put({ALPHA});
    r.Feed("\x1b[2b");  // REP 2: two more, wide
    CHECK(r.G->CursorCol() == 6);
    CHECK(r.At(0, 4).Width == 2);
    r.G->SetWidthPolicy(WidthPolicy{});
    r.Feed("\x1b[1b");  // the terminal now sees a narrow glyph
    CHECK(r.At(0, 6).Width == 1);
    CHECK(r.G->LastPrinted().Width == 1);
}

TEST_CASE("wide glyphs from the policy overwrite like any wide glyph") {
    Rig r;
    r.Put({'a', 'b', 'c', 'd'});
    r.Feed("\x1b[1;2H");
    r.G->SetWidthPolicy(Wide(2, 2, false));
    r.Put({ALPHA});  // over b and c
    r.Put({ALPHA});  // over d and the blank: d's cell
    CheckPairing(*r.G);
    r.Feed("\x1b[1;1H");
    r.Put({'x'});
    r.Feed("\x1b[1;3H");  // into the continuation of the first wide glyph
    r.Put({'y'});
    CheckPairing(*r.G);
}

TEST_CASE("the doubt mask silences a class but not Cluster") {
    Rig r;
    WidthPolicy w;
    w.DoubtClasses = static_cast<std::uint8_t>(WIDTH_CLASSES_ALL & ~A);
    r.G->SetWidthPolicy(w);
    r.Put({ALPHA, PUA});
    CHECK(r.Classes(0) == P);
    w.DoubtClasses = 0;
    r.G->SetWidthPolicy(w);
    r.Put({CLOUD, VS16});
    CHECK(r.Classes(0) == P);  // E silenced
    r.Put({MAN, ZWJ, WOMAN});
    CHECK(r.Classes(0) == (P | C));
}

TEST_CASE("VS16 widens a narrow glyph with an emoji form") {
    Rig r;
    r.G->SetWidthPolicy(Wide(1, 1, true));
    r.Put({'a', CLOUD, VS16, 'b'});
    CHECK(r.At(0, 1).Codepoint == CLOUD);
    CHECK(r.At(0, 1).Width == 2);
    CHECK(r.At(0, 2).Width == 0);
    CHECK(r.At(0, 3).Codepoint == U'b');
    CHECK(r.G->CursorCol() == 4);
    CHECK(r.Classes(0) == 0);              // nothing left to doubt
    CHECK(r.G->LastPrinted().Width == 1);  // 'b'
    CheckPairing(*r.G);
}

TEST_CASE("without the widening policy VS16 changes nothing") {
    Rig r;
    r.Put({'a', CLOUD, VS16, 'b'});
    CHECK(r.At(0, 1).Width == 1);
    CHECK(r.At(0, 2).Codepoint == U'b');
    CHECK(r.G->CursorCol() == 3);
}

TEST_CASE("VS16 widening moves the cursor and the last glyph's width") {
    Rig r;
    r.G->SetWidthPolicy(Wide(1, 1, true));
    r.Put({CLOUD, VS16});
    CHECK(r.G->CursorCol() == 2);
    CHECK(r.G->LastPrinted().Width == 2);
    CHECK(r.G->LastPrinted().Col == 0);
    CHECK(r.G->LastPrinted().Marked);
    CHECK(r.G->ColumnSetCount() == 0);
}

TEST_CASE("VS16 next to the right margin") {
    // Glyph in the second to last column: the wide cell just fits.
    Rig r(5, 3);
    r.G->SetWidthPolicy(Wide(1, 1, true));
    r.Feed("\x1b[1;4H");
    r.Put({CLOUD, VS16});
    CHECK(r.At(0, 3).Width == 2);
    CHECK(r.At(0, 4).Width == 0);
    CHECK(r.G->CursorCol() == 4);
    CHECK(r.G->PendingWrap());
    CHECK(r.Classes(0) == 0);
    CheckPairing(*r.G);
}

TEST_CASE("VS16 only widens a glyph printed right before it") {
    Rig r;
    r.G->SetWidthPolicy(Wide(1, 1, true));
    r.Put({CLOUD});
    r.Feed("\r");
    r.Put({VS16});
    CHECK(r.At(0, 0).Width == 1);
    CHECK(r.G->CursorCol() == 0);

    Rig q;
    q.G->SetWidthPolicy(Wide(1, 1, true));
    q.Put({CLOUD, 0x0301, VS16});  // a mark in between
    CHECK(q.At(0, 0).Width == 1);
    CHECK(q.G->CursorCol() == 1);
}

TEST_CASE("VS16 does not widen what is not text with an emoji form") {
    for (const char32_t base : {char32_t{'a'}, char32_t{'1'}, WATCH, ALPHA}) {
        Rig r;
        r.G->SetWidthPolicy(Wide(1, 1, true));
        r.Put({base, VS16, 'x'});
        const int w = base == WATCH ? 2 : 1;
        CHECK(r.At(0, 0).Width == w);
        CHECK(r.At(0, w).Codepoint == U'x');
        CHECK(r.G->CursorCol() == w + 1);
    }
}

TEST_CASE("VS16 does not widen a glyph the policy made wide already") {
    // U+00A9 has an emoji form and is ambiguous.
    Rig r;
    r.G->SetWidthPolicy(Wide(2, 1, true));
    r.Put({0x00A9, VS16, 'x'});
    CHECK(r.At(0, 0).Width == 2);
    CHECK(r.At(0, 2).Codepoint == U'x');
    CHECK(r.G->CursorCol() == 3);
    CheckPairing(*r.G);
}

TEST_CASE("VS16 widening over a wide glyph keeps the pairing") {
    Rig r;
    r.Put({0x65E5, 0x65E5});  // wide at 0-1 and 2-3
    r.Feed("\x1b[1;1H");
    r.G->SetWidthPolicy(Wide(1, 1, true));
    r.Put({CLOUD});  // over the first wide glyph's head: its tail blanks
    r.Put({VS16});   // the cursor cell (col 1) becomes the continuation
    CHECK(r.At(0, 0).Width == 2);
    CHECK(r.At(0, 1).Width == 0);
    CheckPairing(*r.G);

    Rig q;
    q.Put({'a', 0x65E5, 0x65E5});  // wide at 1-2 and 3-4
    q.Feed("\x1b[1;1H");
    q.G->SetWidthPolicy(Wide(1, 1, true));
    q.Put({CLOUD, VS16});  // continuation lands on a wide head at col 1
    CheckPairing(*q.G);
}

TEST_CASE("VS16 widening counts as a cell change") {
    Rig r;
    r.G->SetWidthPolicy(Wide(1, 1, true));
    r.Put({CLOUD});
    const auto before = r.G->CellChangeCount();
    r.Put({VS16});
    CHECK(r.G->CellChangeCount() > before);
}

TEST_CASE("ZWJ, skin tone and VS15 are not widened by any policy") {
    Rig r;
    r.G->SetWidthPolicy(Wide(2, 2, true));
    r.Put({CLOUD, VS15, MAN, ZWJ, WOMAN, THUMB, TONE});
    CHECK(r.At(0, 0).Width == 1);
    CHECK(r.At(0, 1).Codepoint == MAN);
    CHECK(r.Classes(0) == C);
}

// ---------------------------------------------------------------------------
// Verdicts: Clear/Forget
// ---------------------------------------------------------------------------

TEST_CASE("a narrow verdict drops the class from every row") {
    Rig r;
    r.Put({'x', ALPHA, 'a'});
    r.Feed("\r\n");
    r.Put({PUA, ALPHA});
    r.Feed("\x1b[?1049h");
    r.Put({ALPHA});
    r.Feed("\x1b[?1049l");
    REQUIRE(r.Classes(0) == A);
    REQUIRE(r.Classes(1) == (A | P));
    r.G->ClearWidthClass(EWidthClass::Ambiguous);
    CHECK(r.Classes(0) == 0);
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 9));
    CHECK(r.Classes(1) == P);
    // The mark that remains is the private use glyph's, at column 0.
    CHECK(r.G->WidthDoubtBefore(1, 1));
    CHECK_FALSE(r.G->WidthDoubtBefore(1, 0));
    r.Feed("\x1b[?1049h");
    CHECK(r.Classes(0) == 0);  // the alt screen's row was cleared too
    CHECK(r.Known(1) == 0);    // nothing became foreign
}

TEST_CASE("a wide verdict makes the rows holding the class foreign") {
    Rig r;
    r.Put({ALPHA});
    r.Feed("\r\n");
    r.Put({PUA, ALPHA});
    r.Feed("\r\n");
    r.Put({'p'});
    r.G->ForgetWidthClass(EWidthClass::Ambiguous);
    CHECK(r.Known(0) == 10);
    CHECK(r.Known(1) == 10);
    CHECK(r.Known(2) == 0);
    CHECK(r.Classes(0) == 0);
    CHECK(r.Classes(1) == P);  // other classes stay
    // The contents are kept.
    CHECK(r.At(0, 0).Codepoint == ALPHA);
}

TEST_CASE("verdicts leave Cluster marks and unmarked rows alone") {
    Rig r;
    r.Put({MAN, ZWJ, WOMAN});
    r.G->ClearWidthClass(EWidthClass::Ambiguous);
    r.G->ForgetWidthClass(EWidthClass::Ambiguous);
    CHECK(r.Classes(0) == C);
    CHECK(r.Known(0) == 0);
}

// ---------------------------------------------------------------------------
// Marks travel with their rows and shift with DCH
// ---------------------------------------------------------------------------

TEST_CASE("class marks go with their row on scrolls") {
    Rig r(10, 4);
    r.Put({ALPHA});
    r.Feed("\r\n");
    r.Put({PUA});
    r.Feed("\x1b[1T");  // SD: rows move down
    CHECK(r.Classes(0) == 0);
    CHECK(r.Classes(1) == A);
    CHECK(r.Classes(2) == P);
    r.Feed("\x1b[2S");  // SU: rows move up
    CHECK(r.Classes(0) == P);
    CHECK(r.Classes(1) == 0);
}

TEST_CASE("DCH shifts a mark left of where it was") {
    Rig r;
    r.Put({'a', 'b', 'c', 'd', ALPHA});  // mark at column 4
    r.Put({PUA});                        // and at 5
    r.Feed("\x1b[1;2H\x1b[3P");
    CHECK(r.G->WidthDoubtBefore(0, 2, A));
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 1, A));
    CHECK(r.G->WidthDoubtBefore(0, 2, P));
}

TEST_CASE("ED and EL 2 clear the marks of the rows they erase") {
    Rig r;
    r.Put({ALPHA});
    r.Feed("\x1b[2K");
    CHECK(r.Classes(0) == 0);
    r.Put({ALPHA});
    r.Feed("\x1b[2J");
    CHECK(r.Classes(0) == 0);
}

TEST_CASE("the existing counters do not move with the new bookkeeping") {
    Rig r;
    r.Put({ALPHA, CLOUD, VS16});
    CHECK(r.G->ColumnSetCount() == 0);
    CHECK(r.G->ScrollCount() == 0);
    r.Feed("\r");
    CHECK(r.G->ColumnSetCount() == 1);
}

// R5: Special Graphics glyphs are not sized by the A policy.
TEST_CASE("R5 Special Graphics line drawing still marks the row") {
    Rig r;
    r.Feed("\r\x1b(0q\x1b(B");
    CHECK(r.G->WidthDoubtBefore(0, 1));
}

// R6: IRM must insert as many cells as the glyph takes under the policy.
TEST_CASE("R6 insert mode makes room for a glyph the policy widens") {
    Rig r;
    WidthPolicy w;
    w.Ambiguous = 2;
    r.G->SetWidthPolicy(w);
    r.Feed("abc\r\x1b[4h");
    r.Put({ALPHA});
    CHECK(r.At(0, 0).Codepoint == ALPHA);
    CHECK(r.At(0, 0).Width == 2);
    CHECK(r.At(0, 2).Codepoint == U'a');
    CHECK(r.At(0, 3).Codepoint == U'b');
    CHECK(r.At(0, 4).Codepoint == U'c');
}

// Mutant killers (survivors of the grid suite).
TEST_CASE("R8 resync drops the width marks of the alt screen too") {
    Rig r(10, 3);
    r.Feed("\x1b[?47h");
    r.Put({ALPHA});
    r.Feed("\x1b[?47l");
    REQUIRE(r.G->Resync(0, 0, true));
    r.Feed("\x1b[?47h");
    REQUIRE(r.At(0, 0).Codepoint == ALPHA);
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 1));
}

TEST_CASE(
    "R9 resync does not leave the head of a wide glyph cut by the blank tail") {
    Rig r(10, 3);
    r.Feed("abc");
    r.Put({CJK});
    REQUIRE(r.At(0, 3).Width == 2);
    REQUIRE(r.G->Resync(0, 4, true));
    CHECK(r.At(0, 3).Width == 1);
    CHECK(r.At(0, 3).Codepoint == 0);
}

TEST_CASE("R11 a glyph VS16 widens next to the known tail joins it") {
    Rig r(10, 3);
    WidthPolicy w;
    w.Vs16Widens = true;
    r.G->SetWidthPolicy(w);
    REQUIRE(r.G->Resync(0, 5, true));
    r.Feed("\x1b[4G");
    r.Put({HEART});
    REQUIRE(r.G->KnownFrom(0) == 5);
    r.Put({VS16});
    REQUIRE(r.At(0, 3).Width == 2);
    CHECK(r.G->KnownFrom(0) == 3);
}

TEST_CASE("R12 a class mark keeps the leftmost glyph of the class") {
    Rig r;
    r.Put({ALPHA});
    r.Feed("abcd");
    r.Put({ALPHA});
    CHECK(r.G->WidthDoubtBefore(0, 2, A));
}

TEST_CASE("R13 clearing a class keeps the leftmost mark of the others") {
    Rig r;
    r.Put({PUA});
    r.Feed("abcd");
    r.Put({ALPHA});
    r.G->ClearWidthClass(EWidthClass::Cluster);
    CHECK(r.G->WidthDoubtBefore(0, 2));
}

TEST_CASE("R14 resize keeps the class marks") {
    Rig r;
    r.Put({ALPHA});
    REQUIRE(r.G->Resize(12, 3).has_value());
    CHECK(r.G->WidthDoubtBefore(0, 1, A));
}

TEST_CASE("R15 a class verdict reaches the alt screen") {
    Rig r;
    r.Feed("\x1b[?47h");
    r.Put({ALPHA});
    r.G->ClearWidthClass(EWidthClass::Ambiguous);
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 1));
    r.Feed("\r\n");
    r.Put({ALPHA});
    r.G->ForgetWidthClass(EWidthClass::Ambiguous);
    CHECK_FALSE(r.G->WidthDoubtBefore(1, 1));
    CHECK(r.G->KnownFrom(1) == 10);
}

TEST_CASE("R2 the unknown sequence changes nothing else") {
    Rig r(10, 3);
    r.Put({ALPHA});
    const auto col = r.G->CursorCol();
    r.Feed("\x1b[2`");
    CHECK(r.P.UnknownSeqCount() == 1);
    CHECK(r.G->CursorCol() == col);
}

TEST_CASE("R6 insert mode makes room for a VS16 that widens") {
    Rig r;
    WidthPolicy w;
    w.Vs16Widens = true;
    r.G->SetWidthPolicy(w);
    r.Feed("abc\r\x1b[4h");
    r.Put({CLOUD, VS16});
    CHECK(r.At(0, 0).Codepoint == CLOUD);
    CHECK(r.At(0, 0).Width == 2);
    CHECK(r.At(0, 2).Codepoint == U'a');
    CHECK(r.At(0, 3).Codepoint == U'b');
    CHECK(r.At(0, 4).Codepoint == U'c');
}

// ---------------------------------------------------------------------------
// Cyrillic, private use in the BMP and in planes 15-16: one class each
// ---------------------------------------------------------------------------

namespace {

constexpr char32_t ZHE = 0x0436;    // Cyrillic, ambiguous
constexpr char32_t YO_UP = 0x0401;  // Ё
constexpr char32_t A_UP = 0x0410;   // А
constexpr char32_t YA = 0x044F;     // я
constexpr char32_t YO = 0x0451;     // ё
constexpr char32_t DJE = 0x0402;    // Ђ: Cyrillic, but narrow in the table
constexpr char32_t PUA_BMP_LAST = 0xF8FF;
constexpr char32_t PUA_P15 = 0xF024B;  // nf-md-folder
constexpr char32_t PUA_P15_LAST = 0xFFFFD;
constexpr char32_t PUA_P16 = 0x10F000;
constexpr char32_t PUA_P16_LAST = 0x10FFFD;

enum class EPolicyField { Ambiguous, Cyrillic, Bmp, Planes };

WidthPolicy Only(EPolicyField f) {
    WidthPolicy w;
    switch (f) {
        case EPolicyField::Ambiguous:
            w.Ambiguous = 2;
            break;
        case EPolicyField::Cyrillic:
            w.Cyrillic = 2;
            break;
        case EPolicyField::Bmp:
            w.PrivateUseBmp = 2;
            break;
        case EPolicyField::Planes:
            w.PrivateUsePlanes = 2;
            break;
    }
    return w;
}

// The width one glyph gets, alone on a fresh row, under `w`.
int WidthOf(char32_t cp, const WidthPolicy& w) {
    Rig r;
    r.G->SetWidthPolicy(w);
    r.Put({cp});
    return r.At(0, 0).Width;
}

}  // namespace

TEST_CASE("the default policy has every class narrow, as the table says") {
    const WidthPolicy w;
    CHECK(w.Ambiguous == 1);
    CHECK(w.Cyrillic == 1);
    CHECK(w.PrivateUseBmp == 1);
    CHECK(w.PrivateUsePlanes == 1);
    for (const char32_t cp : {ZHE, YO_UP, A_UP, YA, YO, ALPHA, BOX, PUA,
                              PUA_P15, PUA_P16, PUA_BMP_LAST}) {
        CHECK_MESSAGE(WidthOf(cp, w) == 1, "U+", std::hex, unsigned(cp));
    }
}

TEST_CASE("the Cyrillic class is exactly Ё, А-я and ё") {
    for (char32_t cp = 0; cp < 0x3000; ++cp) {
        const bool expected =
            cp == YO_UP || (cp >= A_UP && cp <= YA) || cp == YO;
        CHECK_MESSAGE(IsCyrillicAmbiguous(cp) == expected, "U+", std::hex,
                      unsigned(cp));
        // Every glyph of the class is East Asian ambiguous in the table.
        if (expected) CHECK(IsAmbiguousWidth(cp));
    }
    CHECK_FALSE(IsCyrillicAmbiguous(DJE));
}

TEST_CASE("the private use ranges split into the BMP and planes 15-16") {
    CHECK(IsPrivateUseBmp(0xE000));
    CHECK(IsPrivateUseBmp(PUA_BMP_LAST));
    CHECK_FALSE(IsPrivateUseBmp(0xDFFF));
    CHECK_FALSE(IsPrivateUseBmp(0xF900));
    CHECK_FALSE(IsPrivateUseBmp(PUA_P15));
    CHECK(IsPrivateUsePlanes(0xF0000));
    CHECK(IsPrivateUsePlanes(PUA_P15_LAST));
    CHECK_FALSE(IsPrivateUsePlanes(0xFFFFE));
    CHECK(IsPrivateUsePlanes(0x100000));
    CHECK(IsPrivateUsePlanes(PUA_P16_LAST));
    CHECK_FALSE(IsPrivateUsePlanes(0x10FFFE));
    CHECK_FALSE(IsPrivateUsePlanes(PUA));
    for (const char32_t cp :
         {char32_t{0xE000}, PUA, PUA_BMP_LAST, PUA_P15, PUA_P16_LAST}) {
        CHECK(IsPrivateUse(cp));
    }
}

TEST_CASE("wide Cyrillic widens Cyrillic only, the rest of A stays narrow") {
    const WidthPolicy w = Only(EPolicyField::Cyrillic);
    for (const char32_t cp : {ZHE, YO_UP, A_UP, YA, YO})
        CHECK_MESSAGE(WidthOf(cp, w) == 2, "U+", std::hex, unsigned(cp));
    for (const char32_t cp :
         {ALPHA, BOX, HEART, PUA, PUA_BMP_LAST, PUA_P15, PUA_P16, DJE})
        CHECK_MESSAGE(WidthOf(cp, w) == 1, "U+", std::hex, unsigned(cp));
    Rig r;
    r.G->SetWidthPolicy(w);
    r.Put({ZHE, ALPHA});
    CHECK(r.G->CursorCol() == 3);
}

TEST_CASE("wide PrivateUseBmp widens U+E000-F8FF only") {
    const WidthPolicy w = Only(EPolicyField::Bmp);
    for (const char32_t cp : {char32_t{0xE000}, PUA, PUA_BMP_LAST})
        CHECK_MESSAGE(WidthOf(cp, w) == 2, "U+", std::hex, unsigned(cp));
    for (const char32_t cp :
         {ZHE, YO, ALPHA, BOX, PUA_P15, PUA_P15_LAST, PUA_P16, PUA_P16_LAST})
        CHECK_MESSAGE(WidthOf(cp, w) == 1, "U+", std::hex, unsigned(cp));
}

TEST_CASE("wide PrivateUsePlanes widens planes 15 and 16, plane 16 too") {
    const WidthPolicy w = Only(EPolicyField::Planes);
    for (const char32_t cp : {char32_t{0xF0000}, PUA_P15, PUA_P15_LAST,
                              char32_t{0x100000}, PUA_P16, PUA_P16_LAST})
        CHECK_MESSAGE(WidthOf(cp, w) == 2, "U+", std::hex, unsigned(cp));
    for (const char32_t cp : {ZHE, ALPHA, BOX, PUA, PUA_BMP_LAST})
        CHECK_MESSAGE(WidthOf(cp, w) == 1, "U+", std::hex, unsigned(cp));
}

TEST_CASE("wide Ambiguous leaves Cyrillic and private use narrow") {
    const WidthPolicy w = Only(EPolicyField::Ambiguous);
    for (const char32_t cp : {ALPHA, BOX, HEART})
        CHECK_MESSAGE(WidthOf(cp, w) == 2, "U+", std::hex, unsigned(cp));
    for (const char32_t cp : {ZHE, YO_UP, YO, PUA, PUA_P15, PUA_P16})
        CHECK_MESSAGE(WidthOf(cp, w) == 1, "U+", std::hex, unsigned(cp));
}

TEST_CASE("each glyph marks its own class, the rest of A is none of them") {
    const std::pair<char32_t, std::uint8_t> marks[] = {
        {ZHE, CY}, {YO_UP, CY},   {YO, CY},      {ALPHA, A},
        {PUA, P},  {PUA_P15, PL}, {PUA_P16, PL}, {BOX, A},
    };
    for (const auto& [cp, cls] : marks) {
        Rig r;
        r.Put({cp});
        CHECK_MESSAGE(r.Classes(0) == cls, "U+", std::hex, unsigned(cp));
        CHECK(r.G->WidthDoubtBefore(0, 2, cls));
        CHECK_FALSE(r.G->WidthDoubtBefore(
            0, 2, static_cast<std::uint8_t>(WIDTH_CLASSES_ALL & ~cls)));
    }
    Rig r;
    r.Put({'a', ZHE, ALPHA, PUA, PUA_P15});
    CHECK(r.Classes(0) == (CY | A | P | PL));
    CHECK(r.G->WidthDoubtBefore(0, 2));
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 1));
}

TEST_CASE("SetWidthPolicy normalises the new fields") {
    Rig r;
    WidthPolicy w;
    w.Cyrillic = 3;
    w.PrivateUseBmp = 2;
    w.PrivateUsePlanes = 0;
    r.G->SetWidthPolicy(w);
    CHECK(r.G->Policy().Cyrillic == 1);
    CHECK(r.G->Policy().PrivateUseBmp == 2);
    CHECK(r.G->Policy().PrivateUsePlanes == 1);
}

TEST_CASE("a wide glyph of a new class wraps and widens as any wide one") {
    for (const EPolicyField f :
         {EPolicyField::Cyrillic, EPolicyField::Bmp, EPolicyField::Planes}) {
        Rig r(4, 3);
        r.G->SetWidthPolicy(Only(f));
        const char32_t cp = f == EPolicyField::Cyrillic ? ZHE
                            : f == EPolicyField::Bmp    ? PUA
                                                        : PUA_P15;
        r.Put({'a', 'b', 'c', cp});
        CHECK(r.At(0, 3).Codepoint == 0);  // no room: the glyph goes to row 1
        CHECK(r.At(1, 0).Codepoint == cp);
        CHECK(r.At(1, 0).Width == 2);
        CheckPairing(*r.G);
    }
}

TEST_CASE("verdicts on the new classes reach their rows only") {
    Rig r;
    r.Put({ZHE, PUA, PUA_P15});
    r.Feed("\r\n");
    r.Put({ZHE});
    r.Feed("\r\n");
    r.Put({PUA_P15});
    REQUIRE(r.Classes(0) == (CY | P | PL));
    r.G->ClearWidthClass(EWidthClass::Cyrillic);
    CHECK(r.Classes(0) == (P | PL));
    CHECK(r.Classes(1) == 0);
    CHECK(r.Classes(2) == PL);
    r.G->ForgetWidthClass(EWidthClass::PrivateUsePlanes);
    CHECK(r.Classes(0) == P);
    CHECK(r.Classes(2) == 0);
    CHECK(r.Known(0) == 10);
    CHECK(r.Known(2) == 10);
    CHECK(r.Known(1) == 0);
}

TEST_CASE("the doubt mask silences each new class on its own") {
    for (const auto& [cp, bit] :
         {std::pair{ZHE, CY}, std::pair{PUA, P}, std::pair{PUA_P15, PL}}) {
        Rig r;
        WidthPolicy w;
        w.DoubtClasses = static_cast<std::uint8_t>(WIDTH_CLASSES_ALL & ~bit);
        r.G->SetWidthPolicy(w);
        r.Put({cp, ALPHA});
        CHECK(r.Classes(0) == A);
    }
}

TEST_CASE("private use planes stop at the code points next to the ranges") {
    CHECK_FALSE(IsPrivateUsePlanes(0xEFFFF));
    CHECK(IsPrivateUsePlanes(0xF0000));
    CHECK(IsPrivateUsePlanes(0xFFFFD));
    CHECK_FALSE(IsPrivateUsePlanes(0xFFFFE));
    CHECK_FALSE(IsPrivateUsePlanes(0xFFFFF));
    CHECK(IsPrivateUsePlanes(0x100000));
    CHECK(IsPrivateUsePlanes(0x10FFFD));
    CHECK_FALSE(IsPrivateUsePlanes(0x10FFFE));
    CHECK_FALSE(IsPrivateUsePlanes(0x10FFFF));
    CHECK_FALSE(IsPrivateUsePlanes(0x110000));
}
