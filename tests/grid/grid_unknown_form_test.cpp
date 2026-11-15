// Forms the mirror cannot follow (design.md §9, round C, step C4b): input a
// terminal may take another way than the grid does counts as an unknown
// sequence while the grid draws what it always drew; modes the grid does not
// model, double width rows, SGR attributes the pen cannot hold and the path of
// non-ASCII glyphs are remembered for the client's gates.
#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "zet/grid/grid.hpp"
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
    [[nodiscard]] std::uint64_t Unknown() const { return P.UnknownSeqCount(); }
    [[nodiscard]] char32_t At(int row, int col) const {
        return G
            ->CellAt(static_cast<std::uint16_t>(row),
                     static_cast<std::uint16_t>(col))
            .Codepoint;
    }
    [[nodiscard]] std::uint16_t Known(int row) const {
        return G->KnownFrom(static_cast<std::uint16_t>(row));
    }
};

/// Bytes that feed the parser how a program's output would, with NULs inside.
std::string S(std::string_view v) { return std::string(v); }

}  // namespace

// ---------------------------------------------------------------------------
// Malformed UTF-8 and C1 controls decoded from it
// ---------------------------------------------------------------------------

TEST_CASE("malformed UTF-8 counts as an unknown sequence and draws U+FFFD") {
    struct Case {
        std::string Bytes;
        int Cells;  ///< cells the mirror fills with U+FFFD
    };
    const Case cases[] = {
        {"\xC3x", 1},             // a lead and an ASCII byte
        {"\xE2\x82x", 1},         // cut short by ASCII
        {"\xE0\x80\x80", 1},      // overlong
        {"\xED\xA0\x80", 1},      // a surrogate
        {"\xF4\x90\x80\x80", 1},  // beyond U+10FFFF
        {"\xFF", 1},              // never valid
        {"\xFE", 1},
    };
    for (const Case& c : cases) {
        CAPTURE(c.Bytes);
        Rig r;
        r.Feed(c.Bytes);
        CHECK(r.Unknown() == 1);
        CHECK(r.At(0, 0) == 0xFFFD);
    }
}

TEST_CASE("a lead byte cut off by ESC, CAN or SUB counts") {
    for (const char* cut : {"\x1b[m", "\x18", "\x1a"}) {
        CAPTURE(cut);
        Rig r;
        r.Feed(S("\xE2\x82") + cut);
        CHECK(r.Unknown() == 1);
    }
}

TEST_CASE("a stray continuation byte and 8-bit bytes in a sequence count") {
    Rig r;
    r.Feed("\x80");
    CHECK(r.Unknown() == 1);
    r.Feed("\xBF");
    CHECK(r.Unknown() == 2);
    // Inside CSI the byte is ignored by the mirror; terminals differ.
    r.Feed("\x1b[1\x80;2H");
    CHECK(r.Unknown() == 3);
    // The byte is ignored, the rest of the sequence runs: CUP 1;2.
    CHECK(r.G->CursorRow() == 0);
    CHECK(r.G->CursorCol() == 1);
}

TEST_CASE("valid UTF-8 counts nothing") {
    Rig r;
    r.Feed("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\xC2\xA0");
    CHECK(r.Unknown() == 0);
    CHECK(r.At(0, 0) == 0xE9);
    CHECK(r.At(0, 1) == 0x20AC);
}

TEST_CASE("C1 controls from UTF-8 count and draw nothing") {
    for (const char* c1 :
         {"\xC2\x80", "\xC2\x9B", "\xC2\x9C", "\xC2\x9D", "\xC2\x9F"}) {
        CAPTURE(c1);
        Rig r;
        r.Feed(S("a") + c1 + "b");
        CHECK(r.Unknown() == 1);
        CHECK(r.At(0, 0) == U'a');
        CHECK(r.At(0, 1) == U'b');
        CHECK(r.G->CursorCol() == 2);
    }
    Rig r;
    r.Feed("\xC2\xA0");  // U+00A0 is no control
    CHECK(r.Unknown() == 0);
}

// ---------------------------------------------------------------------------
// CSI forms outside the table
// ---------------------------------------------------------------------------

TEST_CASE("CSI forms the grid does not handle count and do not run") {
    const char* forms[] = {
        "\x1b[3 C",   // intermediate: GSS-like, not CUF
        "\x1b[3$A",   // intermediate
        "\x1b[3 !A",  // two intermediates
        "\x1b[!!p",   // two intermediates on DECSTR's final
        "\x1b[ !q",   // two intermediates on DECSCUSR's final
        "\x1b[>3C",   // private marker on a cursor move
        "\x1b[=3C",   "\x1b[<3C",    "\x1b[?3C",
        "\x1b[3?C",   // a marker after a parameter
        "\x1b[3:2C",  // a colon outside SGR
        "\x1b[2:3H",  "\x1b[2;3:1H",
        "\x1b[1 s",    // `CSI Pl;Pr s` is not SCP
        "\x1b[?25$p",  // DECRQM: private marker and intermediate
        "\x1b[1 r",    // DECSTBM with an intermediate
        "\x1b[!q",     // DECSTR's intermediate on another final
        "\x1b[2 p",    // a space on the DECSTR final
    };
    for (const char* f : forms) {
        CAPTURE(f);
        Rig r;
        r.Feed("\x1b[3;3H");
        REQUIRE(r.G->CursorRow() == 2);
        REQUIRE(r.G->CursorCol() == 2);
        const std::uint64_t before = r.Unknown();
        r.Feed(f);
        CHECK(r.Unknown() == before + 1);
        CHECK(r.G->CursorRow() == 2);
        CHECK(r.G->CursorCol() == 2);
        CHECK(r.G->Rows() == 4);
        CHECK(r.At(2, 2) == 0);
    }
}

TEST_CASE("CSI forms the grid handles count nothing") {
    const char* forms[] = {
        "\x1b[2 q",
        "\x1b[!p",
        "\x1b[?25h",
        "\x1b[?25l",
        "\x1b[1;2H",
        "\x1b[4:3m",
        "\x1b[38:2::1:2:3m",
        "\x1b[s",
        "\x1b[u",
        "\x1b[3C",
        "\x1b[2;5r",
        "\x1b[4h",
        "\x1b[20l",
        "\x1b[6n",
        "\x1b[c",
        "\x1b[0c",
        "\x1b[?1049h\x1b[?1049l",
        "\x1b[?7l",
    };
    for (const char* f : forms) {
        CAPTURE(f);
        Rig r;
        r.Feed(f);
        CHECK(r.Unknown() == 0);
    }
}

TEST_CASE("a CSI form that goes unknown leaves the unknown final alone") {
    // The handler of C would have moved the cursor; the form with an
    // intermediate must not reach it, nor the form with a private marker.
    Rig r;
    r.Feed("ab\x1b[ 5C\x1b[>5C\x1b[$5C");
    CHECK(r.G->CursorCol() == 2);
    CHECK(r.Unknown() == 3);
}

// ---------------------------------------------------------------------------
// REP after zero width
// ---------------------------------------------------------------------------

TEST_CASE("REP after a dropped zero-width code point is unknown but repeats") {
    Rig r;
    r.Feed("a\xCC\x81\x1b[2b");  // a, COMBINING ACUTE, REP 2
    CHECK(r.Unknown() == 1);
    CHECK(r.At(0, 0) == U'a');
    CHECK(r.At(0, 1) == U'a');
    CHECK(r.At(0, 2) == U'a');
    CHECK(r.G->CursorCol() == 3);
}

TEST_CASE("REP after a joiner or a soft hyphen is unknown") {
    for (const char* mark : {"\xE2\x80\x8D", "\xEF\xB8\x8F", "\xC2\xAD"}) {
        CAPTURE(mark);
        Rig r;
        r.Feed(S("a") + mark + "\x1b[b");
        CHECK(r.Unknown() == 1);
    }
}

TEST_CASE("REP after a glyph, with the mark before it, is not unknown") {
    Rig r;
    r.Feed(
        "a\xCC\x81"
        "b\x1b[b");
    CHECK(r.Unknown() == 0);
    CHECK(r.At(0, 1) == U'b');
    CHECK(r.At(0, 2) == U'b');
    r.Feed("\x1b[b");
    CHECK(r.Unknown() == 0);
}

TEST_CASE("REP with nothing to repeat stays unknown") {
    Rig r;
    r.Feed("\x1b[2b");
    CHECK(r.Unknown() == 1);
    CHECK(r.Known(0) == 10);
}

// ---------------------------------------------------------------------------
// Soft hyphen
// ---------------------------------------------------------------------------

namespace {
constexpr std::uint8_t CLUSTER = 1U
                                 << static_cast<unsigned>(EWidthClass::Cluster);
}  // namespace

TEST_CASE("a soft hyphen marks the cell before it as doubtful") {
    Rig r;
    r.Feed("ab\xC2\xAD");
    // The mark is on the cell of 'b' (column 1): it doubts what is right of it.
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 1));
    CHECK(r.G->WidthDoubtBefore(0, 2));
    CHECK((r.G->WidthDoubtClasses(0) & CLUSTER) != 0);
    CHECK_FALSE(r.G->ColumnDoubt());
    CHECK(r.G->CursorCol() == 2);
    CHECK(r.Unknown() == 0);
}

TEST_CASE("a soft hyphen in column 0 doubts the cursor's column") {
    Rig r;
    r.Feed("\xC2\xAD");
    CHECK(r.G->ColumnDoubt());
    CHECK((r.G->WidthDoubtClasses(0) & CLUSTER) != 0);
    CHECK(r.G->WidthDoubtBefore(0, 1));
    CHECK(r.G->CursorCol() == 0);
    // An absolute column ends the doubt.
    r.Feed("\r");
    CHECK_FALSE(r.G->ColumnDoubt());
}

TEST_CASE("a soft hyphen with a wrap pending doubts the cursor's column") {
    Rig r;
    r.Feed("0123456789");
    REQUIRE(r.G->PendingWrap());
    REQUIRE_FALSE(r.G->ColumnDoubt());
    r.Feed("\xC2\xAD");
    CHECK(r.G->ColumnDoubt());
    CHECK((r.G->WidthDoubtClasses(0) & CLUSTER) != 0);
    CHECK(r.G->PendingWrap());
}

TEST_CASE("a soft hyphen mid-line leaves the column of the cursor alone") {
    Rig r;
    r.Feed(
        "abc\xC2\xAD"
        "d");
    CHECK_FALSE(r.G->ColumnDoubt());
    CHECK(r.G->CursorCol() == 4);
}

// ---------------------------------------------------------------------------
// Modes the grid does not model
// ---------------------------------------------------------------------------

TEST_CASE("DECLRMM, reverse wrap and 132 columns set and clear a flag") {
    struct Case {
        const char* Set;
        const char* Clear;
        EUnmodelledMode Mode;
    };
    const Case cases[] = {
        {"\x1b[?69h", "\x1b[?69l", EUnmodelledMode::LeftRightMargins},
        {"\x1b[?45h", "\x1b[?45l", EUnmodelledMode::ReverseWrap},
        {"\x1b[?3h", "\x1b[?3l", EUnmodelledMode::ColumnMode},
    };
    for (const Case& c : cases) {
        CAPTURE(c.Set);
        Rig r;
        CHECK_FALSE(r.G->AnyUnmodelledMode());
        r.Feed(c.Set);
        CHECK(r.G->AnyUnmodelledMode());
        CHECK(r.G->UnmodelledModeSet(c.Mode));
        CHECK(r.Unknown() == 1);
        r.Feed(c.Clear);
        CHECK_FALSE(r.G->AnyUnmodelledMode());
        CHECK_FALSE(r.G->UnmodelledModeSet(c.Mode));
        CHECK(r.Unknown() == 2);
    }
}

TEST_CASE("each mode counts once, others in the same sequence do not") {
    Rig r;
    r.Feed("\x1b[?25;69h");
    CHECK(r.Unknown() == 1);
    r.Feed("\x1b[?69;45;3l");
    CHECK(r.Unknown() == 4);
    r.Feed("\x1b[?7;25h");
    CHECK(r.Unknown() == 4);
}

TEST_CASE("the flag stays while another unmodelled mode is set") {
    Rig r;
    r.Feed("\x1b[?69h\x1b[?45h\x1b[?69l");
    CHECK(r.G->AnyUnmodelledMode());
    CHECK(r.G->UnmodelledModeSet(EUnmodelledMode::ReverseWrap));
    CHECK_FALSE(r.G->UnmodelledModeSet(EUnmodelledMode::LeftRightMargins));
    r.Feed("\x1b[?45l");
    CHECK_FALSE(r.G->AnyUnmodelledMode());
}

TEST_CASE("DECANM counts as unknown and sets nothing") {
    Rig r;
    r.Feed("\x1b[?2l");
    CHECK(r.Unknown() == 1);
    CHECK_FALSE(r.G->AnyUnmodelledMode());
}

TEST_CASE("DECSLRM sets the margins flag and does not save the cursor") {
    Rig r;
    r.Feed("\x1b[3;4H\x1b[2;5s");
    CHECK(r.Unknown() == 1);
    CHECK(r.G->UnmodelledModeSet(EUnmodelledMode::LeftRightMargins));
    CHECK(r.G->SavedCursorMainState().Row == 0);
    CHECK(r.G->SavedCursorMainState().Col == 0);
    r.Feed("\x1b[?69l");
    CHECK_FALSE(r.G->AnyUnmodelledMode());
    // The bare form is SCP: it saves and counts nothing.
    const std::uint64_t before = r.Unknown();
    r.Feed("\x1b[s");
    CHECK(r.Unknown() == before);
    CHECK(r.G->SavedCursorMainState().Row == 2);
    CHECK(r.G->SavedCursorMainState().Col == 3);
    CHECK_FALSE(r.G->AnyUnmodelledMode());
}

TEST_CASE("ESC % @ leaves UTF-8 and only ESC % G returns") {
    Rig r;
    r.Feed("\x1b%@");
    CHECK(r.G->AnyUnmodelledMode());
    CHECK(r.G->UnmodelledModeSet(EUnmodelledMode::Utf8Off));
    CHECK(r.Unknown() == 1);
    // Nothing else the program sends undoes it.
    r.Feed("\x1b[?69h\x1b[?69l\x1b[?3l\x1b[?45l");
    CHECK(r.G->UnmodelledModeSet(EUnmodelledMode::Utf8Off));
    r.Feed("\x1bG");
    CHECK(r.G->UnmodelledModeSet(EUnmodelledMode::Utf8Off));
    r.Feed("\x1b%G");
    CHECK_FALSE(r.G->AnyUnmodelledMode());
    CHECK(r.Unknown() == 1 + 4 + 1 + 1);
}

TEST_CASE("ESC % G leaves the DEC modes set") {
    Rig r;
    r.Feed("\x1b[?69h\x1b%@\x1b%G");
    CHECK(r.G->UnmodelledModeSet(EUnmodelledMode::LeftRightMargins));
    CHECK_FALSE(r.G->UnmodelledModeSet(EUnmodelledMode::Utf8Off));
}

TEST_CASE("RIS and DECSTR clear the DEC modes but not ESC % @") {
    for (const char* reset : {"\033c", "\x1b[!p"}) {
        CAPTURE(reset);
        Rig r;
        r.Feed("\x1b[?69h\x1b[?45h\x1b[?3h\x1b%@");
        REQUIRE(r.G->AnyUnmodelledMode());
        r.Feed(reset);
        CHECK_FALSE(r.G->UnmodelledModeSet(EUnmodelledMode::LeftRightMargins));
        CHECK_FALSE(r.G->UnmodelledModeSet(EUnmodelledMode::ReverseWrap));
        CHECK_FALSE(r.G->UnmodelledModeSet(EUnmodelledMode::ColumnMode));
        CHECK(r.G->UnmodelledModeSet(EUnmodelledMode::Utf8Off));
        r.Feed("\x1b%G");
        CHECK_FALSE(r.G->AnyUnmodelledMode());
    }
}

TEST_CASE("RIS and DECSTR clear a margin set by DECSLRM") {
    for (const char* reset : {"\033c", "\x1b[!p"}) {
        Rig r;
        r.Feed("\x1b[2;5s");
        REQUIRE(r.G->AnyUnmodelledMode());
        r.Feed(reset);
        CHECK_FALSE(r.G->AnyUnmodelledMode());
    }
}

// ---------------------------------------------------------------------------
// Double width and double height rows
// ---------------------------------------------------------------------------

TEST_CASE("ESC # 3, 4 and 6 make the cursor row foreign") {
    for (char fin : {'3', '4', '6'}) {
        CAPTURE(fin);
        Rig r;
        r.Feed("\x1b[3;2H");
        r.Feed(S("\x1b#") + fin);
        CHECK(r.Known(2) == 10);
        CHECK(r.Known(0) == 0);
        CHECK(r.Known(1) == 0);
        CHECK(r.Known(3) == 0);
        CHECK(r.Unknown() == 1);
        // The program's erase of the whole row brings it back.
        r.Feed("\x1b[2K");
        CHECK(r.Known(2) == 0);
    }
}

TEST_CASE("ESC # 5 leaves the row known") {
    Rig r;
    r.Feed("\x1b[3;2H\x1b#5");
    CHECK(r.Known(2) == 0);
    CHECK(r.Known(0) == 0);
}

TEST_CASE(
    "ESC # 3 over a row the mirror knows only partly makes it all foreign") {
    Rig r;
    REQUIRE(r.G->Resync(1, 4, true));
    REQUIRE(r.Known(1) == 4);
    r.Feed("\x1b#6");
    CHECK(r.Known(1) == 10);
    CHECK(r.Known(0) == 10);  // Resync made the rows above foreign
}

// ---------------------------------------------------------------------------
// SGR attributes the pen cannot hold
// ---------------------------------------------------------------------------

TEST_CASE("SGR 53, 6 and 58 with a true colour set the unmodelled mark") {
    const char* sets[] = {
        "\x1b[53m",         "\x1b[6m",  "\x1b[58:2::1:2:3m", "\x1b[58:2:1:2:3m",
        "\x1b[58;2;1;2;3m", "\x1b[51m", "\x1b[52m",          "\x1b[26m",
        "\x1b[73m",         "\x1b[74m", "\x1b[1;53;4m",      "\x1b[0;53m",
    };
    for (const char* s : sets) {
        CAPTURE(s);
        Rig r;
        CHECK_FALSE(r.G->HasUnmodelledSgr());
        r.Feed(s);
        CHECK(r.G->HasUnmodelledSgr());
        CHECK(r.Unknown() == 0);
    }
}

TEST_CASE("modelled SGR sets no unmodelled mark") {
    const char* sets[] = {
        "\x1b[1m",           "\x1b[5m",      "\x1b[4:3m",
        "\x1b[58:5:3m",      "\x1b[58;5;3m", "\x1b[38;2;1;2;3m",
        "\x1b[48:2::1:2:3m", "\x1b[21m",     "\x1b[7;9;31;42m",
        "\x1b[59m",          "\x1b[25m",     "\x1b[55m",
    };
    for (const char* s : sets) {
        CAPTURE(s);
        Rig r;
        r.Feed(s);
        CHECK_FALSE(r.G->HasUnmodelledSgr());
    }
}

TEST_CASE("an unmodelled mark lasts until SGR 0 or the attribute's reset") {
    struct Case {
        const char* Set;
        const char* Reset;
    };
    const Case cases[] = {
        {"\x1b[6m", "\x1b[25m"},          {"\x1b[26m", "\x1b[50m"},
        {"\x1b[51m", "\x1b[54m"},         {"\x1b[52m", "\x1b[54m"},
        {"\x1b[53m", "\x1b[55m"},         {"\x1b[58:2::1:2:3m", "\x1b[59m"},
        {"\x1b[58;2;1;2;3m", "\x1b[59m"}, {"\x1b[73m", "\x1b[75m"},
        {"\x1b[74m", "\x1b[75m"},
    };
    for (const Case& c : cases) {
        CAPTURE(c.Set);
        Rig r;
        r.Feed(c.Set);
        // Text and unrelated attributes leave it.
        r.Feed("abc\x1b[1m\x1b[31m\r\n\x1b[22;39m");
        CHECK(r.G->HasUnmodelledSgr());
        r.Feed(c.Reset);
        CHECK_FALSE(r.G->HasUnmodelledSgr());
        r.Feed(c.Set);
        r.Feed("\x1b[0m");
        CHECK_FALSE(r.G->HasUnmodelledSgr());
        r.Feed(c.Set);
        r.Feed("\x1b[m");
        CHECK_FALSE(r.G->HasUnmodelledSgr());
        r.Feed(c.Set);
        r.Feed("\x1b[1;0m");
        CHECK_FALSE(r.G->HasUnmodelledSgr());
        r.Feed(c.Set);
        r.Feed("\033c");
        CHECK_FALSE(r.G->HasUnmodelledSgr());
        r.Feed(c.Set);
        r.Feed("\x1b[!p");
        CHECK_FALSE(r.G->HasUnmodelledSgr());
    }
}

TEST_CASE("another attribute's reset leaves the mark") {
    Rig r;
    r.Feed("\x1b[53m");
    for (const char* other :
         {"\x1b[25m", "\x1b[50m", "\x1b[54m", "\x1b[59m", "\x1b[75m",
          "\x1b[22m", "\x1b[24m", "\x1b[39m", "\x1b[49m", "\x1b[27m"}) {
        CAPTURE(other);
        r.Feed(other);
        CHECK(r.G->HasUnmodelledSgr());
    }
}

TEST_CASE("each attribute keeps its own mark") {
    Rig r;
    r.Feed("\x1b[53;6m");
    REQUIRE(r.G->HasUnmodelledSgr());
    r.Feed("\x1b[25m");
    CHECK(r.G->HasUnmodelledSgr());
    r.Feed("\x1b[55m");
    CHECK_FALSE(r.G->HasUnmodelledSgr());
    r.Feed("\x1b[53;6;58;2;1;2;3m");
    r.Feed("\x1b[55;25m");
    CHECK(r.G->HasUnmodelledSgr());
    r.Feed("\x1b[59m");
    CHECK_FALSE(r.G->HasUnmodelledSgr());
}

TEST_CASE("SGR 0 inside a parameter list ends the mark set before it only") {
    Rig r;
    r.Feed("\x1b[53;0m");
    CHECK_FALSE(r.G->HasUnmodelledSgr());
    r.Feed("\x1b[0;53m");
    CHECK(r.G->HasUnmodelledSgr());
}

TEST_CASE("DECRC brings back the marks DECSC saw and keeps the present ones") {
    Rig r;
    r.Feed(
        "\x1b[53m\x1b"
        "7\x1b[0m");
    REQUIRE_FALSE(r.G->HasUnmodelledSgr());
    r.Feed(
        "\x1b"
        "8");
    CHECK(r.G->HasUnmodelledSgr());

    Rig q;
    q.Feed(
        "\x1b"
        "7\x1b[53m");
    q.Feed(
        "\x1b"
        "8");
    CHECK(q.G->HasUnmodelledSgr());
}

TEST_CASE("the unmodelled mark does not change what the grid draws") {
    Rig a;
    Rig b;
    a.Feed("\x1b[53;6mab\x1b[1mcd\x1b[0mef");
    b.Feed("\x1b[6mab\x1b[1mcd\x1b[0mef");
    for (int c = 0; c < 6; ++c) {
        CHECK(a.At(0, c) == b.At(0, c));
    }
    CHECK(a.G->PenAttrs() == b.G->PenAttrs());
}

// ---------------------------------------------------------------------------
// The path of non-ASCII glyphs
// ---------------------------------------------------------------------------

TEST_CASE("printable ASCII leaves the path clean") {
    Rig r;
    CHECK_FALSE(r.G->PathHasNonAscii());
    r.Feed("hello world~ \x1b[3D\x1b[Kabc\x1b[2C\x08\t\n");
    CHECK_FALSE(r.G->PathHasNonAscii());
}

TEST_CASE("a glyph from U+0080 up marks the path") {
    for (const char* glyph : {"\xC3\xA9", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80",
                              "\xC2\xA0", "\xEE\x82\xA0", "\xD0\x96"}) {
        CAPTURE(glyph);
        Rig r;
        r.Feed(S("ab") + glyph);
        CHECK(r.G->PathHasNonAscii());
    }
}

TEST_CASE("a dropped zero-width code point marks the path") {
    for (const char* mark : {"\xCC\x81", "\xC2\xAD", "\xE2\x80\x8D",
                             "\xEF\xB8\x8F", "\xE2\x80\x8B"}) {
        CAPTURE(mark);
        Rig r;
        r.Feed(S("ab") + mark);
        CHECK(r.G->PathHasNonAscii());
    }
}

TEST_CASE("a malformed byte printed as U+FFFD marks the path") {
    Rig r;
    r.Feed("ab\xFF");
    CHECK(r.G->PathHasNonAscii());
}

TEST_CASE("DEC Special Graphics line drawing marks the path") {
    Rig r;
    r.Feed("\x1b(0q\x1b(B");
    CHECK(r.G->PathHasNonAscii());
    Rig q;
    q.Feed("\x1b(0A\x1b(B");  // 'A' is not mapped and stays ASCII
    CHECK_FALSE(q.G->PathHasNonAscii());
}

TEST_CASE("the mark survives erasing the glyph with ASCII or EL 1") {
    Rig r;
    r.Feed("\xE4\xB8\xAD");
    REQUIRE(r.G->PathHasNonAscii());
    r.Feed(
        "\x1b[2D"
        "ab");
    CHECK(r.At(0, 0) == U'a');
    CHECK(r.G->PathHasNonAscii());

    Rig q;
    q.Feed("x\xE4\xB8\xAD\x1b[1K");
    CHECK(q.At(0, 1) == 0);
    CHECK(q.G->PathHasNonAscii());

    Rig s;
    s.Feed("x\xCC\x81\x1b[3D  \x1b[3D");
    CHECK(s.G->PathHasNonAscii());
}

TEST_CASE("the mark survives relative moves, erases and attributes") {
    Rig r;
    r.Feed("\xC3\xA9");
    for (const char* f : {"\x1b[2C", "\x1b[1D", "\x1b[K", "\x1b[2K", "\x1b[2J",
                          "\x1b[1m", "\x1b[B", "\x1b[A", "\t", "\x08", "\n",
                          "\x1b[X", "\x1b[2@", "\x1b[2P"}) {
        CAPTURE(f);
        r.Feed(f);
        CHECK(r.G->PathHasNonAscii());
    }
}

TEST_CASE("an absolute column clears the mark") {
    const char* clears[] = {
        "\r", "\x1b[5G", "\x1b[2;3H", "\x1b[2;3f", "\033c", "\x1b[E", "\x1b[F",
    };
    for (const char* f : clears) {
        CAPTURE(f);
        Rig r;
        r.Feed("\xE4\xB8\xAD");
        REQUIRE(r.G->PathHasNonAscii());
        r.Feed(f);
        CHECK_FALSE(r.G->PathHasNonAscii());
    }
}

TEST_CASE("a resync clears the mark") {
    Rig r;
    r.Feed("\xE4\xB8\xAD");
    REQUIRE(r.G->PathHasNonAscii());
    REQUIRE(r.G->Resync(1, 2, true));
    CHECK_FALSE(r.G->PathHasNonAscii());
}

TEST_CASE("the mark begins again after a clearing move") {
    Rig r;
    r.Feed("\xE4\xB8\xAD\rab");
    CHECK_FALSE(r.G->PathHasNonAscii());
    r.Feed("\xC3\xA9");
    CHECK(r.G->PathHasNonAscii());
    r.Feed("\x1b[1;1Hx");
    CHECK_FALSE(r.G->PathHasNonAscii());
}

TEST_CASE("DECRC returns the mark of the save") {
    Rig r;
    r.Feed(
        "ab\x1b"
        "7\xE4\xB8\xAD\x1b"
        "8");
    CHECK_FALSE(r.G->PathHasNonAscii());
    Rig q;
    q.Feed(
        "\xE4\xB8\xAD\x1b"
        "7\r"
        "ab\x1b"
        "8");
    CHECK(q.G->PathHasNonAscii());
}

TEST_CASE("the mark of the path sits beside the column doubt, apart from it") {
    Rig r;
    r.Feed("\xC3\xA9");
    CHECK(r.G->PathHasNonAscii());
    CHECK_FALSE(r.G->ColumnDoubt());
    Rig q;
    q.Feed("\xE2\x80\xA6\x1b[D");  // an ambiguous glyph, CUB over it
    CHECK(q.G->ColumnDoubt());
    CHECK(q.G->PathHasNonAscii());
}
