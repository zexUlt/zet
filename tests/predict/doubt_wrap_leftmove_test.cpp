// Width doubt (design.md §9, step 5) on a wrap and on a move left over a
// doubtful glyph. The grid-level cases check the state, the rig cases the
// restore against a terminal that draws East Asian ambiguous glyphs wide.
#include <doctest/doctest.h>

#include <string>

#include "overlay_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;

namespace {

const char* const ZHE = "\xd0\xb6";  // U+0436, East Asian ambiguous

void Wide(Client& c) {
    grid::WidthPolicy wide;
    wide.Ambiguous = 2;
    c.Real.G->SetWidthPolicy(wide);
    c.Ref.G->SetWidthPolicy(wide);
}

struct Mirror {
    Term T;
    Mirror(int cols, int rows) : T(cols, rows) {}
    void Feed(std::string_view s) { T.Feed(s); }
    [[nodiscard]] bool Before(std::uint16_t row, std::uint16_t col) const {
        return T.G->WidthDoubtBefore(row, col);
    }
};

}  // namespace

TEST_CASE("a wrap hands the doubt to the continuation row") {
    Mirror m(5, 3);
    m.Feed(std::string("ab") + ZHE + "cde");
    REQUIRE(m.T.G->CursorRow() == 1);
    CHECK(m.Before(1, 1));
    CHECK_FALSE(m.Before(1, 0));
    CHECK(m.Before(0, 4));
}

TEST_CASE("a wrap from a row without doubt leaves the next row clean") {
    Mirror m(5, 3);
    m.Feed("abcdefg");
    REQUIRE(m.T.G->CursorRow() == 1);
    CHECK_FALSE(m.Before(1, 2));
}

TEST_CASE("a wrap passes the doubt on through a chain of rows") {
    Mirror m(3, 4);
    m.Feed(std::string(ZHE) + "abcdefg");
    REQUIRE(m.T.G->CursorRow() == 2);
    CHECK(m.Before(2, 1));
}

TEST_CASE("backspace over a doubtful glyph leaves the column untrusted") {
    Mirror m(10, 3);
    m.Feed(std::string(ZHE));
    REQUIRE(m.T.G->CursorCol() == 1);
    CHECK_FALSE(m.Before(0, 0));
    m.Feed("\x08");
    REQUIRE(m.T.G->CursorCol() == 0);
    CHECK(m.Before(0, 0));
    CHECK(m.Before(1, 0));
}

TEST_CASE("CUB over a doubtful glyph leaves the column untrusted") {
    Mirror m(10, 3);
    m.Feed(std::string("ab") + ZHE + "\x1b[3D");
    REQUIRE(m.T.G->CursorCol() == 0);
    CHECK(m.Before(0, 0));
}

TEST_CASE("a left move with no doubtful glyph left of it is harmless") {
    Mirror m(10, 3);
    m.Feed(std::string("ab\x08\x1b[1D"));
    CHECK_FALSE(m.Before(0, 0));
    m.Feed(std::string("cd") + ZHE);
    m.Feed("\x1b[2C\x08");  // a move right, then left: past the glyph only
    CHECK(m.Before(0, 5));
}

TEST_CASE("an absolute column set clears the untrusted column") {
    for (const char* set : {"\r", "\x1b[1G", "\x1b[1;1H", "\x1b[1;1f",
                            "\x1b"
                            "c"}) {
        Mirror m(10, 3);
        m.Feed(std::string("ab") + ZHE + "\x08\x08\x08");
        REQUIRE(m.Before(1, 0));
        m.Feed(set);
        CAPTURE(set);
        CHECK_FALSE(m.Before(1, 0));
    }
}

TEST_CASE("probe: a doubtful glyph wraps the line, the row below is typed on") {
    Client c(5, 3, EDisplayMode::Always);
    Wide(c);
    c.Agent("\r");
    c.Agent(std::string("abc") + ZHE + "$ ");
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Key("c");
    c.Key("d");
    c.Undo();
    CHECK(SameScreen(*c.Real.G, *c.Ref.G));
}

TEST_CASE("probe: backspace over a doubtful glyph, then typing") {
    Client c(34, 6, EDisplayMode::Always);
    Wide(c);
    c.Agent("\r$ ");
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Key("c");
    c.Key("d");
    c.Agent(std::string("\r") + ZHE + "\x08");
    c.Undo();
    CHECK(SameScreen(*c.Real.G, *c.Ref.G));
}

TEST_CASE("CUB over a doubtful glyph, landing at column 0, then typing") {
    Client c(34, 6, EDisplayMode::Always);
    Wide(c);
    c.Agent("\r$ ");
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Key("c");
    c.Key("d");
    c.Agent(std::string("\r") + ZHE + "\x1b[D");
    c.Undo();
    CHECK(SameScreen(*c.Real.G, *c.Ref.G));
}

TEST_CASE("typing after the left move is not predicted at the mirror column") {
    Client c(34, 6, EDisplayMode::Always);
    Wide(c);
    c.Agent("\r$ ");
    c.Key("a");
    c.Agent("a");
    c.R.Ack();
    c.Key("B");
    c.Agent("B");
    c.R.Ack();
    c.Agent(std::string("\r") + ZHE + "\x08");
    c.Key("x");
    c.Key("y");
    c.Undo();
    CHECK(SameScreen(*c.Real.G, *c.Ref.G));
}
