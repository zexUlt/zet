// The Cyrillic bit of the path mark (design.md §9, round C, C4d). The grid
// notes that Cyrillic was printed since the column was last set absolutely, on
// either width policy, and no other non-ASCII glyph sets it. A matched
// calibration answer restarts the path, and DECSC/DECRC carry the bit.
#include <doctest/doctest.h>

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

    explicit Rig(bool narrowCyrillic) {
        auto r = Grid::Create(20, 4, 0);
        REQUIRE(r.has_value());
        G.reset(r.value());
        if (narrowCyrillic) {
            WidthPolicy policy = G->Policy();
            policy.DoubtClasses &= static_cast<std::uint8_t>(
                ~WidthClassBit(EWidthClass::Cyrillic));
            G->SetWidthPolicy(policy);
        }
    }
    void Feed(std::string_view s) {
        P.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), *G);
    }
};

const std::string CYR = "\xD0\xBF";  // U+043F

}  // namespace

TEST_CASE("Cyrillic marks the path under either policy") {
    for (const bool narrow : {false, true}) {
        CAPTURE(narrow);
        Rig r(narrow);
        CHECK_FALSE(r.G->PathHasCyrillic());
        r.Feed("ab");
        CHECK_FALSE(r.G->PathHasCyrillic());
        r.Feed(CYR);
        CHECK(r.G->PathHasCyrillic());
    }
}

TEST_CASE("Other non-ASCII glyphs leave the Cyrillic bit alone") {
    for (const char* glyph :
         {"\xC3\xA9", "\xCE\xB1", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80",
          "\xEE\x82\xA0", "\xE2\x94\x80", "\xCC\x81"}) {
        CAPTURE(glyph);
        for (const bool narrow : {false, true}) {
            Rig r(narrow);
            r.Feed(std::string("ab") + glyph);
            CHECK(r.G->PathHasNonAscii());
            CHECK_FALSE(r.G->PathHasCyrillic());
        }
    }
}

TEST_CASE("An absolute column set clears the Cyrillic bit") {
    for (const char* reset : {"\r", "\x1b[5G", "\x1b[2;3H"}) {
        CAPTURE(reset);
        Rig r(true);
        r.Feed(CYR);
        REQUIRE(r.G->PathHasCyrillic());
        r.Feed(reset);
        CHECK_FALSE(r.G->PathHasCyrillic());
        r.Feed(CYR);
        CHECK(r.G->PathHasCyrillic());
    }
}

TEST_CASE("A relative move keeps the Cyrillic bit") {
    Rig r(true);
    r.Feed(CYR + "\x1b[2D\x1b[C\x08\t");
    CHECK(r.G->PathHasCyrillic());
}

TEST_CASE("Resync and ConfirmPath clear the Cyrillic bit") {
    Rig r(true);
    r.Feed(CYR);
    REQUIRE(r.G->PathHasCyrillic());
    r.G->ConfirmPath();
    CHECK_FALSE(r.G->PathHasCyrillic());
    r.Feed(CYR);
    REQUIRE(r.G->PathHasCyrillic());
    r.G->Resync(1, 4, false);
    CHECK_FALSE(r.G->PathHasCyrillic());
}

TEST_CASE("DECSC and DECRC carry the Cyrillic bit") {
    Rig r(true);
    r.Feed(CYR +
           "\x1b"
           "7");
    r.Feed("\r");
    REQUIRE_FALSE(r.G->PathHasCyrillic());
    r.Feed(
        "\x1b"
        "8");
    CHECK(r.G->PathHasCyrillic());

    // A slot saved with a clean path restores a clean path.
    r.Feed(
        "\r\x1b"
        "7" +
        CYR);
    REQUIRE(r.G->PathHasCyrillic());
    r.Feed(
        "\x1b"
        "8");
    CHECK_FALSE(r.G->PathHasCyrillic());
}
