// Snapshot size-measurement tests (design.md §9/§14).
//
// Two test cases:
//
//   worst_case_fits
//     Builds a 220×50 grid with every screen cell having a unique truecolor
//     foreground+background and 1000 history lines of cycling truecolor text,
//     takes a snapshot with MaxBytes=0 (the maximum allowed cap), and prints
//     the measured sizes.  Fails when:
//       - Snapshot::Create returns SnapshotTooLarge  → the encoder's output
//         for the defined worst case exceeds SNAPSHOT_MAX_BYTES, violating the
//         design §9 budget promise.
//       - total > SNAPSHOT_MAX_BYTES (redundant guard, same condition).
//
//   corpus_sizes_within_budget
//     For every .bin file in ZET_GRID_CORPUS_DIR (set by CMake), renders
//     the capture at 80×24 with default scrollback, takes a snapshot, records
//     sizes, and asserts the snapshot fits within SNAPSHOT_MAX_BYTES.  Fails
//     when any corpus file produces a snapshot that exceeds the budget.
//     Skipped (not failed) when the corpus dir is empty or not defined.
//
// How to confirm each assertion catches a regression:
//   - Reduce SNAPSHOT_MAX_BYTES in limits.hpp: worst_case_fits fails.
//   - Replace the Overflow check in Snapshot::Create with a no-op (always
//     succeed): no meaningful regression visible here but the nm-gate catches
//     the removed abort.
//   - Make EncodeScreen emit each truecolor cell twice: total roughly doubles,
//     worst_case_fits fails.
//   - Feed a corpus file that is huge enough to overflow the budget: corpus
//     test fails.

#include <doctest/doctest.h>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef ZET_GRID_CORPUS_DIR
#define ZET_GRID_CORPUS_DIR ""
#endif

using namespace zet::grid;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static Grid* MakeSizeGrid(int cols, int rows, int scrollback) {
    auto r = Grid::Create(static_cast<std::uint16_t>(cols),
                          static_cast<std::uint16_t>(rows),
                          static_cast<std::uint32_t>(scrollback));
    REQUIRE(r.has_value());
    return r.value();
}

// Build a short decimal SGR number string into buf (no NUL).  Returns bytes
// written.  Buf must hold at least 3 bytes.
static int DecStr(char* buf, unsigned v) {
    if (v >= 100) {
        buf[0] = static_cast<char>('0' + v / 100);
        buf[1] = static_cast<char>('0' + (v / 10) % 10);
        buf[2] = static_cast<char>('0' + v % 10);
        return 3;
    }
    if (v >= 10) {
        buf[0] = static_cast<char>('0' + v / 10);
        buf[1] = static_cast<char>('0' + v % 10);
        return 2;
    }
    buf[0] = static_cast<char>('0' + v);
    return 1;
}

// Feed one line of truecolor text into the grid via VtParser.  Each call
// produces one line of `cols` copies of `ch` in the given truecolor, followed
// by \r\n to scroll.
static void FeedColorLine(VtParser& p, Grid& g, int cols, unsigned r,
                          unsigned gr, unsigned b, char ch) {
    // Build: ESC[38;2;R;G;Bm + cols×ch + ESC[0m + \r\n
    // Max sequence: "\x1b[38;2;255;255;255m" = 19 chars + cols chars +
    // "\x1b[0m\r\n"
    std::vector<char> seq;
    seq.reserve(static_cast<std::size_t>(cols) + 32);

    // SGR for foreground
    seq.push_back('\x1b');
    seq.push_back('[');
    seq.push_back('3');
    seq.push_back('8');
    seq.push_back(';');
    seq.push_back('2');
    seq.push_back(';');
    char tmp[3];
    int n = DecStr(tmp, r);
    for (int i = 0; i < n; i++) seq.push_back(tmp[i]);
    seq.push_back(';');
    n = DecStr(tmp, gr);
    for (int i = 0; i < n; i++) seq.push_back(tmp[i]);
    seq.push_back(';');
    n = DecStr(tmp, b);
    for (int i = 0; i < n; i++) seq.push_back(tmp[i]);
    seq.push_back('m');

    for (int i = 0; i < cols; ++i) seq.push_back(ch);

    // Reset SGR + newline
    seq.push_back('\x1b');
    seq.push_back('[');
    seq.push_back('0');
    seq.push_back('m');
    seq.push_back('\r');
    seq.push_back('\n');

    p.Feed(reinterpret_cast<const std::byte*>(seq.data()), seq.size(), g);
}

// ---------------------------------------------------------------------------
// Print and record a size measurement row.
// ---------------------------------------------------------------------------

struct SizeRow {
    const char* label;
    std::size_t scrollback_len;  // preamble + history bytes
    std::size_t screen_len;      // total - scrollback_len
    std::size_t total;
    int cols;
    int rows;
    std::size_t history_lines;
};

static void PrintHeader() {
    MESSAGE(
        "  label                          "
        "  scrollback    screen       total   B/screen-cell");
}

static void PrintRow(const SizeRow& s) {
    double bpc = (s.cols > 0 && s.rows > 0)
                     ? static_cast<double>(s.screen_len) /
                           (static_cast<double>(s.cols) * s.rows)
                     : 0.0;
    char line[200];
    std::snprintf(line, sizeof(line), "  %-32s  %10zu  %10zu  %10zu  %7.1f",
                  s.label, s.scrollback_len, s.screen_len, s.total, bpc);
    MESSAGE(line);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("sizes: worst-case truecolor 220x50 1000 history lines") {
    // Mutant: reduce SNAPSHOT_MAX_BYTES → this test fails.
    // Mutant: make EncodeScreen emit each cell twice → total roughly doubles,
    //   test fails on the CHECK at the end.
    constexpr int kCols = 220;
    constexpr int kRows = 50;
    constexpr int kHistory = 1000;

    Grid* g = MakeSizeGrid(kCols, kRows, kHistory);
    VtParser p;

    // --- Fill history: kHistory lines, each with a cycling truecolor.
    // One SGR per line, kCols identical chars → the encoder emits one SGR +
    // kCols chars per line.  Represents real-world coloured terminal output.
    for (int line = 0; line < kHistory; ++line) {
        unsigned cr = static_cast<unsigned>((line * 3) & 0xFF);
        unsigned cg = static_cast<unsigned>((line * 7) & 0xFF);
        unsigned cb = static_cast<unsigned>((line * 11) & 0xFF);
        FeedColorLine(p, *g, kCols, cr, cg, cb, 'H');
    }

    // --- Fill screen: each cell with a unique truecolor FG + BG.
    // This maximises per-cell SGR overhead (no two adjacent cells share
    // colour) and represents the absolute worst-case for the screen section.
    for (std::uint16_t row = 0; row < kRows; ++row) {
        for (std::uint16_t col = 0; col < kCols; ++col) {
            std::uint32_t idx = static_cast<std::uint32_t>(row) * kCols + col;
            Cell& c = g->CellAt(row, col);
            c.Codepoint = U'X';
            c.Width = 1;
            c.FgKind = EColorKind::TrueColor;
            // Use idx to derive unique RGB; wrap within 0-255.
            c.Fg[0] = static_cast<std::uint8_t>((idx * 3 + 17) & 0xFF);
            c.Fg[1] = static_cast<std::uint8_t>((idx * 7 + 31) & 0xFF);
            c.Fg[2] = static_cast<std::uint8_t>((idx * 13 + 47) & 0xFF);
            c.BgKind = EColorKind::TrueColor;
            c.Bg[0] = static_cast<std::uint8_t>((~idx * 5 + 11) & 0xFF);
            c.Bg[1] = static_cast<std::uint8_t>((~idx * 11 + 23) & 0xFF);
            c.Bg[2] = static_cast<std::uint8_t>((~idx * 19 + 37) & 0xFF);
        }
    }

    // Take snapshot with the default cap (MaxBytes=0 → SNAPSHOT_MAX_BYTES).
    auto snap_res = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = 0});

    // If Create fails the worst case does not fit within SNAPSHOT_MAX_BYTES —
    // the design §9 budget is violated.
    if (!snap_res.has_value()) {
        FAIL("worst-case snapshot exceeds SNAPSHOT_MAX_BYTES ("
             << SNAPSHOT_MAX_BYTES
             << " bytes): design §9 budget violated — "
                "increase SNAPSHOT_MAX_BYTES or reduce the worst case");
    }

    Snapshot* snap = snap_res.value();
    const std::size_t total = snap->Bytes().size();
    const std::size_t scrollback_len = snap->ScrollbackLen();
    const std::size_t screen_len = total - scrollback_len;
    const double bpc_screen = static_cast<double>(screen_len) / (kCols * kRows);
    const double bpc_history =
        (kHistory > 0)
            ? static_cast<double>(scrollback_len) / (kHistory * kCols)
            : 0.0;

    MESSAGE("=== Snapshot size table — worst case ===");
    MESSAGE("  grid:         " << kCols << "×" << kRows << " truecolor, "
                               << kHistory << " history lines");
    MESSAGE("  scrollback section (preamble+history): " << scrollback_len
                                                        << " bytes");
    MESSAGE("  screen section:                        " << screen_len
                                                        << " bytes");
    MESSAGE("  total:                                 " << total << " bytes");
    MESSAGE("  SNAPSHOT_MAX_BYTES:                    " << SNAPSHOT_MAX_BYTES
                                                        << " bytes");
    MESSAGE("  bytes per screen cell (approx):        " << bpc_screen);
    MESSAGE("  bytes per history cell (approx):       " << bpc_history);
    MESSAGE("  budget used:  " << (100.0 * static_cast<double>(total) /
                                   static_cast<double>(SNAPSHOT_MAX_BYTES))
                               << "%");

    // Structural assertion: total must fit the design promise.
    CHECK(total <= SNAPSHOT_MAX_BYTES);

    delete snap;
    delete g;
}

TEST_CASE("sizes: corpus files all within budget") {
    const char* corpus_dir = ZET_GRID_CORPUS_DIR;
    if (!corpus_dir || corpus_dir[0] == '\0') {
        // Corpus directory not configured — skip measurement but do not fail.
        return;
    }

    // Collect .bin files from corpus_dir.  Use plain C stdio: no filesystem
    // library dependency.
#if defined(_WIN32)
    // Windows not in scope for v1 (design.md §12).
    return;
#else
    // Use popen(ls) rather than dirent to avoid a system-header dependency
    // mismatch between doctest and the rest.  This is a test helper.
    std::string cmd = std::string("ls '") + corpus_dir + "'/*.bin 2>/dev/null";
    FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return;

    std::vector<std::string> files;
    {
        char linebuf[4096];
        while (std::fgets(linebuf, sizeof(linebuf), fp)) {
            std::size_t len = std::strlen(linebuf);
            while (len > 0 &&
                   (linebuf[len - 1] == '\n' || linebuf[len - 1] == '\r'))
                --len;
            if (len > 0) files.emplace_back(linebuf, len);
        }
    }
    ::pclose(fp);
#endif

    if (files.empty()) return;

    MESSAGE(
        "=== Snapshot size table — corpus files (80×24 default "
        "scrollback) ===");
    PrintHeader();

    bool any_over_budget = false;
    for (const auto& path : files) {
        // Derive label from filename.
        std::string label = path;
        auto slash = label.rfind('/');
        if (slash != std::string::npos) label = label.substr(slash + 1);
        // Strip .bin
        if (label.size() > 4 && label.substr(label.size() - 4) == ".bin")
            label.resize(label.size() - 4);

        // Read file.
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            MESSAGE("  [skip] cannot open: " << path);
            continue;
        }
        std::vector<std::uint8_t> raw;
        {
            std::uint8_t tmp[4096];
            std::size_t n;
            while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0)
                raw.insert(raw.end(), tmp, tmp + n);
        }
        std::fclose(f);

        // Render through 80×24 grid.
        constexpr int kCols = 80;
        constexpr int kRows = 24;
        Grid* g = nullptr;
        {
            auto gr = Grid::Create(kCols, kRows);
            REQUIRE(gr.has_value());
            g = gr.value();
        }
        VtParser p;
        p.Feed(reinterpret_cast<const std::byte*>(raw.data()), raw.size(), *g);

        auto snap_res = Snapshot::Create(*g, SnapshotOptions{.MaxBytes = 0});
        if (!snap_res.has_value()) {
            MESSAGE("  [FAIL] " << label << ": Snapshot::Create returned "
                                << "SnapshotTooLarge");
            any_over_budget = true;
            delete g;
            continue;
        }
        Snapshot* snap = snap_res.value();
        SizeRow row{label.c_str(),
                    snap->ScrollbackLen(),
                    snap->Bytes().size() - snap->ScrollbackLen(),
                    snap->Bytes().size(),
                    kCols,
                    kRows,
                    0};
        PrintRow(row);

        if (snap->Bytes().size() > SNAPSHOT_MAX_BYTES) {
            any_over_budget = true;
        }

        delete snap;
        delete g;
    }

    CHECK(!any_over_budget);
}
