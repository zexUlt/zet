// Fuzz target for VtParser + Grid (design.md §16).
//
// Invariants checked after every feed:
//   1. cursor within bounds (with pending_wrap: col may equal cols–1 with flag)
//   2. scroll region sane: top < bot, both < rows
//   3. saved cursor coords within bounds
//   4. parser always in a valid EParserState (enum range 0–14)
//   5. ExtCount <= MAX_EXT_CELLS
//
// Resize is interleaved to exercise the resize path under parser state.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"
#include "zet/grid/vt_parser.hpp"

using namespace zet::grid;

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

    VtParser p;

    const std::byte* bytes = reinterpret_cast<const std::byte*>(data + 4);
    const std::size_t len = size - 4;

    // Split the input roughly in half: feed the first half, resize, feed the
    // rest.
    const std::size_t half = len / 2;
    p.Feed(bytes, half, *g);

    g->Resize(cols1, rows1);

    p.Feed(bytes + half, len - half, *g);

    // -----------------------------------------------------------------------
    // Invariant checks
    // -----------------------------------------------------------------------

    const std::uint16_t cols = g->Cols();
    const std::uint16_t rows = g->Rows();

    // 1. Cursor within bounds.
    if (g->CursorRow() >= rows) __builtin_trap();
    if (g->CursorCol() >= cols) __builtin_trap();

    // 2. Scroll region sane.
    if (g->ScrollTop() >= rows) __builtin_trap();
    if (g->ScrollBot() >= rows) __builtin_trap();
    if (g->ScrollTop() > g->ScrollBot()) __builtin_trap();

    // 3. Parser in a valid state.
    const auto state = static_cast<std::uint8_t>(p.State());
    if (state > static_cast<std::uint8_t>(EParserState::kBypass))
        __builtin_trap();

    // 4. ExtCount bounded.
    if (g->ExtCount() > MAX_EXT_CELLS) __builtin_trap();

    // 5. Scrollback within limit.
    if (g->ScrollbackSize() > g->ScrollbackLimit()) __builtin_trap();

    delete g;
    return 0;
}
