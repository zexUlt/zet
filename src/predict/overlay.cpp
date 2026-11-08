#include "zet/predict/overlay.hpp"

#include <algorithm>
#include <new>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/sgr.hpp"

namespace zet::predict {

namespace {

/// What the terminal has been told to draw with: the mirror's pen as a cell,
/// so that the SGR encoder of the snapshot can say it.
grid::Cell PenCell(const grid::Grid& g) noexcept {
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

grid::Pen PenOf(const grid::Cell& c) noexcept {
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
char32_t Printable(char32_t cp) noexcept {
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F) ||
        (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        return U'?';
    }
    return cp;
}

bool IsBlank(const grid::Cell& c) noexcept {
    return c.Codepoint == 0 || c.Codepoint == U' ';
}

void Column(grid::EscWriter& w, std::uint16_t col) noexcept {
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(static_cast<std::uint32_t>(col) + 1U);
    w.WriteByte('G');
}

/// Hides the cursor while the overlay is written, inside a synchronised
/// update unless the program has its own open: closing that one would end its
/// frame.
void Begin(grid::EscWriter& w, const grid::Grid& g) noexcept {
    if (!g.ModeSet(grid::EMode::SyncOutput)) w.WriteLiteral("\x1b[?2026h");
    w.WriteLiteral("\x1b[?25l");
}

/// The pen and the cursor back where the mirror has them, the cursor visible
/// if the mirror says so.
void End(grid::EscWriter& w, grid::Pen& pen, const grid::Grid& g,
         std::uint16_t cursorCol) noexcept {
    grid::EmitSgr(w, pen, PenCell(g));
    Column(w, cursorCol);
    if (g.ModeSet(grid::EMode::Dectcem)) w.WriteLiteral("\x1b[?25h");
    if (!g.ModeSet(grid::EMode::SyncOutput)) w.WriteLiteral("\x1b[?2026l");
}

/// A cell as the mirror has it, a wide glyph whole. An empty cell goes through
/// ECH with the background of the cell, not a space: a space would change what
/// a selection copies.
void Restore(grid::EscWriter& w, grid::Pen& pen, const grid::Grid& g,
             std::uint16_t row, std::uint16_t col) noexcept {
    // The half of a wide glyph is put back by writing the glyph whole, from
    // its head: the terminal has the glyph broken, or something in its place.
    if (g.CellAt(row, col).Width == 0 && col > 0 &&
        g.CellAt(row, static_cast<std::uint16_t>(col - 1)).Width == 2) {
        --col;
    }
    const grid::Cell& cell = g.CellAt(row, col);
    if (cell.Width == 0) return;  // nothing the mirror can say about it
    Column(w, col);
    grid::EmitSgr(w, pen, cell);
    if (cell.Codepoint == 0) {
        w.WriteLiteral("\x1b[X");
    } else {
        w.WriteUtf8(Printable(cell.Codepoint));
    }
}

}  // namespace

std::unique_ptr<Overlay> Overlay::Create() noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return std::unique_ptr<Overlay>(new (std::nothrow) Overlay());
}

bool Overlay::State::Same(const State& other) const noexcept {
    if (Count != other.Count || Cursor != other.Cursor) return false;
    for (std::size_t i = 0; i < Count; ++i) {
        if (!Cells[i].SameCell(other.Cells[i])) return false;
    }
    return true;
}

ByteSpan Overlay::BuildUndo(const MirrorView& mirror) noexcept {
    Pending_ = EPending::None;
    if (!Drawn()) return {};
    const grid::Grid& g = mirror.Screen;
    grid::EscWriter w{Buf_.data(), Buf_.size()};
    Begin(w, g);
    grid::Pen pen = PenOf(PenCell(g));
    for (std::size_t i = 0; i < Drawn_.Count; ++i) {
        const Placed& p = Drawn_.Cells[i];
        if (p.Row < g.Rows() && p.Col < g.Cols()) {
            Restore(w, pen, g, p.Row, p.Col);
        }
    }
    End(w, pen, g, g.CursorCol());
    if (w.Overflow) return {};
    Pending_ = EPending::Undo;
    return ByteSpan{Buf_.data(), w.Pos};
}

bool Overlay::UndoSurvivesResize(const MirrorView& mirror,
                                 std::uint16_t newCols) const noexcept {
    if (!Drawn()) return true;
    const grid::Grid& g = mirror.Screen;
    const std::uint16_t width = std::min(g.Cols(), newCols);
    if (g.CursorCol() >= width) return false;
    // The terminal may reflow a row that continues into another. The mirror
    // keeps no wrap flags; a row whose last column holds text, or that follows
    // one, may be wrapped, and a row the mirror does not know the one above
    // is taken as not continued (the row is not known at all, ADR-037).
    const auto filled = [&g](std::uint16_t row) {
        return !IsBlank(
            g.CellAt(row, static_cast<std::uint16_t>(g.Cols() - 1)));
    };
    for (std::size_t i = 0; i < Drawn_.Count; ++i) {
        const Placed& p = Drawn_.Cells[i];
        if (p.Row >= g.Rows() || p.Col >= width) return false;
        if (filled(p.Row) || (p.Row > 0 && filled(p.Row - 1))) return false;
    }
    if (g.CursorRow() < g.Rows() &&
        (filled(g.CursorRow()) ||
         (g.CursorRow() > 0 && filled(g.CursorRow() - 1)))) {
        return false;
    }
    return true;
}

ByteSpan Overlay::BuildUpdate(const PredictionEngine& engine,
                              const MirrorView& mirror) noexcept {
    Pending_ = EPending::None;
    const grid::Grid& g = mirror.Screen;
    // The terminal takes bytes here only at a boundary of what the agent sent.
    const bool ground = mirror.Parser.State() == grid::EParserState::Ground &&
                        !mirror.Parser.Utf8Pending();
    const bool canDraw = engine.Mode() != EDisplayMode::Never && ground &&
                         !g.IsAltScreen() && !g.PendingWrap() &&
                         !g.ModeSet(grid::EMode::Irm) && g.CharsetG0() == 0 &&
                         g.CharsetActive() == 0 && engine.ColumnTrusted() &&
                         g.CursorRow() < g.Rows() && g.CursorCol() < g.Cols();
    // What is drawn is taken off only at a point where bytes may go in; the
    // overlay is only ever drawn at one, and the next piece of bytes comes
    // after a restore, so a point that is not one has nothing to take off.
    if (!canDraw) return {};

    Next_.Count = 0;
    Next_.Cursor.reset();
    if (engine.ShowPredictions()) {
        const bool underline = engine.UnderlinePredictions();
        const auto preds = engine.Predictions();
        for (std::size_t i = 0; i < preds.size(); ++i) {
            const Prediction& p = preds[i];
            if (!engine.IsVisible(p) || p.Row != g.CursorRow() ||
                p.Col >= g.Cols()) {
                continue;
            }
            const grid::Cell& cell = g.CellAt(p.Row, p.Col);
            // Never on a wide glyph or the half of one, whatever the engine
            // thinks of the cell: the terminal has a glyph there that a narrow
            // one would break.
            if (cell.Width != 1 ||
                (p.Col > 0 &&
                 g.CellAt(p.Row, static_cast<std::uint16_t>(p.Col - 1)).Width ==
                     2)) {
                continue;
            }
            const bool under = underline && p.Codepoint != 0;
            // The terminal already shows it, the program's echo is there: a
            // second drawing would change nothing but the colours.
            const bool shown = p.Codepoint == 0 ? IsBlank(cell)
                                                : cell.Codepoint == p.Codepoint;
            if (shown && !under) continue;
            if (Next_.Count >= Next_.Cells.size()) break;
            Next_.Cells[Next_.Count++] =
                Placed{.Row = p.Row,
                       .Col = p.Col,
                       .Codepoint = p.Codepoint,
                       .Underline = under,
                       .Index = static_cast<std::uint16_t>(i)};
        }
        const auto& cursor = engine.Cursor();
        if (cursor && cursor->Epoch <= engine.ConfirmedEpoch() &&
            cursor->Row == g.CursorRow() && cursor->Col < g.Cols() &&
            cursor->Col != g.CursorCol()) {
            Next_.Cursor = cursor->Col;
        }
    }
    if (Next_.Same(Drawn_)) return {};

    grid::EscWriter w{Buf_.data(), Buf_.size()};
    Begin(w, g);
    const grid::Cell base = PenCell(g);
    grid::Pen pen = PenOf(base);
    for (std::size_t i = 0; i < Drawn_.Count; ++i) {
        const Placed& p = Drawn_.Cells[i];
        if (p.Row < g.Rows() && p.Col < g.Cols()) {
            Restore(w, pen, g, p.Row, p.Col);
        }
    }
    for (std::size_t i = 0; i < Next_.Count; ++i) {
        const Placed& p = Next_.Cells[i];
        grid::Cell style = base;
        if (p.Underline) style.Attrs |= grid::Underline;
        Column(w, p.Col);
        grid::EmitSgr(w, pen, style);
        if (p.Codepoint == 0) {
            w.WriteLiteral("\x1b[X");
        } else {
            w.WriteUtf8(Printable(p.Codepoint));
        }
    }
    End(w, pen, g, Next_.Cursor.value_or(g.CursorCol()));
    if (w.Overflow) return {};
    Pending_ = EPending::Update;
    return ByteSpan{Buf_.data(), w.Pos};
}

void Overlay::Commit(PredictionEngine& engine) noexcept {
    switch (Pending_) {
        case EPending::None:
            return;
        case EPending::Undo:
            Drawn_.Count = 0;
            Drawn_.Cursor.reset();
            engine.ClearDrawn();
            break;
        case EPending::Update:
            Drawn_ = Next_;
            engine.ClearDrawn();
            for (std::size_t i = 0; i < Drawn_.Count; ++i) {
                engine.SetDrawn(Drawn_.Cells[i].Index, true);
            }
            break;
    }
    Pending_ = EPending::None;
}

void Overlay::Forget() noexcept {
    Drawn_.Count = 0;
    Drawn_.Cursor.reset();
    Pending_ = EPending::None;
}

}  // namespace zet::predict
