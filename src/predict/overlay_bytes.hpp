#pragma once

// The pieces of byte writing the overlay and the width probe share (design.md
// §9): a probe is an overlay write with a query, so it hides the cursor, puts
// the cells back and the pen and the cursor after itself with the very same
// code. Private to zet_predict.

#include <cstdint>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/sgr.hpp"

namespace zet::predict::emit {

/// What the terminal has been told to draw with: the mirror's pen as a cell,
/// so that the SGR encoder of the snapshot can say it.
inline grid::Cell PenCell(const grid::Grid& g) noexcept {
    grid::Cell c;
    c.Attrs = g.PenAttrs();
    c.FgKind = g.PenFgKind();
    c.BgKind = g.PenBgKind();
    for (int i = 0; i < 3; ++i) {
        c.Fg[i] = g.PenFg()[i];
        c.Bg[i] = g.PenBg()[i];
    }
    c.AttrEx = g.PenAttrEx();
    c.UlIdx = g.PenUlIdx();
    return c;
}

inline grid::Pen PenOf(const grid::Cell& c) noexcept {
    grid::Pen pen;
    pen.Attrs = c.Attrs;
    pen.FgKind = c.FgKind;
    pen.BgKind = c.BgKind;
    for (int i = 0; i < 3; ++i) {
        pen.Fg[i] = c.Fg[i];
        pen.Bg[i] = c.Bg[i];
    }
    pen.AttrEx = c.AttrEx;
    pen.UlIdx = c.UlIdx;
    return pen;
}

/// A glyph that is safe to write back to a terminal, as the snapshot has it:
/// nothing a terminal would take for a control.
inline char32_t Printable(char32_t cp) noexcept {
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        return U'?';
    }
    return cp;
}

inline void Column(grid::EscWriter& w, std::uint16_t col) noexcept {
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(static_cast<std::uint32_t>(col) + 1U);
    w.WriteByte('G');
}

/// Hides the cursor while the overlay is written, inside a synchronised
/// update unless the program has its own open: closing that one would end its
/// frame.
inline void Begin(grid::EscWriter& w, const grid::Grid& g) noexcept {
    if (!g.ModeSet(grid::EMode::SyncOutput)) w.WriteLiteral("\x1b[?2026h");
    w.WriteLiteral("\x1b[?25l");
}

/// The pen and the cursor back where the mirror has them, the cursor visible
/// if the mirror says so.
inline void End(grid::EscWriter& w, grid::Pen& pen, const grid::Grid& g,
                std::uint16_t cursorCol) noexcept {
    grid::EmitSgr(w, pen, PenCell(g));
    Column(w, cursorCol);
    if (g.ModeSet(grid::EMode::Dectcem)) w.WriteLiteral("\x1b[?25h");
    if (!g.ModeSet(grid::EMode::SyncOutput)) w.WriteLiteral("\x1b[?2026l");
}

/// The column Restore writes from: the half of a wide glyph is put back by
/// writing the glyph whole, from its head, because the terminal has the glyph
/// broken, or something in its place.
inline std::uint16_t RestoredCol(const grid::Grid& g, std::uint16_t row,
                                 std::uint16_t col) noexcept {
    if (g.CellAt(row, col).Width == 0 && col > 0 &&
        g.CellAt(row, static_cast<std::uint16_t>(col - 1)).Width == 2) {
        return static_cast<std::uint16_t>(col - 1);
    }
    return col;
}

/// A cell as the mirror has it, a wide glyph whole. An empty cell goes through
/// ECH with the background of the cell, not a space: a space would change what
/// a selection copies.
/// Returns the glyph it printed, 0 for none: a printed glyph is what a REP on
/// the terminal repeats next.
inline char32_t Restore(grid::EscWriter& w, grid::Pen& pen, const grid::Grid& g,
                        std::uint16_t row, std::uint16_t col) noexcept {
    col = RestoredCol(g, row, col);
    const grid::Cell& cell = g.CellAt(row, col);
    if (cell.Width == 0) return 0;  // nothing the mirror can say about it
    Column(w, col);
    grid::EmitSgr(w, pen, cell);
    if (cell.Codepoint == 0) {
        w.WriteLiteral("\x1b[X");
        return 0;
    }
    w.WriteUtf8(Printable(cell.Codepoint));
    return Printable(cell.Codepoint);
}

}  // namespace zet::predict::emit
