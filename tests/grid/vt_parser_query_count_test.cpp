// The parser's count of the primary device attributes queries it dispatches
// (design.md §9, "Калибровка зеркала запросом позиции курсора"): CSI c, CSI 0 c
// and ESC Z.
#include <doctest/doctest.h>

#include <memory>
#include <string_view>

#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

struct Fixture {
    Fixture() {
        auto r = Grid::Create(10, 3, 0);
        REQUIRE(r.has_value());
        G.reset(r.value());
    }
    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }
    std::unique_ptr<Grid> G;
    VtParser P;
};

}  // namespace

TEST_CASE("CSI c and CSI 0 c count as primary device attributes queries") {
    Fixture f;
    f.Feed("\x1b[c");
    CHECK(f.P.Da1QueryCount() == 1);
    f.Feed("\x1b[0c");
    CHECK(f.P.Da1QueryCount() == 2);
}

TEST_CASE("xterm looks only at the first parameter of CSI c") {
    Fixture f;
    f.Feed("\x1b[0;1c");
    CHECK(f.P.Da1QueryCount() == 1);
    f.Feed("\x1b[;1c");
    CHECK(f.P.Da1QueryCount() == 2);
}

TEST_CASE("ESC Z (DECID) counts as a primary device attributes query") {
    Fixture f;
    f.Feed("\x1bZ");
    CHECK(f.P.Da1QueryCount() == 1);
    f.Feed("a\x1bZb\x1bZ");
    CHECK(f.P.Da1QueryCount() == 3);
    // CSI Z is cursor backward tab, not DECID.
    f.Feed("\x1b[Z");
    CHECK(f.P.Da1QueryCount() == 3);
}

TEST_CASE("DSR is not a device attributes query") {
    Fixture f;
    f.Feed("\x1b[6n\x1b[5n\x1b[?6n");
    CHECK(f.P.Da1QueryCount() == 0);
}

TEST_CASE("a device attributes query split between feeds counts once") {
    Fixture f;
    f.Feed("\x1b[0");
    CHECK(f.P.Da1QueryCount() == 0);
    f.Feed("c");
    CHECK(f.P.Da1QueryCount() == 1);
}

TEST_CASE("DA2, DA3 and other forms of CSI c are not DA1") {
    Fixture f;
    f.Feed("\x1b[>c");
    f.Feed("\x1b[>0c");
    f.Feed("\x1b[=c");
    f.Feed("\x1b[?62c");  // a reply, not a query
    f.Feed("\x1b[1c");
    f.Feed("\x1b[0 c");
    CHECK(f.P.Da1QueryCount() == 0);
}

TEST_CASE(
    "query text inside OSC, DCS, APC, PM and SOS strings does not count") {
    Fixture f;
    f.Feed("\x1b]0;[c\x07");
    f.Feed("\x1bP1$r[c\x1b\\");
    f.Feed("\x1b_[c\x1b\\");
    f.Feed("\x1b^[c\x1b\\");
    f.Feed("\x1bX[c\x1b\\");
    CHECK(f.P.Da1QueryCount() == 0);
    // The parser is back in Ground after each: a real query counts again.
    f.Feed("\x1b[c");
    CHECK(f.P.Da1QueryCount() == 1);
}

TEST_CASE("the counts survive Reset") {
    Fixture f;
    f.Feed("\x1b[c");
    f.P.Reset();
    CHECK(f.P.Da1QueryCount() == 1);
}
