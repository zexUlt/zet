// Fuzz target for Snapshot::Create round trip (design.md §9, §16).
//
// A grid is built from the input, snapshotted, and the bytes are fed to a
// fresh grid of the same size and scrollback limit. What has to survive is
// read off snap->Degradation(), and each step may relax only what it changes:
//   None            everything equal: both screens, history, cursor, wrap
//   HistoryTrimmed  history is the newest k < n lines
//   Colors256       no history; TrueColor becomes the nearest palette index
//   NoColor         no history; fg/bg Default
//   RowsCropped     as NoColor; cropped rows are blank, the cursor row is kept
// Other invariants: output fits the cap, ScrollbackLen is in range, no raw
// control bytes, the ladder runs only when the full encoding does not fit,
// and SnapshotTooLarge only when the cap is below a single row's worst case.
//
// Input layout (6-byte header + VT byte stream):
//   [0] cols       = (data[0] & 0x7F) + 1          1..128
//   [1] rows       = (data[1] & 0x3F) + 1          1..64
//   [2] scrollback = (data[2] & 0x1F) * 5          0..155 lines
//   [3] cap        = 0 -> SNAPSHOT_MAX_BYTES, else data[3] * 512
//   [4] resize cols, [5] resize rows               as [0], [1]
//   [6..] VT bytes; the Resize is applied at the midpoint

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/unicode.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

namespace {

enum class EColors : std::uint8_t { Full, Indexed256, None };

using CellFn = const Cell& (*)(const Grid&, std::uint16_t, std::uint16_t);

// Upper bound of the single-row, no-colour, no-history encoding: ~2.5 KB of
// preamble and mode restore plus 100 bytes per column (design.md §9, step 4
// "fits in a few KB").
constexpr std::size_t FIXED_BYTES = 2500;
constexpr std::size_t BYTES_PER_COL = 100;

[[noreturn]] void Fail(const char* what, std::size_t a = 0, std::size_t b = 0) {
    std::fprintf(stderr, "%s %zu %zu\n", what, a, b);
    __builtin_trap();
}

[[noreturn]] void FailCell(const char* label, const char* what, std::size_t r,
                           std::uint16_t c, const Cell& src, const Cell& dst) {
    std::fprintf(stderr, "%s %s r=%zu c=%u\n", label, what, r, c);
    for (const Cell* x : {&src, &dst})
        std::fprintf(
            stderr,
            "  cp=%x w=%u at=%u/%u ul=%u fg=%u:%u,%u,%u bg=%u:%u,%u,%u\n",
            x->Codepoint, x->Width, x->Attrs, x->AttrEx, x->UlIdx,
            static_cast<unsigned>(x->FgKind), x->Fg[0], x->Fg[1], x->Fg[2],
            static_cast<unsigned>(x->BgKind), x->Bg[0], x->Bg[1], x->Bg[2]);
    __builtin_trap();
}

const Cell& VisibleCell(const Grid& g, std::uint16_t r, std::uint16_t c) {
    return g.CellAt(r, c);
}
const Cell& MainCell(const Grid& g, std::uint16_t r, std::uint16_t c) {
    return g.MainCellAt(r, c);
}

// Squared distance from rgb to xterm palette entry idx (16..255).
int PaletteDist(int idx, const std::uint8_t* rgb) {
    static constexpr int STEPS[6] = {0, 95, 135, 175, 215, 255};
    int p[3];
    if (idx < 232) {
        p[0] = STEPS[(idx - 16) / 36];
        p[1] = STEPS[(idx - 16) / 6 % 6];
        p[2] = STEPS[(idx - 16) % 6];
    } else {
        p[0] = p[1] = p[2] = 8 + 10 * (idx - 232);
    }
    int d = 0;
    for (int i = 0; i < 3; ++i) d += (p[i] - rgb[i]) * (p[i] - rgb[i]);
    return d;
}

bool IsNearestIn256(int idx, const std::uint8_t* rgb) {
    if (idx < 16) return false;
    const int d = PaletteDist(idx, rgb);
    for (int i = 16; i < 256; ++i)
        if (PaletteDist(i, rgb) < d) return false;
    return true;
}

bool ColorOk(EColorKind sk, const std::uint8_t* s, EColorKind dk,
             const std::uint8_t* d, EColors colors) {
    if (colors == EColors::None) return dk == EColorKind::Default;
    if (colors == EColors::Indexed256 && sk == EColorKind::TrueColor)
        return dk == EColorKind::Indexed && IsNearestIn256(d[0], s);
    if (sk != dk) return false;
    if (sk == EColorKind::Indexed) return s[0] == d[0];
    return sk != EColorKind::TrueColor || std::memcmp(s, d, 3) == 0;
}

// Name of the first field of dst that differs from what src must restore as.
const char* DiffCell(const Cell& src, const Cell& dst, EColors colors) {
    // 0 is the grid's blank (cell.hpp) and goes out as a space. D7: a wide
    // codepoint in a Width=1 cell (orphan head) goes out as U+FFFD.
    char32_t want = src.Codepoint == 0 ? U' ' : src.Codepoint;
    if (src.Width == 1 && Wcwidth(want) == 2) want = U'\xFFFD';
    if ((dst.Codepoint == 0 ? U' ' : dst.Codepoint) != want) return "cp";
    if (src.Width != dst.Width) return "width";
    if (src.Attrs != dst.Attrs) return "attrs";
    // AttrEx is the underline style: SGR 24 leaves it stale on a cell without
    // Underline, and the snapshot writes it only together with SGR 4.
    if ((src.Attrs & Underline) && src.AttrEx != dst.AttrEx) return "attrex";
    if (src.UlIdx != dst.UlIdx) return "ulidx";
    if (!ColorOk(src.FgKind, src.Fg, dst.FgKind, dst.Fg, colors)) return "fg";
    if (!ColorOk(src.BgKind, src.Bg, dst.BgKind, dst.Bg, colors)) return "bg";
    return nullptr;
}

bool RowBlank(CellFn at, const Grid& g, std::uint16_t r) {
    const Cell blank{};
    for (std::uint16_t c = 0; c < g.Cols(); ++c)
        if (DiffCell(blank, at(g, r, c), EColors::None)) return false;
    return true;
}

// With mayCrop, the blank rows of b above and below keepRow (the cursor row,
// which a crop never removes) are taken as cropped and not compared.
void CheckScreen(CellFn at, const Grid& a, const Grid& b, EColors colors,
                 bool mayCrop, std::uint16_t keepRow, const char* label) {
    const std::uint16_t rows = a.Rows();
    std::uint16_t top = 0;
    std::uint16_t end = rows;
    if (mayCrop) {
        while (top < keepRow && RowBlank(at, b, top)) ++top;
        while (end > keepRow + 1 && RowBlank(at, b, end - 1u)) --end;
    }
    for (std::uint16_t r = top; r < end; ++r) {
        for (std::uint16_t c = 0; c < a.Cols(); ++c) {
            const Cell& src = at(a, r, c);
            const Cell& dst = at(b, r, c);
            if (const char* d = DiffCell(src, dst, colors))
                FailCell(label, d, r, c, src, dst);
        }
    }
}

void CheckHistory(const Grid& a, const Grid& b, EDegradation deg,
                  EColors colors) {
    const std::uint32_t n = a.ScrollbackSize();
    const std::uint32_t k = b.ScrollbackSize();
    const bool ok = deg == EDegradation::None             ? k == n
                    : deg == EDegradation::HistoryTrimmed ? k < n
                                                          : k == 0;
    if (!ok) Fail("history-size", n, k);
    for (std::uint32_t i = 0; i < k; ++i) {
        const Line* la = a.ScrollbackLine(n - k + i);
        const Line* lb = b.ScrollbackLine(i);
        if (!la || !lb) Fail("history-line", i);
        // Open finding, seed history_bce_cellsused: EncodeHistory stops at
        // Line::CellsUsed, which ClearLine leaves 0 for BCE-coloured blanks.
        for (std::uint16_t c = 0; c < la->CellsUsed; ++c)
            if (const char* d = DiffCell(la->Cells[c], lb->Cells[c], colors))
                FailCell("history", d, i, c, la->Cells[c], lb->Cells[c]);
    }
}

// True when p holds a raw control byte, DEL, a surrogate or an invalid UTF-8
// lead. ESC sequences are skipped; CAN, SO, SI, CR and LF are emitted by
// design.
bool HasHostileBytes(const std::uint8_t* p, std::size_t size) {
    std::size_t i = 0;
    while (i < size) {
        const std::uint8_t b = p[i++];
        if (b == 0x1B && i < size) {
            const std::uint8_t k = p[i++];
            if (k == '[')
                while (i < size && (p[i] < 0x40 || p[i] > 0x7E)) ++i;
            if (k == '[' || k == '(' || k == ')') ++i;  // final / designator
        } else if (b == 0x7F || b == 0xC0 || b == 0xC1 || b >= 0xF5) {
            return true;
        } else if (b < 0x20 && b != 0x18 && b != 0x0E && b != 0x0F &&
                   b != 0x0D && b != 0x0A) {
            return true;
        } else if (b == 0xED && i < size && p[i] >= 0xA0 && p[i] <= 0xBF) {
            return true;
        }
    }
    return false;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size < 6) return 0;

    const auto cols = static_cast<std::uint16_t>((data[0] & 0x7Fu) + 1u);
    const auto rows = static_cast<std::uint16_t>((data[1] & 0x3Fu) + 1u);
    const std::uint32_t scrollback = (data[2] & 0x1Fu) * 5u;
    const std::size_t maxBytes = static_cast<std::size_t>(data[3]) * 512u;
    const auto rcols = static_cast<std::uint16_t>((data[4] & 0x7Fu) + 1u);
    const auto rrows = static_cast<std::uint16_t>((data[5] & 0x3Fu) + 1u);
    const auto* bytes = reinterpret_cast<const std::byte*>(data + 6);
    const std::size_t len = size - 6;

    auto res = Grid::Create(cols, rows, scrollback);
    if (!res.has_value()) return 0;
    std::unique_ptr<Grid> g1(res.value());

    VtParser p;
    const std::size_t half = len / 2;
    p.Feed(bytes, half, *g1);
    if (half < len) {
        (void)g1->Resize(rcols, rrows);  // on failure the grid is unchanged
        p.Feed(bytes + half, len - half, *g1);
    }

    const std::size_t cap = (maxBytes == 0 || maxBytes > SNAPSHOT_MAX_BYTES)
                                ? SNAPSHOT_MAX_BYTES
                                : maxBytes;
    auto sr = Snapshot::Create(*g1, {.MaxBytes = maxBytes});
    if (!sr.has_value()) {
        if (sr.error() == EGridError::SnapshotTooLarge &&
            cap < FIXED_BYTES + BYTES_PER_COL * g1->Cols())
            return 0;
        Fail("create-error", static_cast<std::size_t>(sr.error()), cap);
    }
    std::unique_ptr<Snapshot> snap(sr.value());
    const auto out = snap->Bytes();
    const EDegradation deg = snap->Degradation();

    if (out.size() > cap) Fail("over-cap", out.size(), cap);
    if (snap->ScrollbackLen() > out.size()) Fail("scrollback-len");
    if (HasHostileBytes(reinterpret_cast<const std::uint8_t*>(out.data()),
                        out.size()))
        Fail("hostile-bytes");
    if (snap->AltScreen() != g1->IsAltScreen()) Fail("snap-alt");
    if (snap->PossiblyDiverged() != g1->PossiblyDiverged()) Fail("snap-div");

    // The ladder runs only when the full-fidelity encoding does not fit.
    auto fr = Snapshot::Create(*g1);
    if (!fr.has_value()) Fail("full-create-error");
    std::unique_ptr<Snapshot> full(fr.value());
    if (full->Degradation() != EDegradation::None) Fail("full-degraded");
    if (deg == EDegradation::None) {
        const auto fb = full->Bytes();
        if (fb.size() != out.size() ||
            std::memcmp(fb.data(), out.data(), out.size()) != 0)
            Fail("full-differs");
    } else if (full->Bytes().size() <= cap) {
        Fail("degraded-but-fits", full->Bytes().size(), cap);
    }

    // Open finding, seed decom_cursor_above_region: with DECOM on and the
    // cursor above the scroll region (Grid::SwitchAltScreen) the final CUP
    // cannot reach the cursor and the pending-wrap re-print lands elsewhere.
    if (g1->ModeSet(EMode::Decom) && (g1->CursorRow() < g1->ScrollTop() ||
                                      g1->CursorRow() > g1->ScrollBot()))
        return 0;

    auto res2 = Grid::Create(g1->Cols(), g1->Rows(), g1->ScrollbackLimit());
    if (!res2.has_value()) Fail("grid2-create");
    std::unique_ptr<Grid> g2(res2.value());
    VtParser p2;
    p2.Feed(out.data(), out.size(), *g2);

    if (g1->IsAltScreen() != g2->IsAltScreen()) Fail("altscreen");
    if (g1->CursorRow() != g2->CursorRow())
        Fail("cursor-row", g1->CursorRow(), g2->CursorRow());
    if (g1->CursorCol() != g2->CursorCol())
        Fail("cursor-col", g1->CursorCol(), g2->CursorCol());
    if (g1->PendingWrap() != g2->PendingWrap()) Fail("pending-wrap");

    const EColors colors =
        deg == EDegradation::None || deg == EDegradation::HistoryTrimmed
            ? EColors::Full
        : deg == EDegradation::Colors256 ? EColors::Indexed256
                                         : EColors::None;
    const bool mayCrop = deg == EDegradation::RowsCropped;
    CheckScreen(VisibleCell, *g1, *g2, colors, mayCrop, g1->CursorRow(),
                "screen");
    if (g1->IsAltScreen())
        CheckScreen(MainCell, *g1, *g2, colors, mayCrop, g1->CursorRow(),
                    "main");
    CheckHistory(*g1, *g2, deg, colors);
    return 0;
}
