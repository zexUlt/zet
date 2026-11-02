// Degrade-ladder tests for the snapshot serializer (design.md §9).
//
// Every test states the mutant it exists to kill.  Caps are never guessed:
// they are derived from the encoder itself (smallest/largest cap that selects
// a step) or from the full encoding of a smaller grid.
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
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

Grid* Mk(int cols, int rows, int sb) {
    auto g = Grid::Create(static_cast<std::uint16_t>(cols),
                          static_cast<std::uint16_t>(rows),
                          static_cast<std::uint32_t>(sb));
    REQUIRE(g.has_value());
    return g.value();
}

void Feed(Grid& g, std::string_view s) {
    VtParser p;
    p.Feed(reinterpret_cast<const std::byte*>(s.data()), s.size(), g);
}

Grid* RestoreInto(const Snapshot& snap, int cols, int rows, int sb) {
    Grid* g = Mk(cols, rows, sb);
    VtParser p;
    const auto b = snap.Bytes();
    p.Feed(b.data(), b.size(), *g);
    return g;
}

std::string_view View(const Snapshot& s) {
    const auto b = s.Bytes();
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

std::size_t FullSize(const Grid& g) {
    auto r = Snapshot::Create(g);
    REQUIRE(r.has_value());
    REQUIRE(r.value()->Degradation() == EDegradation::None);
    const std::size_t n = r.value()->Bytes().size();
    delete r.value();
    return n;
}

/// Largest cap at which Create selects `want`; fails the test if none does.
std::size_t LargestCapWith(const Grid& g, EDegradation want) {
    for (std::size_t cap = FullSize(g); cap > 0; --cap) {
        auto r = Snapshot::Create(g, SnapshotOptions{.MaxBytes = cap});
        if (!r.has_value()) break;
        const bool hit = r.value()->Degradation() == want;
        delete r.value();
        if (hit) return cap;
    }
    FAIL("no cap selects the requested degradation step");
    return 0;
}

struct Rgb {
    std::uint8_t R, G, B, Idx;
};

std::string Sgr(const char* prefix, int r, int g, int b) {
    return std::string("\x1b[") + prefix + ";2;" + std::to_string(r) + ";" +
           std::to_string(g) + ";" + std::to_string(b) + "m";
}

}  // namespace

// ---------------------------------------------------------------------------
// TrueColor -> xterm-256 mapping: exact table, asserted unconditionally.
// Kills: cube tie rounding, grey-vs-cube tie, grey rounding, cube step value,
// grey clamp, grey ramp base, cube channel order.
// ---------------------------------------------------------------------------

TEST_CASE("degrade 256: exact palette index for boundary colours") {
    static const Rgb TABLE[] = {
        {0, 0, 0, 16},         // pure cube black
        {255, 255, 255, 231},  // pure cube white
        {95, 95, 95, 59},      // cube step 1
        {135, 135, 135, 102},  // cube step 2
        {255, 0, 0, 196},
        {0, 255, 0, 46},
        {0, 0, 255, 21},
        {115, 0, 0, 52},    // channel tie 95/135 resolves to the lower step
        {0, 155, 0, 28},    // channel tie 135/175 resolves to the lower step
        {0, 0, 195, 19},    // channel tie 175/215 resolves to the lower step
        {4, 4, 4, 16},      // grey and cube equally far: cube wins
        {0, 0, 13, 232},    // grey just closer than the cube
        {12, 12, 12, 232},  // grey ramp first step
        {13, 13, 13, 233},  // grey rounding boundary
        {14, 14, 14, 233},
        {47, 0, 0, 233},
        {48, 0, 0, 233},
        {128, 128, 128, 244},
        {138, 138, 138, 245},
        {128, 128, 155, 245},  // channel 155 is a tie, must not leave the grey
        {235, 235, 235, 255},  // last grey step
        {245, 245, 245, 255},  // beyond the ramp: clamped to the last step
    };
    constexpr int count = static_cast<int>(sizeof(TABLE) / sizeof(TABLE[0]));

    Grid* g = Mk(count, 1, 0);
    std::string in;
    for (const Rgb& c : TABLE)
        in += Sgr("38", c.R, c.G, c.B) + Sgr("48", c.R, c.G, c.B) + "x";
    Feed(*g, in);

    const std::size_t cap = LargestCapWith(*g, EDegradation::Colors256);
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = cap});
    REQUIRE(r.has_value());
    REQUIRE(r.value()->Degradation() == EDegradation::Colors256);

    Grid* dst = RestoreInto(*r.value(), count, 1, 0);
    for (int i = 0; i < count; ++i) {
        INFO("rgb(" << int(TABLE[i].R) << "," << int(TABLE[i].G) << ","
                    << int(TABLE[i].B) << ")");
        const Cell& c = dst->MainCellAt(0, static_cast<std::uint16_t>(i));
        CHECK(c.FgKind == EColorKind::Indexed);
        CHECK(c.Fg[0] == TABLE[i].Idx);
        CHECK(c.BgKind == EColorKind::Indexed);
        CHECK(c.Bg[0] == TABLE[i].Idx);
    }
    delete dst;
    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// Saved cursor and live pen colours follow the step.
// Kills: saved cursor / alt-screen saved cursor / live pen / bg not downgraded
// in 256-colour mode; the same fields kept in colour in no-colour mode.
// ---------------------------------------------------------------------------

namespace {

// Main screen: DECSC with truecolor fg+bg, then a different live pen.
const char* const SAVED_MAIN =
    "\x1b[1;38;2;255;0;0;48;2;0;0;255m\x1b[2;3Hmain\x1b"
    "7"
    "\x1b[38;2;0;255;0;48;2;255;255;0mlive";

// Alt screen: main-screen DECSC (taken by ?1049h) and an alt-screen DECSC.
const char* const SAVED_ALT =
    "\x1b[3;1H\x1b[4;38;2;255;0;0;48;2;0;0;255m\x1b[2;3Hmain\x1b[?1049h"
    "\x1b[38;2;0;255;0;48;2;255;255;0m\x1b[3;3Halt\x1b"
    "7"
    "\x1b[38;2;255;255;0;48;2;255;0;0m\x1b[1;1Hpen";

}  // namespace

TEST_CASE("degrade 256: saved cursor and live pen are downgraded") {
    Grid* g = Mk(12, 6, 0);
    Feed(*g, SAVED_MAIN);
    const std::size_t cap = LargestCapWith(*g, EDegradation::Colors256);
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = cap});
    REQUIRE(r.has_value());
    REQUIRE(r.value()->Degradation() == EDegradation::Colors256);
    Grid* d = RestoreInto(*r.value(), 12, 6, 0);

    const SavedCursor& sc = d->SavedCursorMainState();
    CHECK(sc.FgKind == EColorKind::Indexed);
    CHECK(sc.Fg[0] == 196);  // (255,0,0)
    CHECK(sc.BgKind == EColorKind::Indexed);
    CHECK(sc.Bg[0] == 21);  // (0,0,255)
    CHECK((sc.Attrs & Bold) != 0);

    CHECK(d->PenFgKind() == EColorKind::Indexed);
    CHECK(d->PenFg()[0] == 46);  // (0,255,0)
    CHECK(d->PenBgKind() == EColorKind::Indexed);
    CHECK(d->PenBg()[0] == 226);  // (255,255,0)

    delete d;
    delete r.value();
    delete g;
}

TEST_CASE("degrade 256: alt-screen saved cursors are downgraded") {
    Grid* g = Mk(12, 6, 0);
    Feed(*g, SAVED_ALT);
    REQUIRE(g->IsAltScreen());
    const std::size_t cap = LargestCapWith(*g, EDegradation::Colors256);
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = cap});
    REQUIRE(r.has_value());
    REQUIRE(r.value()->Degradation() == EDegradation::Colors256);
    Grid* d = RestoreInto(*r.value(), 12, 6, 0);
    REQUIRE(d->IsAltScreen());

    const SavedCursor& mainSc = d->SavedCursorMainState();
    CHECK(mainSc.FgKind == EColorKind::Indexed);
    CHECK(mainSc.Fg[0] == 196);
    CHECK(mainSc.BgKind == EColorKind::Indexed);
    CHECK(mainSc.Bg[0] == 21);

    const SavedCursor& altSc = d->SavedCursorAltState();
    CHECK(altSc.FgKind == EColorKind::Indexed);
    CHECK(altSc.Fg[0] == 46);
    CHECK(altSc.BgKind == EColorKind::Indexed);
    CHECK(altSc.Bg[0] == 226);

    delete d;
    delete r.value();
    delete g;
}

namespace {

void CheckNoColour(const Grid& d) {
    for (std::uint16_t r = 0; r < d.Rows(); ++r)
        for (std::uint16_t c = 0; c < d.Cols(); ++c) {
            CHECK(d.MainCellAt(r, c).FgKind == EColorKind::Default);
            CHECK(d.MainCellAt(r, c).BgKind == EColorKind::Default);
        }
    for (const SavedCursor* sc :
         {&d.SavedCursorMainState(), &d.SavedCursorAltState()}) {
        CHECK(sc->FgKind == EColorKind::Default);
        CHECK(sc->BgKind == EColorKind::Default);
    }
    CHECK(d.PenFgKind() == EColorKind::Default);
    CHECK(d.PenBgKind() == EColorKind::Default);
}

}  // namespace

TEST_CASE(
    "degrade no-colour: cell, saved cursor and live pen colour stripped") {
    Grid* g = Mk(12, 6, 0);
    Feed(*g, SAVED_MAIN);
    const std::size_t cap = LargestCapWith(*g, EDegradation::NoColor);
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = cap});
    REQUIRE(r.has_value());
    REQUIRE(r.value()->Degradation() == EDegradation::NoColor);
    Grid* d = RestoreInto(*r.value(), 12, 6, 0);

    CheckNoColour(*d);
    // Only colour goes: attributes and text survive.
    CHECK((d->SavedCursorMainState().Attrs & Bold) != 0);
    CHECK(d->MainCellAt(1, 2).Codepoint == U'm');
    CHECK((d->MainCellAt(1, 2).Attrs & Bold) != 0);

    delete d;
    delete r.value();
    delete g;
}

TEST_CASE("degrade no-colour: alt-screen saved cursors lose colour") {
    Grid* g = Mk(12, 6, 0);
    Feed(*g, SAVED_ALT);
    REQUIRE(g->IsAltScreen());
    const std::size_t cap = LargestCapWith(*g, EDegradation::NoColor);
    auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = cap});
    REQUIRE(r.has_value());
    REQUIRE(r.value()->Degradation() == EDegradation::NoColor);
    Grid* d = RestoreInto(*r.value(), 12, 6, 0);
    REQUIRE(d->IsAltScreen());

    CheckNoColour(*d);
    CHECK((d->SavedCursorMainState().Attrs & Underline) != 0);

    delete d;
    delete r.value();
    delete g;
}

// ---------------------------------------------------------------------------
// History trimming keeps exactly the newest lines that fit.
// The expectation is the plain full encoding of a grid that holds only those
// lines, so no ladder code is involved in the oracle.
// Kills: trim keeps one line fewer (or more).
// ---------------------------------------------------------------------------

namespace {

constexpr int HIST_ROWS = 4;
constexpr int HIST_TOTAL = 40;

/// Grid whose scrollback holds the newest `m` of the HIST_TOTAL numbered
/// lines; the screen always shows the last HIST_ROWS lines.
Grid* HistoryGrid(int m) {
    Grid* g = Mk(10, HIST_ROWS, 100);
    std::string in;
    for (int i = HIST_TOTAL - HIST_ROWS - m; i < HIST_TOTAL; ++i) {
        char line[16];
        std::snprintf(line, sizeof line, "h%03d", i);
        in += line;
        if (i + 1 < HIST_TOTAL) in += "\r\n";
    }
    Feed(*g, in);
    REQUIRE(g->ScrollbackSize() == static_cast<std::uint32_t>(m));
    return g;
}

void ExpectSameSnapshot(const Snapshot& a, const Snapshot& b) {
    CHECK(a.Bytes().size() == b.Bytes().size());
    CHECK(View(a) == View(b));
    CHECK(a.ScrollbackLen() == b.ScrollbackLen());
}

}  // namespace

TEST_CASE("degrade history: cap fitting m lines keeps exactly m newest") {
    Grid* big = HistoryGrid(HIST_TOTAL - HIST_ROWS);
    const int counts[] = {0, 1, 2, 7, 20, HIST_TOTAL - HIST_ROWS - 1};
    for (int m : counts) {
        INFO("m=" << m);
        Grid* small = HistoryGrid(m);
        const std::size_t cap = FullSize(*small);

        auto at = Snapshot::Create(*big, SnapshotOptions{.MaxBytes = cap});
        REQUIRE(at.has_value());
        CHECK(at.value()->Degradation() == EDegradation::HistoryTrimmed);
        auto want = Snapshot::Create(*small);
        REQUIRE(want.has_value());
        ExpectSameSnapshot(*at.value(), *want.value());

        if (m > 0) {
            // One byte less no longer fits m lines: m-1 lines are kept.
            Grid* fewer = HistoryGrid(m - 1);
            auto below =
                Snapshot::Create(*big, SnapshotOptions{.MaxBytes = cap - 1});
            REQUIRE(below.has_value());
            auto wantBelow = Snapshot::Create(*fewer);
            REQUIRE(wantBelow.has_value());
            ExpectSameSnapshot(*below.value(), *wantBelow.value());
            delete below.value();
            delete wantBelow.value();
            delete fewer;
        }
        delete at.value();
        delete want.value();
        delete small;
    }
    delete big;
}

// ---------------------------------------------------------------------------
// Row cropping: tight for every cap, never drops the cursor row.
// Kills: crop off by one in either direction (top or bottom), cropping the
// cursor row.
// ---------------------------------------------------------------------------

namespace {

constexpr int CROP_ROWS = 12;

std::string Marker(int row) {
    char b[16];
    std::snprintf(b, sizeof b, "row%02d", row);
    return b;
}

bool Has(std::string_view hay, std::string_view needle) {
    return hay.find(needle) != std::string_view::npos;
}

/// 12 rows of plain text, cursor parked on `cursorRow` (0-based).
Grid* CropGrid(int cursorRow) {
    Grid* g = Mk(30, CROP_ROWS, 0);
    std::string in;
    for (int r = 0; r < CROP_ROWS; ++r) {
        in += "\x1b[" + std::to_string(r + 1) + ";1H" + Marker(r) +
              "-padding-padding";
    }
    in += "\x1b[" + std::to_string(cursorRow + 1) + ";3H";
    Feed(*g, in);
    REQUIRE(g->CursorRow() == cursorRow);
    return g;
}

int KeptRows(std::string_view bytes) {
    int n = 0;
    for (int r = 0; r < CROP_ROWS; ++r) n += Has(bytes, Marker(r)) ? 1 : 0;
    return n;
}

}  // namespace

TEST_CASE(
    "degrade crop: every cap yields the tightest snapshot, cursor row kept") {
    for (int cursorRow : {0, 5, CROP_ROWS - 1}) {
        INFO("cursorRow=" << cursorRow);
        Grid* g = CropGrid(cursorRow);
        const std::size_t full = FullSize(*g);

        bool everFit = false;
        int prevKept = 0;
        std::size_t firstFit = 0;
        for (std::size_t cap = 1; cap <= full; ++cap) {
            INFO("cap=" << cap);
            auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = cap});
            if (!r.has_value()) {
                CHECK_FALSE(everFit);  // once something fits, larger caps fit
                CHECK(r.error() == EGridError::SnapshotTooLarge);
                continue;
            }
            if (!everFit) firstFit = cap;
            everFit = true;
            const Snapshot& s = *r.value();
            CHECK(s.Bytes().size() <= cap);
            CHECK(Has(View(s), Marker(cursorRow)));  // cursor row always kept

            // Tight: the snapshot's own size is a fixed point, and one byte
            // less forces strictly more cropping.
            auto again = Snapshot::Create(
                *g, SnapshotOptions{.MaxBytes = s.Bytes().size()});
            REQUIRE(again.has_value());
            CHECK(View(*again.value()) == View(s));
            delete again.value();
            auto less = Snapshot::Create(
                *g, SnapshotOptions{.MaxBytes = s.Bytes().size() - 1});
            if (less.has_value()) {
                CHECK(less.value()->Bytes().size() < s.Bytes().size());
                CHECK(KeptRows(View(*less.value())) < KeptRows(View(s)));
                delete less.value();
            }

            const int kept = KeptRows(View(s));
            CHECK(kept >= prevKept);
            prevKept = kept;
            if (s.Degradation() == EDegradation::None) {
                CHECK(kept == CROP_ROWS);
            }
            delete r.value();
        }
        REQUIRE(everFit);

        // The smallest cap that fits keeps the cursor row and nothing else.
        auto minimal =
            Snapshot::Create(*g, SnapshotOptions{.MaxBytes = firstFit});
        REQUIRE(minimal.has_value());
        CHECK(minimal.value()->Degradation() == EDegradation::RowsCropped);
        CHECK(KeptRows(View(*minimal.value())) == 1);
        delete minimal.value();
        delete g;
    }
}

TEST_CASE("degrade crop: rows above the cursor go first, top first") {
    Grid* g = CropGrid(8);
    // Cap between "cursor row + rows below" and "everything": only top rows
    // are cropped, the rows below the cursor survive.
    const std::size_t full = FullSize(*g);
    bool sawTopOnly = false;
    for (std::size_t cap = full; cap > 0; --cap) {
        auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = cap});
        if (!r.has_value()) break;
        const std::string_view v = View(*r.value());
        int firstKept = -1;
        int lastKept = -1;
        for (int row = 0; row < CROP_ROWS; ++row)
            if (Has(v, Marker(row))) {
                if (firstKept < 0) firstKept = row;
                lastKept = row;
            }
        // Kept rows are a contiguous block containing the cursor row.
        for (int row = firstKept; row <= lastKept; ++row)
            CHECK(Has(v, Marker(row)));
        CHECK(firstKept <= 8);
        CHECK(lastKept >= 8);
        if (firstKept > 0 && lastKept == CROP_ROWS - 1) sawTopOnly = true;
        // Bottom rows are only dropped once every row above is gone.
        if (lastKept < CROP_ROWS - 1) CHECK(firstKept == 8);
        delete r.value();
    }
    CHECK(sawTopOnly);
    delete g;
}

// ---------------------------------------------------------------------------
// MaxBytes above SNAPSHOT_MAX_BYTES is clamped.
// Kills: removing the clamp.
// ---------------------------------------------------------------------------

TEST_CASE("degrade cap: MaxBytes is clamped to SNAPSHOT_MAX_BYTES") {
    // 1400 history lines of 200 differently-coloured cells: ~4.5 MiB encoded.
    Grid* g = Mk(200, 4, 1500);
    std::string line;
    for (int c = 0; c < 200; ++c)
        line += (c % 2 ? "\x1b[38;2;1;2;3mx" : "\x1b[38;2;4;5;6my");
    std::string in;
    for (int i = 0; i < 1400; ++i) in += line + "\r\n";
    Feed(*g, in);
    REQUIRE(g->ScrollbackSize() > 1300);

    for (std::size_t asked : {SNAPSHOT_MAX_BYTES + 1,
                              std::size_t{64} * 1024 * 1024, ~std::size_t{0}}) {
        INFO("MaxBytes=" << asked);
        auto r = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = asked});
        REQUIRE(r.has_value());
        CHECK(r.value()->Bytes().size() <= SNAPSHOT_MAX_BYTES);
        CHECK(r.value()->Degradation() == EDegradation::HistoryTrimmed);
        delete r.value();
    }
    delete g;
}
