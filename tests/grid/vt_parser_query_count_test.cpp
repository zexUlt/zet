// The parser's counts of the terminal queries it dispatches (design.md §9,
// "Калибровка зеркала запросом позиции курсора"): CSI 6 n and CSI c.
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

TEST_CASE("CSI 6 n counts as a position query") {
    Fixture f;
    f.Feed("\x1b[6n");
    CHECK(f.P.Dsr6QueryCount() == 1);
    CHECK(f.P.Da1QueryCount() == 0);
    f.Feed("a\x1b[6nb\x1b[6n");
    CHECK(f.P.Dsr6QueryCount() == 3);
}

TEST_CASE(
    "a position query split between feeds counts once, on the last byte") {
    Fixture f;
    f.Feed("\x1b[");
    f.Feed("6");
    CHECK(f.P.Dsr6QueryCount() == 0);
    f.Feed("n");
    CHECK(f.P.Dsr6QueryCount() == 1);
}

TEST_CASE("only the plain CSI 6 n is a position query") {
    Fixture f;
    f.Feed("\x1b[?6n");  // DECXCPR
    f.Feed("\x1b[5n");   // device status
    f.Feed("\x1b[n");    // no parameter
    f.Feed("\x1b[;6n");  // two parameters
    f.Feed("\x1b[6;1n");
    f.Feed("\x1b[6 n");  // intermediate
    f.Feed("\x1b[>6n");
    CHECK(f.P.Dsr6QueryCount() == 0);
}

TEST_CASE("CSI c and CSI 0 c count as primary device attributes queries") {
    Fixture f;
    f.Feed("\x1b[c");
    CHECK(f.P.Da1QueryCount() == 1);
    f.Feed("\x1b[0c");
    CHECK(f.P.Da1QueryCount() == 2);
    CHECK(f.P.Dsr6QueryCount() == 0);
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
    f.Feed("\x1b[0;1c");
    f.Feed("\x1b[0 c");
    CHECK(f.P.Da1QueryCount() == 0);
}

TEST_CASE(
    "query text inside OSC, DCS, APC, PM and SOS strings does not count") {
    Fixture f;
    f.Feed("\x1b]0;[6n[c\x07");
    f.Feed("\x1bP1$r[6n[c\x1b\\");
    f.Feed("\x1b_[6n[c\x1b\\");
    f.Feed("\x1b^[6n[c\x1b\\");
    f.Feed("\x1bX[6n[c\x1b\\");
    CHECK(f.P.Dsr6QueryCount() == 0);
    CHECK(f.P.Da1QueryCount() == 0);
    // The parser is back in Ground after each: a real query counts again.
    f.Feed("\x1b[6n\x1b[c");
    CHECK(f.P.Dsr6QueryCount() == 1);
    CHECK(f.P.Da1QueryCount() == 1);
}

TEST_CASE("the counts survive Reset") {
    Fixture f;
    f.Feed("\x1b[6n\x1b[c");
    f.P.Reset();
    CHECK(f.P.Dsr6QueryCount() == 1);
    CHECK(f.P.Da1QueryCount() == 1);
}
