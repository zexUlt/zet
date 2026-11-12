// The two per-row primitives the width verdicts use (design.md §9, round C4):
// SetKnownFrom and ClearWidthClass(row, cls).
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
        auto r = Grid::Create(10, 3, 0);
        REQUIRE(r.has_value());
        G.reset(r.value());
    }
    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }
};

constexpr std::uint8_t A = WidthClassBit(EWidthClass::Ambiguous);
constexpr std::uint8_t P = WidthClassBit(EWidthClass::PrivateUse);

}  // namespace

TEST_CASE("SetKnownFrom sets the column outright, up and down") {
    Rig r;
    r.Feed("abc");
    CHECK(r.G->KnownFrom(0) == 0);
    r.G->SetKnownFrom(0, 7);
    CHECK(r.G->KnownFrom(0) == 7);
    r.G->SetKnownFrom(0, 2);
    CHECK(r.G->KnownFrom(0) == 2);
    CHECK(r.G->KnownFrom(1) == 0);
}

TEST_CASE("SetKnownFrom clamps to the width and ignores rows out of range") {
    Rig r;
    r.G->SetKnownFrom(1, 500);
    CHECK(r.G->KnownFrom(1) == 10);
    r.G->SetKnownFrom(3, 4);
    r.G->SetKnownFrom(65535, 4);
    CHECK(r.G->KnownFrom(0) == 0);
    CHECK(r.G->KnownFrom(2) == 0);
}

TEST_CASE("ClearWidthClass on a row clears that row's class only") {
    Rig r;
    // Row 0: A and P; row 1: A.
    r.Feed("\xce\xb1\xee\x82\xb0\r\n\xce\xb1");
    REQUIRE(r.G->WidthDoubtClasses(0) == (A | P));
    REQUIRE(r.G->WidthDoubtClasses(1) == A);
    r.G->ClearWidthClass(0, EWidthClass::Ambiguous);
    CHECK(r.G->WidthDoubtClasses(0) == P);
    CHECK(r.G->WidthDoubtClasses(1) == A);
    CHECK(r.G->WidthDoubtBefore(0, 10));
    r.G->ClearWidthClass(0, EWidthClass::PrivateUse);
    CHECK(r.G->WidthDoubtClasses(0) == 0);
    CHECK_FALSE(r.G->WidthDoubtBefore(0, 10));
    // Contents and KnownFrom stay.
    CHECK(r.G->CellAt(0, 0).Codepoint == U'α');
    CHECK(r.G->KnownFrom(0) == 0);
    // Out of range rows are ignored.
    r.G->ClearWidthClass(9, EWidthClass::Ambiguous);
    CHECK(r.G->WidthDoubtClasses(1) == A);
}
