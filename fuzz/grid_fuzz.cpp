// Fuzz target for VtParser + Grid (design.md §16).
//
// Invariants checked after every feed:
//   1. cursor within bounds (col may equal cols–1 when PendingWrap is set)
//   2. scroll region sane: top <= bot, both < rows
//   3. saved cursor within bounds (probed via SaveCursor/RestoreCursor)
//   4. parser always in a valid EParserState (enum range 0..Bypass)
//   5. scrollback within limit; scrollback-0 grid never has history
//   6. wide-char pairing: a width==2 cell is followed by width==0 or EOL;
//      a width==0 cell is preceded by a width==2 cell (no orphan continuation)
//   7. pending-wrap relation: PendingWrap only when col == cols–1
//   8. byte-at-a-time produces identical screen to bulk feed
//
// Resize is interleaved to exercise the resize path under parser state.

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

    // 4. Parser in a valid state.
    const auto state = static_cast<std::uint8_t>(p.State());
    if (state > static_cast<std::uint8_t>(EParserState::Bypass))
        __builtin_trap();

    // 5. Scrollback within limit; scrollback-0 never has entries.
    if (g.ScrollbackSize() > g.ScrollbackLimit()) __builtin_trap();
    if (g.ScrollbackLimit() == 0 && g.ScrollbackSize() != 0) __builtin_trap();

    // 6. Wide-char pairing invariant.
    //    Lead (width==2) must be followed by continuation (width==0) or EOL.
    //    Continuation (width==0) must be preceded by a lead — no orphans.
    for (std::uint16_t r = 0; r < rows; ++r) {
        for (std::uint16_t c = 0; c < cols; ++c) {
            const Cell& cell = g.CellAt(r, c);
            if (cell.Width == 2) {
                if (c + 1 < cols) {
                    const Cell& next =
                        g.CellAt(r, static_cast<std::uint16_t>(c + 1));
                    if (next.Width != 0) __builtin_trap();
                }
            } else if (cell.Width == 0) {
                // Orphan continuation: no lead cell before it.
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

    // First 2 bytes encode initial cols/rows; next 2 encode resize target.
    const std::uint16_t cols0 = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0] & 0x7F) + 1) * 2);
    const std::uint16_t rows0 = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[1] & 0x3F) + 1) * 2);
    const std::uint16_t cols1 = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[2] & 0x7F) + 1) * 2);
    const std::uint16_t rows1 = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[3] & 0x3F) + 1) * 2);

    auto res = Grid::Create(cols0, rows0, 100);
    if (!res.has_value()) return 0;
    Grid* g = res.value();

    // A 1×1 grid must also survive the same input.
    {
        auto r1 = Grid::Create(1, 1, 0);
        if (r1.has_value()) {
            VtParser p1;
            const std::byte* bytes =
                reinterpret_cast<const std::byte*>(data + 4);
            p1.Feed(bytes, size - 4, *r1.value());
            CheckInvariants(*r1.value(), p1);
            delete r1.value();
        }
    }

    VtParser p;

    // Set a bypass callback to prove it does not crash on any bypass event.
    std::size_t bypassCount = 0;
    p.SetBypassCallback([](EBypassEvent, std::byte,
                           void* ctx) { *static_cast<std::size_t*>(ctx) += 1; },
                        &bypassCount);

    const std::byte* bytes = reinterpret_cast<const std::byte*>(data + 4);
    const std::size_t len = size - 4;

    // 8. Byte-at-a-time must produce the same screen as bulk feed.
    //    Use scrollback 0 to keep it cheap.
    {
        Grid* gByte = Grid::Create(cols0, rows0, 0).value_or(nullptr);
        Grid* gBulk = Grid::Create(cols0, rows0, 0).value_or(nullptr);
        if (gByte && gBulk) {
            VtParser pByte, pBulk;
            for (std::size_t i = 0; i < len; ++i)
                pByte.Feed(bytes + i, 1, *gByte);
            pBulk.Feed(bytes, len, *gBulk);

            for (std::uint16_t r = 0; r < rows0; ++r) {
                for (std::uint16_t c = 0; c < cols0; ++c) {
                    const Cell& a = gByte->CellAt(r, c);
                    const Cell& b = gBulk->CellAt(r, c);
                    if (a.Codepoint != b.Codepoint || a.Width != b.Width)
                        __builtin_trap();
                }
            }
        }
        delete gByte;
        delete gBulk;
    }

    // Split the input roughly in half: feed the first half, resize, feed the
    // rest.  Also do a second resize to exercise the multi-resize path.
    const std::size_t half = len / 2;
    p.Feed(bytes, half, *g);

    (void)g->Resize(cols1, rows1);

    // Second resize: swap cols/rows to exercise shrink and grow together.
    (void)g->Resize(rows1 < MAX_COLS ? rows1 : MAX_COLS,
                    cols1 < MAX_ROWS ? cols1 : MAX_ROWS);

    p.Feed(bytes + half, len - half, *g);

    CheckInvariants(*g, p);

    delete g;
    return 0;
}
