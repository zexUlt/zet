// RowsTrusted() and a line feed under a coloured pen before the first row
// sync (design.md §9, "Строки до первой синхронизации"): the terminal may be
// on its bottom row, scroll, and fill the new row with the pen background,
// while the mirror only moves its cursor onto a blank row.
#include <doctest/doctest.h>

#include <memory>
#include <string_view>

#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

struct Rig {
    std::unique_ptr<Grid> G;
    VtParser P;

    Rig() {
        auto r = Grid::Create(10, 5, 0);
        REQUIRE(r.has_value());
        G.reset(r.value());
    }
    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }
};

}  // namespace

TEST_CASE(
    "lf bg drift: a coloured line feed above the bottom row loses the rows") {
    for (const char* seq : {"\n",
                            "\x1b"
                            "D",
                            "\x1b"
                            "E"}) {
        Rig r;
        r.Feed("\x1b[100m");
        CHECK(r.G->RowsTrusted());
        r.Feed(seq);
        CHECK_FALSE(r.G->RowsTrusted());
    }
}

TEST_CASE("lf bg drift: auto-wrap under a coloured pen loses the rows") {
    Rig r;
    r.Feed("\x1b[100m");
    r.Feed("0123456789a");
    CHECK_FALSE(r.G->RowsTrusted());
}

TEST_CASE("lf bg drift: the default background keeps the rows") {
    for (const char* seq : {"\n",
                            "\x1b"
                            "D",
                            "\x1b"
                            "E"}) {
        Rig r;
        r.Feed(seq);
        CHECK(r.G->RowsTrusted());
    }
    Rig r;
    r.Feed("0123456789a");
    CHECK(r.G->RowsTrusted());
}

TEST_CASE("lf bg drift: after a sync a coloured line feed changes nothing") {
    Rig r;
    r.Feed("\x1b[2J\x1b[1;1H");  // whole-screen clear and an absolute row
    REQUIRE(r.G->RowsTrusted());
    r.Feed(
        "\x1b[100m\n\x1b"
        "D\x1b"
        "E");
    CHECK(r.G->RowsTrusted());
}

TEST_CASE("lf bg drift: VT, FF, REP across the edge, a wide glyph wrap") {
    for (const char* seq : {"\x0b", "\x0c"}) {
        Rig r;
        r.Feed("\x1b[100m");
        r.Feed(seq);
        CHECK_FALSE(r.G->RowsTrusted());
    }
    {
        Rig r;  // pending wrap resolved by REP
        r.Feed("\x1b[41m0123456789");
        REQUIRE(r.G->RowsTrusted());
        r.Feed("\x1b[2b");
        CHECK_FALSE(r.G->RowsTrusted());
    }
    {
        Rig r;  // wide glyph that does not fit in the last column
        r.Feed("\x1b[41m012345678");
        REQUIRE(r.G->RowsTrusted());
        r.Feed("\xe3\x81\x82");
        CHECK_FALSE(r.G->RowsTrusted());
    }
}

TEST_CASE("lf bg drift: indexed 256 and RGB backgrounds count") {
    for (const char* sgr :
         {"\x1b[48;5;200m", "\x1b[48;2;1;2;3m", "\x1b[42m", "\x1b[105m"}) {
        Rig r;
        r.Feed(sgr);
        r.Feed("\n");
        CHECK_FALSE(r.G->RowsTrusted());
    }
    Rig r;  // SGR 49 returns to the default
    r.Feed("\x1b[41m\x1b[49m\n");
    CHECK(r.G->RowsTrusted());
}
