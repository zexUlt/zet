// 400x300 truecolor (design.md §16 `test_m6_400x300`): every cell of the
// largest everyday terminal carries its own foreground and background
// colour.  The colours live inline in the cell (no side table to run out), the
// snapshot builds, the degrade ladder fits it under SNAPSHOT_MAX_BYTES at
// every step, and a terminal that is handed the bytes shows the grid.
//
// Mutants these tests were checked against (applied by hand):
//   TrueColorTo256: cube channels swapped            -> "256 colours"
//   Snapshot ladder: the 256-colour step skipped     -> "256 colours", "no
//   colour" Snapshot ladder: the no-colour step keeps colour -> "no colour",
//   "crop" Snapshot ladder: rows are not cropped from the top
//                                                    -> "crop"
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

constexpr int COLS = 400;
constexpr int ROWS = 300;
constexpr int CELLS = COLS * ROWS;

/// Every colour channel is three digits, so a full-fidelity cell costs the
/// most: the full encoding is over SNAPSHOT_MAX_BYTES and the ladder runs.
constexpr int LEVELS = 156;  // channel values 100..255

struct Rgb {
    std::uint8_t R, G, B;
};

Rgb FromIndex(int i) {
    return Rgb{static_cast<std::uint8_t>(100 + i % LEVELS),
               static_cast<std::uint8_t>(100 + (i / LEVELS) % LEVELS),
               static_cast<std::uint8_t>(100 + i / (LEVELS * LEVELS))};
}

/// Distinct for distinct cells: multiplying by a number coprime to LEVELS^3 is
/// a bijection of 0..LEVELS^3-1.
Rgb Fg(int cell) { return FromIndex(cell); }
Rgb Bg(int cell) {
    return FromIndex((cell * 7919) % (LEVELS * LEVELS * LEVELS));
}

/// Row marker "Rnnn" in the first four columns, a letter pattern after it.
char Glyph(int row, int col) {
    if (col < 4) {
        const std::string marker = "R" + std::to_string(1000 + row).substr(1);
        return marker[static_cast<std::size_t>(col)];
    }
    return static_cast<char>('A' + (row + col) % 26);
}

std::string RowMarker(int row) {
    return "R" + std::to_string(1000 + row).substr(1);
}

std::string Sgr(const Rgb& fg, const Rgb& bg) {
    return "\x1b[38;2;" + std::to_string(fg.R) + ";" + std::to_string(fg.G) +
           ";" + std::to_string(fg.B) + ";48;2;" + std::to_string(bg.R) + ";" +
           std::to_string(bg.G) + ";" + std::to_string(bg.B) + "m";
}

std::unique_ptr<Grid> MakeGrid(std::uint16_t cols, std::uint16_t rows) {
    auto g = Grid::Create(cols, rows);
    REQUIRE(g.has_value());
    return std::unique_ptr<Grid>{*g};
}

void Feed(Grid& g, const std::string& s) {
    VtParser parser;
    parser.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), g);
}

/// The 400x300 grid, written the way a program would: a CUP per row, then an
/// SGR and a glyph per cell.
std::unique_ptr<Grid> Filled() {
    auto g = MakeGrid(COLS, ROWS);
    for (int row = 0; row < ROWS; ++row) {
        std::string line = "\x1b[" + std::to_string(row + 1) + ";1H";
        for (int col = 0; col < COLS; ++col) {
            const int cell = row * COLS + col;
            line += Sgr(Fg(cell), Bg(cell));
            line += Glyph(row, col);
        }
        Feed(*g, line);
    }
    return g;
}

std::unique_ptr<Snapshot> Take(const Grid& g, std::size_t cap) {
    auto r = Snapshot::Create(g, SnapshotOptions{.MaxBytes = cap});
    REQUIRE(r.has_value());
    return std::unique_ptr<Snapshot>{*r};
}

std::unique_ptr<Grid> Restore(const Snapshot& snap) {
    auto g = MakeGrid(COLS, ROWS);
    const auto bytes = snap.Bytes();
    VtParser parser;
    parser.Feed(bytes.data(), bytes.size(), *g);
    return g;
}

std::string_view View(const Snapshot& s) {
    const auto b = s.Bytes();
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

/// xterm palette entry 16..255 as RGB.
Rgb Palette(int idx) {
    static constexpr int STEPS[6] = {0, 95, 135, 175, 215, 255};
    if (idx >= 232) {
        const auto v = static_cast<std::uint8_t>(8 + 10 * (idx - 232));
        return Rgb{v, v, v};
    }
    const int n = idx - 16;
    return Rgb{static_cast<std::uint8_t>(STEPS[n / 36]),
               static_cast<std::uint8_t>(STEPS[(n / 6) % 6]),
               static_cast<std::uint8_t>(STEPS[n % 6])};
}

int Dist2(const Rgb& a, const Rgb& b) {
    const int dr = a.R - b.R;
    const int dg = a.G - b.G;
    const int db = a.B - b.B;
    return dr * dr + dg * dg + db * db;
}

}  // namespace

TEST_CASE("400x300: cells keep their own colours") {
    const auto g = Filled();
    REQUIRE(g->Cols() == COLS);
    REQUIRE(g->Rows() == ROWS);
    static_assert(sizeof(Cell) == 16);

    std::vector<std::uint64_t> pairs;
    pairs.reserve(CELLS);
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            const int cell = row * COLS + col;
            const Cell& c = g->CellAt(static_cast<std::uint16_t>(row),
                                      static_cast<std::uint16_t>(col));
            const Rgb fg = Fg(cell);
            const Rgb bg = Bg(cell);
            REQUIRE(c.Codepoint == static_cast<char32_t>(Glyph(row, col)));
            REQUIRE(c.FgKind == EColorKind::TrueColor);
            REQUIRE(c.BgKind == EColorKind::TrueColor);
            REQUIRE(c.Fg[0] == fg.R);
            REQUIRE(c.Fg[1] == fg.G);
            REQUIRE(c.Fg[2] == fg.B);
            REQUIRE(c.Bg[0] == bg.R);
            REQUIRE(c.Bg[1] == bg.G);
            REQUIRE(c.Bg[2] == bg.B);
            pairs.push_back((std::uint64_t{c.Fg[0]} << 40) |
                            (std::uint64_t{c.Fg[1]} << 32) |
                            (std::uint64_t{c.Fg[2]} << 24) |
                            (std::uint64_t{c.Bg[0]} << 16) |
                            (std::uint64_t{c.Bg[1]} << 8) | c.Bg[2]);
        }
    }
    std::sort(pairs.begin(), pairs.end());
    CHECK(std::adjacent_find(pairs.begin(), pairs.end()) == pairs.end());
}

TEST_CASE("400x300: ladder: 256 colours is the first step that fits") {
    const auto g = Filled();
    const auto snap = Take(*g, SNAPSHOT_MAX_BYTES);
    CHECK(snap->Bytes().size() <= SNAPSHOT_MAX_BYTES);
    CHECK(snap->Degradation() == EDegradation::Colors256);

    // The default cap is the same cap.
    auto dflt = Snapshot::Create(*g);
    REQUIRE(dflt.has_value());
    CHECK(std::vector<std::byte>{(*dflt)->Bytes().begin(),
                                 (*dflt)->Bytes().end()} ==
          std::vector<std::byte>{snap->Bytes().begin(), snap->Bytes().end()});
    delete *dflt;

    const auto shown = Restore(*snap);
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            const int cell = row * COLS + col;
            const Cell& c = shown->CellAt(static_cast<std::uint16_t>(row),
                                          static_cast<std::uint16_t>(col));
            INFO("row " << row << " col " << col);
            REQUIRE(c.Codepoint == static_cast<char32_t>(Glyph(row, col)));
            REQUIRE(c.FgKind == EColorKind::Indexed);
            REQUIRE(c.BgKind == EColorKind::Indexed);
            REQUIRE(c.Fg[0] >= 16);
            REQUIRE(c.Bg[0] >= 16);
            // The nearest palette entry is within half a cube step per
            // channel (the widest gap is 40).
            REQUIRE(Dist2(Palette(c.Fg[0]), Fg(cell)) <= 3 * 20 * 20);
            REQUIRE(Dist2(Palette(c.Bg[0]), Bg(cell)) <= 3 * 20 * 20);
        }
    }
    CHECK(shown->CursorRow() == ROWS - 1);
    CHECK(shown->CursorCol() == COLS - 1);
    CHECK(shown->PendingWrap() == g->PendingWrap());
}

TEST_CASE("400x300: ladder: no colour fits a 1 MiB cap") {
    const auto g = Filled();
    const std::size_t cap = 1024 * 1024;
    const auto snap = Take(*g, cap);
    CHECK(snap->Bytes().size() <= cap);
    CHECK(snap->Degradation() == EDegradation::NoColor);

    const auto shown = Restore(*snap);
    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ++col) {
            const Cell& c = shown->CellAt(static_cast<std::uint16_t>(row),
                                          static_cast<std::uint16_t>(col));
            INFO("row " << row << " col " << col);
            REQUIRE(c.Codepoint == static_cast<char32_t>(Glyph(row, col)));
            REQUIRE(c.FgKind == EColorKind::Default);
            REQUIRE(c.BgKind == EColorKind::Default);
        }
    }
    CHECK(shown->CursorRow() == ROWS - 1);
}

TEST_CASE("400x300: ladder: crop keeps the bottom rows, not the top") {
    const auto g = Filled();
    const std::size_t cap = 64 * 1024;  // less than the 120000 glyphs
    const auto snap = Take(*g, cap);
    CHECK(snap->Bytes().size() <= cap);
    CHECK(snap->Degradation() == EDegradation::RowsCropped);

    const std::string_view bytes = View(*snap);
    CHECK(bytes.find(RowMarker(ROWS - 1)) != std::string_view::npos);
    CHECK(bytes.find(RowMarker(0)) == std::string_view::npos);

    // A terminal handed these bytes shows the last row, not the first.
    const auto shown = Restore(*snap);
    bool sawLast = false;
    bool sawFirst = false;
    for (int row = 0; row < ROWS; ++row) {
        std::string head;
        for (int col = 0; col < 4; ++col) {
            head +=
                static_cast<char>(shown
                                      ->CellAt(static_cast<std::uint16_t>(row),
                                               static_cast<std::uint16_t>(col))
                                      .Codepoint);
        }
        sawLast = sawLast || head == RowMarker(ROWS - 1);
        sawFirst = sawFirst || head == RowMarker(0);
    }
    CHECK(sawLast);
    CHECK_FALSE(sawFirst);
}
