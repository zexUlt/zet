// Fuzz target for VtParser + Grid (design.md §16).
//
// Invariants checked after EVERY feed step and every Resize:
//   1. cursor within bounds (col may equal cols–1 when PendingWrap is set)
//   2. scroll region sane: top <= bot, both < rows
//   3. saved cursor (main and alt) within current grid bounds
//   4. parser always in a valid EParserState (enum range 0..Bypass)
//   5. scrollback within limit; scrollback-0 grid never has history
//   6. wide-char pairing: a width==2 cell is followed by width==0;
//      a width==0 cell is preceded by width==2; width==2 NOT in last column
//   7. pending-wrap relation: PendingWrap only when col == cols–1
//   8. byte-at-a-time (with scrollback) produces identical screen to bulk:
//      all cell fields (codepoint, width, attrs, colours) and cursor state
//
// Resize count and targets are variable and driven by input bytes.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

static void CheckInvariants(const Grid& g, const VtParser& p) noexcept {
    const std::uint16_t cols = g.Cols();
    const std::uint16_t rows = g.Rows();

    // 1. Cursor within bounds.
    if (g.CursorRow() >= rows) __builtin_trap();
    if (g.CursorCol() >= cols) __builtin_trap();

    // 7. PendingWrap only valid when cursor is at the last column.
    if (g.PendingWrap() &&
        g.CursorCol() != static_cast<std::uint16_t>(cols - 1))
        __builtin_trap();

    // 2. Scroll region sane.
    if (g.ScrollTop() >= rows) __builtin_trap();
    if (g.ScrollBot() >= rows) __builtin_trap();
    if (g.ScrollTop() > g.ScrollBot()) __builtin_trap();

    // 3. Saved cursor within current grid bounds.
    {
        const SavedCursor& sm = g.SavedCursorMainState();
        if (sm.Row >= rows) __builtin_trap();
        if (sm.Col >= cols) __builtin_trap();
        const SavedCursor& sa = g.SavedCursorAltState();
        if (sa.Row >= rows) __builtin_trap();
        if (sa.Col >= cols) __builtin_trap();
    }

    // 4. Parser in a valid state.
    const auto state = static_cast<std::uint8_t>(p.State());
    if (state > static_cast<std::uint8_t>(EParserState::Bypass))
        __builtin_trap();

    // 5. Scrollback within limit; scrollback-0 never has entries.
    if (g.ScrollbackSize() > g.ScrollbackLimit()) __builtin_trap();
    if (g.ScrollbackLimit() == 0 && g.ScrollbackSize() != 0) __builtin_trap();

    // 6. Wide-char pairing invariant.
    //    Lead (width==2) must NOT be in the last column.
    //    Lead must be followed by continuation (width==0).
    //    Continuation (width==0) must be preceded by a lead — no orphans.
    for (std::uint16_t r = 0; r < rows; ++r) {
        for (std::uint16_t c = 0; c < cols; ++c) {
            const Cell& cell = g.CellAt(r, c);
            if (cell.Width == 2) {
                // Wide head must not be in the last column.
                if (c + 1 >= cols) __builtin_trap();
                const Cell& next =
                    g.CellAt(r, static_cast<std::uint16_t>(c + 1));
                if (next.Width != 0) __builtin_trap();
            } else if (cell.Width == 0) {
                if (c == 0) __builtin_trap();
                const Cell& prev =
                    g.CellAt(r, static_cast<std::uint16_t>(c - 1));
                if (prev.Width != 2) __builtin_trap();
            }
        }
    }
}

// libFuzzer entry point.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size < 4) return 0;

    // Bytes 0-1: initial cols/rows — allow odd values to cover 1-column grids
    // and other edge sizes.  Max encoded: 0x7F+1=128 cols, 0x3F+1=64 rows.
    const std::uint16_t cols0 = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[0] & 0x7F) + 1);
    const std::uint16_t rows0 = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[1] & 0x3F) + 1);

    // Bytes 2-3: first resize target — same range, also odd-friendly.
    const std::uint16_t cols1 = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[2] & 0x7F) + 1);
    const std::uint16_t rows1 = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[3] & 0x3F) + 1);

    const std::byte* bytes = reinterpret_cast<const std::byte*>(data + 4);
    const std::size_t len = size - 4;

    // -----------------------------------------------------------------------
    // 8. Byte-at-a-time vs bulk feed with ALL cell fields and cursor state.
    //    Enable scrollback (20 lines) to exercise history paths.
    // -----------------------------------------------------------------------
    {
        auto rByte = Grid::Create(cols0, rows0, 20);
        auto rBulk = Grid::Create(cols0, rows0, 20);
        if (rByte.has_value() && rBulk.has_value()) {
            Grid* gByte = rByte.value();
            Grid* gBulk = rBulk.value();
            VtParser pByte, pBulk;

            for (std::size_t i = 0; i < len; ++i)
                pByte.Feed(bytes + i, 1, *gByte);
            pBulk.Feed(bytes, len, *gBulk);

            // Compare every cell field.
            for (std::uint16_t r = 0; r < rows0; ++r) {
                for (std::uint16_t c = 0; c < cols0; ++c) {
                    const Cell& a = gByte->CellAt(r, c);
                    const Cell& b = gBulk->CellAt(r, c);
                    if (a.Codepoint != b.Codepoint) __builtin_trap();
                    if (a.Width != b.Width) __builtin_trap();
                    if (a.Attrs != b.Attrs) __builtin_trap();
                    if (a.FgKind != b.FgKind) __builtin_trap();
                    if (a.BgKind != b.BgKind) __builtin_trap();
                    if (a.Fg[0] != b.Fg[0]) __builtin_trap();
                    if (a.Fg[1] != b.Fg[1]) __builtin_trap();
                    if (a.Fg[2] != b.Fg[2]) __builtin_trap();
                    if (a.Bg[0] != b.Bg[0]) __builtin_trap();
                    if (a.Bg[1] != b.Bg[1]) __builtin_trap();
                    if (a.Bg[2] != b.Bg[2]) __builtin_trap();
                    if (a.AttrEx != b.AttrEx) __builtin_trap();
                    if (a.UlIdx != b.UlIdx) __builtin_trap();
                }
            }
            // Compare cursor state.
            if (gByte->CursorRow() != gBulk->CursorRow()) __builtin_trap();
            if (gByte->CursorCol() != gBulk->CursorCol()) __builtin_trap();
            if (gByte->PendingWrap() != gBulk->PendingWrap()) __builtin_trap();

            delete gByte;
            delete gBulk;
        } else {
            if (rByte.has_value()) delete rByte.value();
            if (rBulk.has_value()) delete rBulk.value();
        }
    }

    // -----------------------------------------------------------------------
    // Main grid: feed one byte at a time with invariant checks after EACH
    // byte, and interleave a variable number of Resize calls driven by input.
    // -----------------------------------------------------------------------
    auto res = Grid::Create(cols0, rows0, 100);
    if (!res.has_value()) return 0;
    Grid* g = res.value();

    VtParser p;
    p.SetBypassCallback([](EBypassEvent, std::byte, void*) {}, nullptr);

    // Decide the resize schedule: each resize is triggered when the byte
    // index is a multiple of (resize_period+1), where resize_period is
    // encoded in data[2] bits 7 (gives 1..128 period).  Resize dimensions
    // are derived from data[2] and data[3] for the first resize, and swap
    // cols/rows for subsequent resizes.
    const std::size_t resizePeriod =
        static_cast<std::size_t>((data[2] >> 1 & 0x3F) + 1);  // 1..64

    std::uint16_t rcols = cols1;
    std::uint16_t rrows = rows1;

    // Also keep a 1×1 grid alive to prove it survives the entire input.
    Grid* g1x1 = nullptr;
    VtParser p1x1;
    {
        auto r1 = Grid::Create(1, 1, 0);
        if (r1.has_value()) g1x1 = r1.value();
    }

    for (std::size_t i = 0; i < len; ++i) {
        p.Feed(bytes + i, 1, *g);
        CheckInvariants(*g, p);

        if (g1x1) {
            p1x1.Feed(bytes + i, 1, *g1x1);
            // 1×1 grid has fixed size: just check it doesn't crash.
            if (g1x1->CursorRow() >= g1x1->Rows()) __builtin_trap();
            if (g1x1->CursorCol() >= g1x1->Cols()) __builtin_trap();
        }

        // Interleave resizes.
        if ((i + 1) % resizePeriod == 0) {
            (void)g->Resize(rcols, rrows);
            CheckInvariants(*g, p);
            // Swap for next resize to cover both grow and shrink.
            std::uint16_t tmp = rcols;
            rcols = rrows;
            rrows = tmp;
        }
    }

    // One final explicit 1×1 resize to exercise the extreme shrink path.
    (void)g->Resize(1, 1);
    CheckInvariants(*g, p);

    delete g;
    if (g1x1) delete g1x1;
    return 0;
}
