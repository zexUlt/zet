// Review additions for the left-move doubt (design.md §9, "Неоднозначная
// ширина"): the flag across rows, a saved cursor, the alt screen, a pending
// wrap, CBT and the 1-column wrap chain.
#include <doctest/doctest.h>

#include <string>

#include "overlay_rig.hpp"

using namespace zet;
using namespace zet::predict;
using namespace zet::predict::test;

namespace {

const char* const ZHE = "\xd0\xb6";  // U+0436, East Asian ambiguous

struct Mirror {
    Term T;
    Mirror(int cols, int rows) : T(cols, rows) {}
    void Feed(std::string_view s) { T.Feed(s); }
    [[nodiscard]] bool Before(std::uint16_t row, std::uint16_t col) const {
        return T.G->WidthDoubtBefore(row, col) || T.G->ColumnDoubt();
    }
};

}  // namespace

TEST_CASE("left-move doubt follows the cursor to another row") {
    for (const char* mv : {"\x1b[A", "\x1b[B", "\x1b[2d", "\x1b[e", "\n"}) {
        Mirror m(10, 5);
        m.Feed("\x1b[2;1H");
        m.Feed(std::string(ZHE) + "\x08");
        REQUIRE(m.T.G->ColumnDoubt());
        m.Feed(mv);
        CAPTURE(mv);
        CHECK(m.Before(m.T.G->CursorRow(), m.T.G->CursorCol()));
    }
}

TEST_CASE("CBT over a doubtful glyph leaves the column untrusted") {
    Mirror m(20, 3);
    m.Feed(std::string("ab") + ZHE + "\x1b[Z");
    REQUIRE(m.T.G->CursorCol() == 0);
    CHECK(m.Before(0, 0));
}

TEST_CASE("CUB that stays right of the mark leaves no stale flag after CR") {
    Mirror m(10, 3);
    m.Feed(std::string(ZHE) + "abc\x1b[1D");
    m.Feed("\r");
    CHECK_FALSE(m.Before(0, 0));
}

TEST_CASE("a saved cursor taken after a left move over doubt keeps the doubt") {
    Mirror m(10, 3);
    m.Feed(std::string(ZHE) +
           "\x08\x1b"
           "7");
    REQUIRE(m.T.G->ColumnDoubt());
    m.Feed("\x1b[3;5H");  // absolute: the flag goes
    REQUIRE_FALSE(m.T.G->ColumnDoubt());
    m.Feed(
        "\x1b"
        "8");  // the terminal is back at its own saved column
    CHECK(m.Before(m.T.G->CursorRow(), m.T.G->CursorCol()));
}

TEST_CASE("alt screen round trip keeps the doubt of the main screen") {
    Mirror m(10, 3);
    m.Feed(std::string(ZHE) + "\x08");
    REQUIRE(m.T.G->ColumnDoubt());
    m.Feed("\x1b[?1049h\x1b[1;1H\x1b[?1049l");
    CHECK(m.Before(m.T.G->CursorRow(), m.T.G->CursorCol()));
}

TEST_CASE("backspace from a pending wrap over a doubtful last glyph") {
    Mirror m(5, 3);
    m.Feed(std::string("abcd") +
           ZHE);  // glyph in the last column, pending wrap
    REQUIRE(m.T.G->PendingWrap());
    m.Feed("\x08");
    CHECK(m.Before(m.T.G->CursorRow(), m.T.G->CursorCol()));
}

TEST_CASE("CUB from a pending wrap over a doubtful last glyph") {
    Mirror m(5, 3);
    m.Feed(std::string("abcd") + ZHE + "\x1b[2D");
    CHECK(m.Before(m.T.G->CursorRow(), m.T.G->CursorCol()));
}

TEST_CASE("a wrap chain keeps the mark when the glyph is the last of a row") {
    Mirror m(3, 5);
    m.Feed(std::string("ab") + ZHE + "defghi");
    REQUIRE(m.T.G->CursorRow() >= 2);
    CHECK(m.Before(m.T.G->CursorRow(), 1));
}

TEST_CASE("a column in doubt always sits on a broken path (random walk)") {
    static const char* const PIECES[] = {"a",
                                         "\xd0\xb6",
                                         "\xe4\xb8\xad",
                                         "\x08",
                                         "\r",
                                         "\n",
                                         "\x1b[D",
                                         "\x1b[3D",
                                         "\x1b[C",
                                         "\x1b[Z",
                                         "\t",
                                         "\x1b[A",
                                         "\x1b[B",
                                         "\x1b[1G",
                                         "\x1b[2;3H",
                                         "\x1b"
                                         "7",
                                         "\x1b"
                                         "8",
                                         "\x1b[?1049h",
                                         "\x1b[?1049l",
                                         "\x1b[2K",
                                         "\x1b"
                                         "c",
                                         "\x1b[2J",
                                         "\x1b[?7l",
                                         "\x1b[?7h",
                                         "\x1b[4h"};
    constexpr std::size_t N = sizeof(PIECES) / sizeof(PIECES[0]);
    std::uint32_t x = 12345;
    for (int round = 0; round < 300; ++round) {
        Mirror m(6 + round % 5, 4);
        for (int i = 0; i < 80; ++i) {
            x = x * 1664525U + 1013904223U;
            m.Feed(PIECES[(x >> 16) % pieceCount]);
            if (m.T.G->ColumnDoubt()) CHECK(m.T.G->PathBroken());
        }
    }
}

TEST_CASE("CUF after a left move over doubt keeps the column untrusted") {
    Mirror m(10, 3);
    m.Feed(std::string("ab") + ZHE + "\x1b[3D\x1b[C");
    REQUIRE(m.T.G->CursorCol() == 1);
    CHECK(m.Before(0, 1));
}

TEST_CASE("a wrap of a wide glyph from the last column hands on the doubt") {
    Mirror m(5, 3);
    m.Feed(std::string("ab") + ZHE + "c\xe4\xb8\xad");  // U+4E2D needs 2 cells
    REQUIRE(m.T.G->CursorRow() == 1);
    CHECK(m.Before(1, 1));
}

// A shell erases what it echoed with BS, space, BS: the cells are blank again,
// so only the doubt keeps the engine from predicting.
TEST_CASE("engine: no insertion is predicted after a left move over doubt") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Output(std::string(ZHE) + "\x08 \x08");
    REQUIRE(r.G->ColumnDoubt());
    REQUIRE_FALSE(r.G->WidthDoubtBefore(r.G->CursorRow(), r.G->CursorCol()));
    r.Type("x");
    CHECK(r.Count() == 0);
}

TEST_CASE("engine: no backspace is predicted after a left move over doubt") {
    Rig r(20, 4, EDisplayMode::Always);
    r.Prompt();
    r.ConfirmEpoch();
    r.Output(std::string("b") + ZHE + "\x08 \x08\x08 \x08");
    REQUIRE(r.G->ColumnDoubt());
    REQUIRE(r.G->CursorCol() > 2);
    REQUIRE_FALSE(r.G->WidthDoubtBefore(r.G->CursorRow(), r.G->CursorCol()));
    r.Type("\x7f");
    CHECK(r.Count() == 0);
}

TEST_CASE("engine: the same keys are predicted without the doubtful glyph") {
    {
        Rig r(20, 4, EDisplayMode::Always);
        r.Prompt();
        r.ConfirmEpoch();
        r.Output("a\x08 \x08");
        REQUIRE_FALSE(r.G->ColumnDoubt());
        r.Type("x");
        CHECK(r.Count() == 1);
    }
    {
        Rig r(20, 4, EDisplayMode::Always);
        r.Prompt();
        r.ConfirmEpoch();
        r.Output("b\x08 \x08");
        REQUIRE_FALSE(r.G->ColumnDoubt());
        r.Type("\x7f");
        CHECK(r.Count() == 1);
    }
}
