// REP with nothing to repeat (design.md §9, "REP после оверлея"): the terminal
// repeats a glyph the mirror never saw, so the cursor row becomes foreign.
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
        auto r = Grid::Create(10, 4, 0);
        REQUIRE(r.has_value());
        G.reset(r.value());
    }
    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }
    [[nodiscard]] std::uint16_t Known(int row) const {
        return G->KnownFrom(static_cast<std::uint16_t>(row));
    }
};

}  // namespace

TEST_CASE("REP with an empty register makes the cursor row foreign") {
    Rig r;
    r.Feed("\x1b[3;4H\x1b[2b");
    CHECK(r.P.UnknownSeqCount() == 1);
    CHECK(r.Known(2) == 10);
    CHECK(r.Known(0) == 0);
    CHECK(r.Known(1) == 0);
    CHECK(r.Known(3) == 0);
}

TEST_CASE("REP with an empty register leaves foreign rows above untouched") {
    Rig r;
    REQUIRE(r.G->Resync(2, 5, true));
    CHECK(r.Known(1) == 10);
    CHECK(r.Known(2) == 5);
    r.Feed("\x1b[2b");
    CHECK(r.Known(1) == 10);
    CHECK(r.Known(2) == 10);
    CHECK(r.Known(3) == 0);
}

TEST_CASE("the foreign row after REP is restored by an erase") {
    for (const char* erase : {"\x1b[2K", "\x1b[2J"}) {
        Rig r;
        r.Feed("\x1b[2;3H\x1b[2b");
        REQUIRE(r.Known(1) == 10);
        r.Feed(erase);
        CHECK(r.Known(1) == 0);
    }
}

TEST_CASE("REP after a printed glyph repeats it and keeps the row known") {
    Rig r;
    r.Feed("\x1b[2;3Hx\x1b[3b");
    CHECK(r.P.UnknownSeqCount() == 0);
    CHECK(r.Known(1) == 0);
    CHECK(r.G->CellAt(1, 5).Codepoint == U'x');
}

TEST_CASE("REP right after a Resync, which clears the register, is foreign") {
    Rig r;
    r.Feed("x");
    REQUIRE(r.G->Resync(1, 0, true));
    r.Feed("\x1b[2b");
    CHECK(r.P.UnknownSeqCount() == 1);
    CHECK(r.Known(1) == 10);
}
