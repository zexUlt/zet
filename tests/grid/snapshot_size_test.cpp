// Snapshot size measurements (design.md §9, §14).
//
// Every number here comes from Snapshot::Create on a real Grid, never from a
// constant typed in by hand.  What the tests assert is the contract, not an
// exact byte count: the exact counts move with harmless encoder tweaks (an
// SGR reordering, a cheaper cursor jump) and a pin would break on them
// without catching anything.  So:
//
//   - Total <= SNAPSHOT_MAX_BYTES for every case (the hard budget);
//   - the degrade level is pinned only where the input is far from a
//     threshold, so no harmless tweak can flip it;
//   - a loose floor and ceiling on bytes per screen cell guard the worst
//     case against an encoder that silently drops colour (floor) or bloats
//     (ceiling).
//
// Run with `zet_tests -s -tc="sizes*"` to print the table that design.md §9
// quotes.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

#ifndef ZET_GRID_CORPUS_DIR
#define ZET_GRID_CORPUS_DIR ""
#endif

using namespace zet::grid;

namespace {

struct Measured {
    std::size_t Scrollback{0};  // preamble + history
    std::size_t Screen{0};      // Total - Scrollback
    std::size_t Total{0};
    EDegradation Degradation{EDegradation::None};
};

const char* LevelName(EDegradation d) {
    switch (d) {
        case EDegradation::None:
            return "none";
        case EDegradation::HistoryTrimmed:
            return "history-trimmed";
        case EDegradation::Colors256:
            return "256-colour";
        case EDegradation::NoColor:
            return "no-colour";
        case EDegradation::RowsCropped:
            return "rows-cropped";
    }
    return "?";
}

Grid* MakeGrid(int cols, int rows, int scrollback) {
    auto r = Grid::Create(static_cast<std::uint16_t>(cols),
                          static_cast<std::uint16_t>(rows),
                          static_cast<std::uint32_t>(scrollback));
    REQUIRE(r.has_value());
    return r.value();
}

void Feed(Grid& g, const std::string& s) {
    VtParser p;
    p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), g);
}

Measured Measure(const Grid& g) {
    auto r = Snapshot::Create(g);
    REQUIRE(r.has_value());
    const Snapshot* s = r.value();
    Measured m;
    m.Total = s->Bytes().size();
    m.Scrollback = s->ScrollbackLen();
    m.Screen = m.Total - m.Scrollback;
    m.Degradation = s->Degradation();
    delete s;
    return m;
}

void PrintHeader() {
    MESSAGE(
        "case                                     scrollback      screen"
        "       total  B/cell  level");
}

void PrintRow(const char* label, const Measured& m, int cols, int rows) {
    const double perCell =
        static_cast<double>(m.Screen) / (static_cast<double>(cols) * rows);
    char line[200];
    std::snprintf(line, sizeof(line), "%-38s %12zu %11zu %11zu %7.1f  %s",
                  label, m.Scrollback, m.Screen, m.Total, perCell,
                  LevelName(m.Degradation));
    MESSAGE(line);
}

// One line of `cols` cells where every cell has its own truecolor FG and BG.
// Adjacent cells always differ (the FG red channel steps by 3, mod 256), so
// the encoder cannot reuse a pen between cells.
void AppendUniqueLine(std::string& out, int cols, std::uint32_t& counter) {
    for (int c = 0; c < cols; ++c, ++counter) {
        const std::uint32_t i = counter;
        out += "\x1b[38;2;" + std::to_string((i * 3 + 17) & 0xFF) + ";" +
               std::to_string((i * 7 + 31) & 0xFF) + ";" +
               std::to_string((i * 13 + 47) & 0xFF) + ";48;2;" +
               std::to_string((~i * 5 + 11) & 0xFF) + ";" +
               std::to_string((~i * 11 + 23) & 0xFF) + ";" +
               std::to_string((~i * 19 + 37) & 0xFF) + "mX";
    }
}

// One line of `cols` identical cells in a truecolor chosen by the line index:
// one SGR per line, which is what coloured program output usually looks like.
void AppendUniformLine(std::string& out, int cols, int line) {
    out += "\x1b[38;2;" + std::to_string((line * 3) & 0xFF) + ";" +
           std::to_string((line * 7) & 0xFF) + ";" +
           std::to_string((line * 11) & 0xFF) + "m";
    out.append(static_cast<std::size_t>(cols), 'H');
}

// `history` uniform lines that scroll off, then `rows` unique-cell screen rows.
Grid* BuildUniformHistoryUniqueScreen(int cols, int rows, int history) {
    Grid* g = MakeGrid(cols, rows, history);
    std::string s;
    for (int line = 0; line < history; ++line) {
        AppendUniformLine(s, cols, line);
        s += "\r\n";
    }
    std::uint32_t counter = 0;
    for (int row = 0; row < rows; ++row) {
        AppendUniqueLine(s, cols, counter);
        if (row + 1 < rows) s += "\r\n";
    }
    Feed(*g, s);
    return g;
}

// Every cell of every line, history and screen alike, has a unique truecolor
// FG and BG.
Grid* BuildAllUnique(int cols, int rows, int history) {
    Grid* g = MakeGrid(cols, rows, history);
    std::string s;
    std::uint32_t counter = 0;
    for (int line = 0; line < history + rows; ++line) {
        AppendUniqueLine(s, cols, counter);
        if (line + 1 < history + rows) s += "\r\n";
    }
    Feed(*g, s);
    return g;
}

std::vector<std::filesystem::path> CorpusFiles() {
    std::vector<std::filesystem::path> files;
    const std::filesystem::path dir{ZET_GRID_CORPUS_DIR};
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".bin") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

}  // namespace

TEST_CASE("sizes: every corpus capture fits without degradation") {
    // Mutant: a corpus capture that makes the encoder blow past the budget or
    // trip the ladder fails the two CHECKs.
    const auto files = CorpusFiles();
    // The directory is wired in by CMake; an empty one is a broken build, not
    // a reason to skip.
    REQUIRE_MESSAGE(!files.empty(),
                    "no .bin captures in " << ZET_GRID_CORPUS_DIR);

    constexpr int cols = 80;
    constexpr int rows = 24;
    PrintHeader();
    for (const auto& path : files) {
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "cannot open " << path);
        const std::string raw{std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>()};
        Grid* g =
            MakeGrid(cols, rows, static_cast<int>(DEFAULT_SCROLLBACK_LIMIT));
        Feed(*g, raw);
        const Measured m = Measure(*g);
        delete g;

        PrintRow(path.stem().string().c_str(), m, cols, rows);
        CHECK_MESSAGE(m.Total <= SNAPSHOT_MAX_BYTES, path.stem().string());
        CHECK_MESSAGE(m.Degradation == EDegradation::None,
                      path.stem().string());
    }
}

TEST_CASE("sizes: 220x50 truecolor screen with 1000 history lines fits") {
    // The worst case design.md §9 quotes: every screen cell has its own
    // truecolor FG+BG; history is 1000 full-width lines, one colour per line.
    // Mutant: an encoder that emits a cell twice, or SNAPSHOT_MAX_BYTES cut
    // below the measured total, fails the budget CHECK; one that drops colour
    // fails the floor.
    constexpr int cols = 220;
    constexpr int rows = 50;
    constexpr int history = 1000;
    Grid* g = BuildUniformHistoryUniqueScreen(cols, rows, history);
    const Measured m = Measure(*g);
    delete g;

    PrintHeader();
    PrintRow("220x50 unique screen, uniform history", m, cols, rows);

    CHECK(m.Total <= SNAPSHOT_MAX_BYTES);
    CHECK(m.Degradation == EDegradation::None);
    const double perCell = static_cast<double>(m.Screen) / (cols * rows);
    // Unique FG+BG needs at least one 38;2;r;g;b and one 48;2;r;g;b per cell.
    CHECK(perCell >= 20.0);
    CHECK(perCell <= 60.0);
}

TEST_CASE(
    "sizes: 220x50 truecolor everywhere with 1000 history lines degrades") {
    // The same grid with a unique pen on every history cell too: the full
    // encoding is several times the cap, so the ladder must engage and stop
    // at its first step (trimming history), which is enough here.
    // Mutant: a ladder that skips step 1 and drops colour lands on a later
    // level and fails the level CHECK; one that does not trim fails the budget.
    constexpr int cols = 220;
    constexpr int rows = 50;
    constexpr int history = 1000;
    Grid* g = BuildAllUnique(cols, rows, history);
    const Measured m = Measure(*g);
    delete g;

    PrintHeader();
    PrintRow("220x50 unique everywhere", m, cols, rows);

    CHECK(m.Total <= SNAPSHOT_MAX_BYTES);
    CHECK(m.Degradation == EDegradation::HistoryTrimmed);
}

TEST_CASE("sizes: full-width 4096 columns degrades past history trimming") {
    // 4096 columns is MAX_COLS; MAX_SCREEN_CELLS (1 << 20) allows 256 rows.
    // 50 unique-truecolor rows are ~200k cells, which alone is far over the
    // cap, so trimming history cannot be enough and colour must go.
    // Mutant: a ladder that stops after trimming history returns a snapshot
    // over the cap or fails Create; the level CHECK catches both.
    constexpr int cols = 4096;
    constexpr int rows = 50;
    constexpr int history = 20;
    Grid* g = BuildAllUnique(cols, rows, history);
    const Measured m = Measure(*g);
    delete g;

    PrintHeader();
    PrintRow("4096x50 unique everywhere", m, cols, rows);

    CHECK(m.Total <= SNAPSHOT_MAX_BYTES);
    CHECK(m.Degradation >= EDegradation::Colors256);
    // Dropping colour is enough for 50 rows; cropping rows would be a loss the
    // ladder must not take while a cheaper step still fits.
    CHECK(m.Degradation < EDegradation::RowsCropped);
}

TEST_CASE("sizes: 1024x64 unique truecolor lands exactly on 256 colours") {
    // The only size case that reaches the Colors256 step: unique truecolor
    // over 1024x64 cells is over the cap at full fidelity (the main screen is
    // encoded twice, see design.md §9), and the 256-colour encoding of the
    // same cells fits.  The 4096x50 case above accepts anything from
    // Colors256 up, so it cannot tell a working step from a no-op.
    // Mutant: a Colors256 step that is skipped or does not change the colour
    // encoding lands on NoColor (or RowsCropped) and fails the level CHECK.
    constexpr int cols = 1024;
    constexpr int rows = 64;
    Grid* g = BuildAllUnique(cols, rows, 0);
    const Measured m = Measure(*g);
    delete g;

    PrintHeader();
    PrintRow("1024x64 unique, no history", m, cols, rows);

    CHECK(m.Total <= SNAPSHOT_MAX_BYTES);
    CHECK(m.Degradation == EDegradation::Colors256);
    // Colour is still there: far more than the ~1 byte per cell of NoColor.
    CHECK(static_cast<double>(m.Screen) / (cols * rows) >= 5.0);
}
