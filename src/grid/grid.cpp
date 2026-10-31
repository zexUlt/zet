#include "zet/grid/grid.hpp"

#include <algorithm>
#include <cstring>

namespace zet::grid {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GridResult<Grid*> Grid::Create(std::uint16_t cols, std::uint16_t rows,
                               std::uint32_t scrollbackLimit) noexcept {
    if (cols == 0 || cols > MAX_COLS || rows == 0 || rows > MAX_ROWS)
        return std::unexpected(EGridError::SizeTooLarge);
    if (scrollbackLimit > 0x7FFF'FFFF)
        return std::unexpected(EGridError::ScrollbackTooLarge);

    Grid* g = new (std::nothrow) Grid;
    if (!g) return std::unexpected(EGridError::AllocationFailed);

    if (!g->Allocate(cols, rows, scrollbackLimit)) {
        delete g;
        return std::unexpected(EGridError::AllocationFailed);
    }
    return g;
}

bool Grid::Allocate(std::uint16_t cols, std::uint16_t rows,
                    std::uint32_t scrollbackLimit) noexcept {
    Cols_ = cols;
    Rows_ = rows;
    ScrollbackLimit_ = scrollbackLimit;
    ScrollBot_ = static_cast<std::uint16_t>(rows - 1);

    const std::size_t screenCells = static_cast<std::size_t>(cols) * rows;
    // Guard: both screen arenas together must be strictly less than the
    // scrollback budget.  Prevents Create(MAX_COLS, MAX_ROWS, ...) which
    // needs 2 × 64 MiB = 128 MiB for screens alone, leaving nothing for
    // history and hitting the practical memory ceiling.
    if (screenCells * sizeof(Cell) * 2 >= MAX_SCROLLBACK_BYTES) return false;
    MainCells_ = new (std::nothrow) Cell[screenCells]{};
    AltCells_ = new (std::nothrow) Cell[screenCells]{};
    MainLines_ = new (std::nothrow) Line[rows]{};
    AltLines_ = new (std::nothrow) Line[rows]{};

    if (!MainCells_ || !AltCells_ || !MainLines_ || !AltLines_) return false;

    // Wire up line pointers.
    for (std::uint16_t r = 0; r < rows; ++r) {
        MainLines_[r].Cells = MainCells_ + static_cast<std::size_t>(r) * cols;
        AltLines_[r].Cells = AltCells_ + static_cast<std::size_t>(r) * cols;
    }

    if (scrollbackLimit > 0) {
        const std::size_t histCells =
            static_cast<std::size_t>(scrollbackLimit) * cols;
        if (histCells * sizeof(Cell) > MAX_SCROLLBACK_BYTES) return false;
        HistoryCells_ = new (std::nothrow) Cell[histCells]{};
        HistoryLines_ = new (std::nothrow) Line[scrollbackLimit]{};
        if (!HistoryCells_ || !HistoryLines_) return false;
        for (std::uint32_t r = 0; r < scrollbackLimit; ++r) {
            HistoryLines_[r].Cells =
                HistoryCells_ + static_cast<std::size_t>(r) * cols;
        }
    }

    // Default tab stops every DEFAULT_TAB_INTERVAL columns.
    for (std::uint16_t c = 0; c < cols; c += DEFAULT_TAB_INTERVAL)
        TabStops_[c / 64] |= (std::uint64_t{1} << (c % 64));

    // Default modes.
    Modes_ = ModeFlag(EMode::Awm) | ModeFlag(EMode::Dectcem);

    return true;
}

Grid::~Grid() {
    delete[] MainCells_;
    delete[] AltCells_;
    delete[] MainLines_;
    delete[] AltLines_;
    delete[] HistoryCells_;
    delete[] HistoryLines_;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

Line& Grid::ActiveLine(std::uint16_t row) noexcept {
    return IsAltScreen() ? AltLines_[row] : MainLines_[row];
}

const Line& Grid::ActiveLine(std::uint16_t row) const noexcept {
    return IsAltScreen() ? AltLines_[row] : MainLines_[row];
}

Line& Grid::ActiveBuf(std::uint16_t row) noexcept { return ActiveLine(row); }

const Cell& Grid::CellAt(std::uint16_t row, std::uint16_t col) const noexcept {
    return ActiveLine(row).Cells[col];
}

Cell& Grid::CellAt(std::uint16_t row, std::uint16_t col) noexcept {
    return ActiveLine(row).Cells[col];
}

const Line* Grid::ScrollbackLine(std::uint32_t idx) const noexcept {
    if (!HistoryLines_ || idx >= HistorySize_) return nullptr;
    const std::uint32_t ring = (HistoryStart_ + idx) % ScrollbackLimit_;
    return &HistoryLines_[ring];
}

void Grid::BlankCell(Cell& cell) noexcept {
    cell = Cell{};
    // A blank cell inherits the current pen bg (for ED/EL erase-to-BG).
    cell.BgKind = PenBgKind_;
    cell.Bg[0] = PenBg_[0];
    cell.Bg[1] = PenBg_[1];
    cell.Bg[2] = PenBg_[2];
}

void Grid::PenToCell(Cell& cell) noexcept {
    cell.Attrs = PenAttrs_;
    cell.AttrEx = PenAttrEx_;
    cell.UlIdx = PenUlIdx_;
    cell.FgKind = PenFgKind_;
    cell.BgKind = PenBgKind_;
    cell.Fg[0] = PenFg_[0];
    cell.Fg[1] = PenFg_[1];
    cell.Fg[2] = PenFg_[2];
    cell.Bg[0] = PenBg_[0];
    cell.Bg[1] = PenBg_[1];
    cell.Bg[2] = PenBg_[2];
}

void Grid::ClearLine(Line& line) noexcept {
    for (std::uint16_t c = 0; c < Cols_; ++c) BlankCell(line.Cells[c]);
    line.CellsUsed = 0;
}

void Grid::ClearCells(Cell* cells, std::uint16_t count) noexcept {
    for (std::uint16_t i = 0; i < count; ++i) BlankCell(cells[i]);
}

void Grid::CopyCells(Cell* dst, const Cell* src, std::uint16_t count) noexcept {
    for (std::uint16_t i = 0; i < count; ++i) dst[i] = src[i];
}

void Grid::MoveCells(Cell* dst, const Cell* src, std::uint16_t count) noexcept {
    if (dst < src) {
        for (std::uint16_t i = 0; i < count; ++i) dst[i] = src[i];
    } else if (dst > src) {
        for (std::uint16_t i = count; i > 0; --i) dst[i - 1] = src[i - 1];
    }
}

// Repair wide-char orphans after a region [first, last) on a line is
// cleared or overwritten.  Fixes the cell just before and just after the
// modified range so the width==2 → width==0 pairing invariant holds.
void Grid::RepairWideOrphans(Line& line, std::uint16_t first,
                             std::uint16_t last) noexcept {
    if (first > 0 && line.Cells[first - 1].Width == 2)
        BlankCell(line.Cells[first - 1]);
    if (last < Cols_ && line.Cells[last].Width == 0)
        BlankCell(line.Cells[last]);
}

void Grid::UpdateCellsUsed(Line& line) noexcept {
    std::uint16_t used = 0;
    for (std::uint16_t c = 0; c < Cols_; ++c) {
        if (line.Cells[c].Codepoint != 0 ||
            line.Cells[c].BgKind != EColorKind::Default)
            used = static_cast<std::uint16_t>(c + 1);
    }
    line.CellsUsed = used;
}

void Grid::PushScrollback(std::uint16_t row) noexcept {
    if (!HistoryLines_ || ScrollbackLimit_ == 0) return;
    const std::uint32_t slot =
        (HistoryStart_ + HistorySize_) % ScrollbackLimit_;
    Line& dest = HistoryLines_[slot];
    const Line& src = MainLines_[row];
    CopyCells(dest.Cells, src.Cells, Cols_);
    dest.CellsUsed = src.CellsUsed;

    if (HistorySize_ < ScrollbackLimit_) {
        ++HistorySize_;
    } else {
        HistoryStart_ = (HistoryStart_ + 1) % ScrollbackLimit_;
    }
}

// ---------------------------------------------------------------------------
// Scroll region helpers
// ---------------------------------------------------------------------------

void Grid::ScrollRegionUp(std::uint16_t n, bool push) noexcept {
    if (n == 0) return;
    const std::uint16_t regionHeight =
        static_cast<std::uint16_t>(ScrollBot_ - ScrollTop_ + 1);
    if (n > regionHeight) n = regionHeight;

    if (push && ScrollTop_ == 0 && !IsAltScreen()) {
        for (std::uint16_t i = 0; i < n; ++i) PushScrollback(i);
    }

    // Rotate line pointers instead of copying cell data: O(regionHeight) for
    // the rotate + O(n * cols) to clear vacated rows vs O(regionHeight * cols)
    // before. A single LF (n=1) costs O(cols) total.
    Line* active = IsAltScreen() ? AltLines_ : MainLines_;
    Line* lo = active + ScrollTop_;
    Line* mid = active + ScrollTop_ + n;
    Line* hi = active + ScrollBot_ + 1;
    std::rotate(lo, mid, hi);
    for (std::uint16_t r = static_cast<std::uint16_t>(ScrollBot_ - n + 1);
         r <= ScrollBot_; ++r)
        ClearLine(active[r]);
}

void Grid::ScrollRegionDown(std::uint16_t n) noexcept {
    if (n == 0) return;
    const std::uint16_t regionHeight =
        static_cast<std::uint16_t>(ScrollBot_ - ScrollTop_ + 1);
    if (n > regionHeight) n = regionHeight;

    for (std::uint16_t r = ScrollBot_; r >= ScrollTop_ + n; --r) {
        Line& dst = ActiveLine(r);
        const Line& src = ActiveLine(static_cast<std::uint16_t>(r - n));
        CopyCells(dst.Cells, src.Cells, Cols_);
        dst.CellsUsed = src.CellsUsed;
    }
    for (std::uint16_t r = ScrollTop_; r < ScrollTop_ + n; ++r)
        ClearLine(ActiveLine(r));
}

// ---------------------------------------------------------------------------
// Print / cursor movement
// ---------------------------------------------------------------------------

// DEC Special Graphics mapping (ESC ( 0) for codepoints 0x60–0x7E.
static constexpr char32_t DEC_GFX[31] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1,
    0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,
    0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C,
    0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

void Grid::PutChar(char32_t cp, int width) noexcept {
    // Apply DEC Special Graphics charset when active (SO selects G1=charset 1,
    // SI restores G0).  Mapping covers 0x60–0x7E (30 entries + backtick).
    {
        const std::uint8_t cs = (CharsetActive_ == 0) ? CharsetG0_ : CharsetG1_;
        if (cs == 1 && cp >= 0x60 && cp <= 0x7E) cp = DEC_GFX[cp - 0x60];
    }

    // Zero-width combining mark: GRAPHEME_MAX_CODEPOINTS=1 — keep base, drop
    // mark.
    if (width == 0) return;

    if (PendingWrap_ && ModeSet(EMode::Awm)) {
        ActiveLine(CursorRow_).CellsUsed = Cols_;
        CursorCol_ = 0;
        LineFeed();
    }
    PendingWrap_ = false;

    if (ModeSet(EMode::Irm)) {
        InsertChars(static_cast<std::uint16_t>(width));
    }

    // D4: in a 1-column grid a wide char can never fit regardless of AWM.
    // Replace with a narrow space placeholder (xterm behaviour).
    if (width == 2 && Cols_ == 1) {
        width = 1;
        cp = U' ';
    }

    if (width == 2 && CursorCol_ + 1 >= Cols_) {
        if (ModeSet(EMode::Awm)) {
            ActiveLine(CursorRow_).CellsUsed = Cols_;
            CursorCol_ = 0;
            LineFeed();
        } else {
            return;
        }
    }

    // If writing to a continuation cell (Width==0), break the wide char whose
    // second half we are overwriting so the pairing invariant stays intact.
    if (CursorCol_ > 0) {
        Cell& left =
            CellAt(CursorRow_, static_cast<std::uint16_t>(CursorCol_ - 1));
        if (left.Width == 2) left.Width = 1;
    }

    // D2: if writing over a wide head, blank its continuation to prevent an
    // orphan continuation cell (width==0 with no preceding width==2).
    {
        Cell& cur = CellAt(CursorRow_, CursorCol_);
        if (cur.Width == 2 && CursorCol_ + 1 < Cols_)
            BlankCell(
                CellAt(CursorRow_, static_cast<std::uint16_t>(CursorCol_ + 1)));
    }

    Cell& cell = CellAt(CursorRow_, CursorCol_);
    PenToCell(cell);
    cell.Codepoint = cp;
    cell.Width = static_cast<std::uint8_t>(width);

    if (width == 2 && CursorCol_ + 1 < Cols_) {
        Cell& cont =
            CellAt(CursorRow_, static_cast<std::uint16_t>(CursorCol_ + 1));
        cont = Cell{};
        cont.Width = 0;
    }

    const std::uint16_t used = static_cast<std::uint16_t>(CursorCol_ + width);
    if (used > ActiveLine(CursorRow_).CellsUsed)
        ActiveLine(CursorRow_).CellsUsed = (used <= Cols_ ? used : Cols_);

    LastPrintedChar_ = cp;
    LastPrintedWidth_ = width;

    const std::uint16_t advance = static_cast<std::uint16_t>(width);
    if (CursorCol_ + advance >= Cols_) {
        CursorCol_ = static_cast<std::uint16_t>(Cols_ - 1);
        PendingWrap_ = true;
    } else {
        CursorCol_ = static_cast<std::uint16_t>(CursorCol_ + advance);
    }
}

void Grid::CarriageReturn() noexcept {
    CursorCol_ = 0;
    PendingWrap_ = false;
}

void Grid::LineFeed() noexcept {
    if (ModeSet(EMode::Lnm)) CursorCol_ = 0;
    PendingWrap_ = false;
    if (CursorRow_ == ScrollBot_) {
        ScrollRegionUp(1);
    } else if (CursorRow_ < Rows_ - 1) {
        ++CursorRow_;
    }
}

void Grid::Index() noexcept {
    PendingWrap_ = false;
    if (CursorRow_ == ScrollBot_) {
        ScrollRegionUp(1);
    } else if (CursorRow_ < Rows_ - 1) {
        ++CursorRow_;
    }
}

void Grid::ReverseIndex() noexcept {
    PendingWrap_ = false;
    if (CursorRow_ == ScrollTop_) {
        ScrollRegionDown(1);
    } else if (CursorRow_ > 0) {
        --CursorRow_;
    }
}

void Grid::Backspace() noexcept {
    PendingWrap_ = false;
    if (CursorCol_ > 0) --CursorCol_;
}

void Grid::HorizontalTab() noexcept {
    PendingWrap_ = false;
    for (std::uint16_t c = static_cast<std::uint16_t>(CursorCol_ + 1);
         c < Cols_; ++c) {
        if ((TabStops_[c / 64] >> (c % 64)) & 1) {
            CursorCol_ = c;
            return;
        }
    }
    CursorCol_ = static_cast<std::uint16_t>(Cols_ - 1);
}

void Grid::HorizontalTabBack(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    if (n == 0) n = 1;
    for (std::uint16_t step = 0; step < n; ++step) {
        if (CursorCol_ == 0) break;
        bool found = false;
        for (std::uint16_t c = static_cast<std::uint16_t>(CursorCol_ - 1);;
             --c) {
            if ((TabStops_[c / 64] >> (c % 64)) & 1) {
                CursorCol_ = c;
                found = true;
                break;
            }
            if (c == 0) break;
        }
        if (!found) {
            CursorCol_ = 0;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Relative cursor movement
// ---------------------------------------------------------------------------

void Grid::MoveCursorUp(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    // Determine the limit (topmost row the cursor may reach):
    //   • Inside the scroll region  → stop at ScrollTop_ (never cross the top
    //   margin). • Below the scroll region   → stop at ScrollBot_+1 (don't
    //   enter the region
    //     from below — xterm behaviour for cursor outside the region going up).
    //   • Above the scroll region   → stop at 0 normally, ScrollTop_ under
    //   DECOM.
    std::uint16_t limit;
    if (CursorRow_ >= ScrollTop_ && CursorRow_ <= ScrollBot_) {
        limit = ScrollTop_;
    } else if (CursorRow_ > ScrollBot_) {
        limit = static_cast<std::uint16_t>(ScrollBot_ + 1);
    } else {
        limit = ModeSet(EMode::Decom) ? ScrollTop_ : 0;
    }
    if (n > CursorRow_ - limit)
        n = static_cast<std::uint16_t>(CursorRow_ - limit);
    CursorRow_ = static_cast<std::uint16_t>(CursorRow_ - n);
}

void Grid::MoveCursorDown(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    // When cursor is inside the scroll region, always stop at ScrollBot_
    // regardless of DECOM (CUD must not cross the scroll margin).
    const std::uint16_t limit =
        (CursorRow_ <= ScrollBot_)
            ? ScrollBot_
            : (ModeSet(EMode::Decom) ? ScrollBot_
                                     : static_cast<std::uint16_t>(Rows_ - 1));
    if (CursorRow_ + n > limit)
        n = static_cast<std::uint16_t>(limit - CursorRow_);
    CursorRow_ = static_cast<std::uint16_t>(CursorRow_ + n);
}

void Grid::MoveCursorRight(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    const std::uint16_t max = static_cast<std::uint16_t>(Cols_ - 1);
    CursorCol_ = (CursorCol_ + n <= max)
                     ? static_cast<std::uint16_t>(CursorCol_ + n)
                     : max;
}

void Grid::MoveCursorLeft(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    CursorCol_ =
        (n <= CursorCol_) ? static_cast<std::uint16_t>(CursorCol_ - n) : 0;
}

void Grid::MoveCursorTo(std::uint16_t row1, std::uint16_t col1) noexcept {
    PendingWrap_ = false;
    const std::uint16_t rowOff = ModeSet(EMode::Decom) ? ScrollTop_ : 0;
    const std::uint16_t maxRow = ModeSet(EMode::Decom)
                                     ? ScrollBot_
                                     : static_cast<std::uint16_t>(Rows_ - 1);
    const std::uint16_t row0 =
        (row1 > 0) ? static_cast<std::uint16_t>(row1 - 1) : 0;
    const std::uint16_t col0 =
        (col1 > 0) ? static_cast<std::uint16_t>(col1 - 1) : 0;
    CursorRow_ = ClampRow(static_cast<std::uint16_t>(rowOff + row0));
    if (CursorRow_ > maxRow) CursorRow_ = maxRow;
    CursorCol_ = ClampCol(col0);
}

void Grid::MoveCursorToCol(std::uint16_t col1) noexcept {
    PendingWrap_ = false;
    CursorCol_ = ClampCol(col1 > 0 ? static_cast<std::uint16_t>(col1 - 1) : 0);
}

void Grid::MoveCursorToRow(std::uint16_t row1) noexcept {
    PendingWrap_ = false;
    const std::uint16_t rowOff = ModeSet(EMode::Decom) ? ScrollTop_ : 0;
    const std::uint16_t maxRow = ModeSet(EMode::Decom)
                                     ? ScrollBot_
                                     : static_cast<std::uint16_t>(Rows_ - 1);
    const std::uint16_t row0 =
        row1 > 0 ? static_cast<std::uint16_t>(row1 - 1) : 0;
    CursorRow_ = static_cast<std::uint16_t>(rowOff + row0);
    if (CursorRow_ > maxRow) CursorRow_ = maxRow;
    if (CursorRow_ >= Rows_) CursorRow_ = static_cast<std::uint16_t>(Rows_ - 1);
}

void Grid::CursorNextLine(std::uint16_t n) noexcept {
    MoveCursorDown(n);
    CursorCol_ = 0;
}

void Grid::CursorPrevLine(std::uint16_t n) noexcept {
    MoveCursorUp(n);
    CursorCol_ = 0;
}

// ---------------------------------------------------------------------------
// Save / restore cursor
// ---------------------------------------------------------------------------

void Grid::SaveCursor() noexcept {
    SavedCursor& sc = IsAltScreen() ? SavedCursorAlt_ : SavedCursorMain_;
    sc.Row = CursorRow_;
    sc.Col = CursorCol_;
    sc.Attrs = PenAttrs_;
    sc.AttrEx = PenAttrEx_;
    sc.UlIdx = PenUlIdx_;
    sc.FgKind = PenFgKind_;
    sc.BgKind = PenBgKind_;
    sc.Fg[0] = PenFg_[0];
    sc.Fg[1] = PenFg_[1];
    sc.Fg[2] = PenFg_[2];
    sc.Bg[0] = PenBg_[0];
    sc.Bg[1] = PenBg_[1];
    sc.Bg[2] = PenBg_[2];
}

void Grid::RestoreCursor() noexcept {
    const SavedCursor& sc = IsAltScreen() ? SavedCursorAlt_ : SavedCursorMain_;
    CursorRow_ = ClampRow(sc.Row);
    CursorCol_ = ClampCol(sc.Col);
    // Under DECOM clamp into the scroll region.
    if (ModeSet(EMode::Decom)) {
        if (CursorRow_ < ScrollTop_) CursorRow_ = ScrollTop_;
        if (CursorRow_ > ScrollBot_) CursorRow_ = ScrollBot_;
    }
    PenAttrs_ = sc.Attrs;
    PenAttrEx_ = sc.AttrEx;
    PenUlIdx_ = sc.UlIdx;
    PenFgKind_ = sc.FgKind;
    PenBgKind_ = sc.BgKind;
    PenFg_[0] = sc.Fg[0];
    PenFg_[1] = sc.Fg[1];
    PenFg_[2] = sc.Fg[2];
    PenBg_[0] = sc.Bg[0];
    PenBg_[1] = sc.Bg[1];
    PenBg_[2] = sc.Bg[2];
    PendingWrap_ = false;
}

// ---------------------------------------------------------------------------
// Erase operations
// ---------------------------------------------------------------------------

void Grid::EraseInDisplay(std::uint8_t param) noexcept {
    PendingWrap_ = false;
    switch (param) {
        case 0:
            EraseInLine(0);
            for (std::uint16_t r = static_cast<std::uint16_t>(CursorRow_ + 1);
                 r < Rows_; ++r)
                ClearLine(ActiveLine(r));
            break;
        case 1:
            for (std::uint16_t r = 0; r < CursorRow_; ++r)
                ClearLine(ActiveLine(r));
            EraseInLine(1);
            break;
        case 2:
        case 3:
            for (std::uint16_t r = 0; r < Rows_; ++r) ClearLine(ActiveLine(r));
            if (param == 3 && !IsAltScreen()) {
                HistoryStart_ = 0;
                HistorySize_ = 0;
            }
            break;
        default:
            break;
    }
}

void Grid::EraseInLine(std::uint8_t param) noexcept {
    PendingWrap_ = false;
    Line& line = ActiveLine(CursorRow_);
    switch (param) {
        case 0:
            RepairWideOrphans(line, CursorCol_, CursorCol_);
            for (std::uint16_t c = CursorCol_; c < Cols_; ++c)
                BlankCell(line.Cells[c]);
            UpdateCellsUsed(line);
            break;
        case 1: {
            const std::uint16_t last =
                static_cast<std::uint16_t>(CursorCol_ + 1);
            RepairWideOrphans(line, 0, last);
            for (std::uint16_t c = 0; c < last && c < Cols_; ++c)
                BlankCell(line.Cells[c]);
            UpdateCellsUsed(line);
            break;
        }
        case 2:
            ClearLine(line);
            break;
        default:
            break;
    }
}

void Grid::EraseChars(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    if (n == 0) n = 1;
    Line& line = ActiveLine(CursorRow_);
    const std::uint16_t end = (CursorCol_ + n < Cols_)
                                  ? static_cast<std::uint16_t>(CursorCol_ + n)
                                  : Cols_;
    RepairWideOrphans(line, CursorCol_, end);
    for (std::uint16_t c = CursorCol_; c < end; ++c) BlankCell(line.Cells[c]);
    UpdateCellsUsed(line);
}

void Grid::DeleteChars(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    if (n == 0) n = 1;
    Line& line = ActiveLine(CursorRow_);
    const std::uint16_t avail = static_cast<std::uint16_t>(Cols_ - CursorCol_);
    if (n > avail) n = avail;
    // If cursor is on a continuation cell, its lead (at CursorCol_-1) will
    // become an orphan after the shift — blank it before moving cells.
    if (CursorCol_ > 0 && line.Cells[CursorCol_].Width == 0) {
        BlankCell(line.Cells[CursorCol_ - 1]);
        BlankCell(line.Cells[CursorCol_]);
    }
    MoveCells(line.Cells + CursorCol_, line.Cells + CursorCol_ + n,
              static_cast<std::uint16_t>(avail - n));
    ClearCells(line.Cells + Cols_ - n, n);
    // The shift may have left a continuation-cell orphan at CursorCol (its
    // lead was in the deleted range) or a wide-lead orphan at Cols_-n-1 (its
    // continuation was in the cleared tail).
    if (line.Cells[CursorCol_].Width == 0) BlankCell(line.Cells[CursorCol_]);
    const std::uint16_t tailStart = static_cast<std::uint16_t>(Cols_ - n);
    if (tailStart > 0 && line.Cells[tailStart - 1].Width == 2)
        BlankCell(line.Cells[tailStart - 1]);
    UpdateCellsUsed(line);
}

void Grid::InsertChars(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    if (n == 0) n = 1;
    Line& line = ActiveLine(CursorRow_);
    const std::uint16_t avail = static_cast<std::uint16_t>(Cols_ - CursorCol_);
    if (n > avail) n = avail;
    MoveCells(line.Cells + CursorCol_ + n, line.Cells + CursorCol_,
              static_cast<std::uint16_t>(avail - n));
    ClearCells(line.Cells + CursorCol_, n);
    // The wide char whose lead was just before CursorCol may now have no
    // continuation; fix it.  The moved block may start with a continuation
    // whose lead was in the cleared range; fix that too.
    if (CursorCol_ > 0 && line.Cells[CursorCol_ - 1].Width == 2)
        BlankCell(line.Cells[CursorCol_ - 1]);
    const std::uint16_t movedStart = static_cast<std::uint16_t>(CursorCol_ + n);
    if (movedStart < Cols_ && line.Cells[movedStart].Width == 0)
        BlankCell(line.Cells[movedStart]);
    // D3: the shift may have pushed a wide head into the last column where
    // its continuation would fall outside the grid — blank it.
    if (line.Cells[Cols_ - 1].Width == 2) BlankCell(line.Cells[Cols_ - 1]);
    UpdateCellsUsed(line);
}

void Grid::InsertLines(std::uint16_t n) noexcept {
    // IL is only valid when the cursor is within the scroll region,
    // regardless of DECOM mode.
    if (CursorRow_ < ScrollTop_ || CursorRow_ > ScrollBot_) return;
    PendingWrap_ = false;
    const std::uint16_t savedTop = ScrollTop_;
    ScrollTop_ = CursorRow_;
    ScrollRegionDown(n);
    ScrollTop_ = savedTop;
    CursorCol_ = 0;
}

void Grid::DeleteLines(std::uint16_t n) noexcept {
    // DL is only valid when the cursor is within the scroll region,
    // regardless of DECOM mode.
    if (CursorRow_ < ScrollTop_ || CursorRow_ > ScrollBot_) return;
    PendingWrap_ = false;
    const std::uint16_t savedTop = ScrollTop_;
    ScrollTop_ = CursorRow_;
    // DL must never push lines to scrollback (only LineFeed/Index do that).
    ScrollRegionUp(n, /*push=*/false);
    ScrollTop_ = savedTop;
    CursorCol_ = 0;
}

void Grid::ScrollDown(std::uint16_t n) noexcept { ScrollRegionDown(n); }

void Grid::ScrollUp(std::uint16_t n) noexcept { ScrollRegionUp(n); }

void Grid::RepeatChar(std::uint16_t n) noexcept {
    if (LastPrintedChar_ == 0 || n == 0) return;
    for (std::uint16_t i = 0; i < n; ++i)
        PutChar(LastPrintedChar_, LastPrintedWidth_);
}

// ---------------------------------------------------------------------------
// Scroll region
// ---------------------------------------------------------------------------

void Grid::SetScrollRegion(std::uint16_t top1, std::uint16_t bot1) noexcept {
    std::uint16_t t = (top1 > 0) ? static_cast<std::uint16_t>(top1 - 1) : 0;
    std::uint16_t b = (bot1 > 0) ? static_cast<std::uint16_t>(bot1 - 1)
                                 : static_cast<std::uint16_t>(Rows_ - 1);
    if (t >= Rows_) t = 0;
    if (b >= Rows_) b = static_cast<std::uint16_t>(Rows_ - 1);
    if (t >= b) return;
    ScrollTop_ = t;
    ScrollBot_ = b;
    CursorRow_ = ModeSet(EMode::Decom) ? ScrollTop_ : 0;
    CursorCol_ = 0;
    PendingWrap_ = false;
}

// ---------------------------------------------------------------------------
// Mode set/reset
// ---------------------------------------------------------------------------

void Grid::SetMode(std::uint16_t param, bool value) noexcept {
    auto apply = [&](EMode m) noexcept {
        if (value)
            Modes_ |= ModeFlag(m);
        else
            Modes_ &= ~ModeFlag(m);
    };
    switch (param) {
        case 1:
            apply(EMode::Decckm);
            break;
        case 4:
            apply(EMode::Irm);
            break;
        case 6:
            apply(EMode::Decom);
            CursorRow_ = value ? ScrollTop_ : 0;
            CursorCol_ = 0;
            PendingWrap_ = false;
            break;
        case 7:
            apply(EMode::Awm);
            break;
        case 20:
            apply(EMode::Lnm);
            break;
        case 25:
            apply(EMode::Dectcem);
            break;
        case 47:
            SwitchAltScreen(value);
            break;
        case 1000:
            apply(EMode::Mouse1000);
            break;
        case 1002:
            apply(EMode::Mouse1002);
            break;
        case 1003:
            apply(EMode::Mouse1003);
            break;
        case 1004:
            apply(EMode::FocusEvents);
            break;
        case 1005:
            apply(EMode::Mouse1005);
            break;
        case 1006:
            apply(EMode::Mouse1006);
            break;
        case 1015:
            apply(EMode::Mouse1015);
            break;
        case 1049:
            SwitchAltScreen(value);
            break;
        case 2004:
            apply(EMode::BracketedPaste);
            break;
        case 2026:
            apply(EMode::SyncOutput);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// SGR
// ---------------------------------------------------------------------------

void Grid::ApplySgr(const std::uint16_t* params, const bool* colon,
                    std::size_t count) noexcept {
    auto resetPen = [&]() noexcept {
        PenAttrs_ = 0;
        PenAttrEx_ = 0;
        PenUlIdx_ = 0;
        PenFgKind_ = EColorKind::Default;
        PenBgKind_ = EColorKind::Default;
        PenFg_[0] = PenFg_[1] = PenFg_[2] = 0;
        PenBg_[0] = PenBg_[1] = PenBg_[2] = 0;
    };

    if (count == 0) {
        resetPen();
        return;
    }

    for (std::size_t i = 0; i < count;) {
        const std::uint16_t p = params[i];
        switch (p) {
            case 0:
                resetPen();
                break;
            case 1:
                PenAttrs_ |= Bold;
                break;
            case 2:
                PenAttrs_ |= Dim;
                break;
            case 3:
                PenAttrs_ |= Italic;
                break;
            case 4:
                // 4:n = underline style (colon-separated sub-param).
                // Plain 4 or 4;n (semicolon) = single underline.
                if (i + 1 < count && colon[i + 1] && params[i + 1] <= 5) {
                    const std::uint8_t style =
                        static_cast<std::uint8_t>(params[i + 1]);
                    if (style == 0) {
                        PenAttrs_ &= static_cast<std::uint8_t>(~Underline);
                        PenAttrEx_ = 0;
                    } else {
                        PenAttrs_ |= Underline;
                        PenAttrEx_ = style;
                    }
                    ++i;
                } else {
                    PenAttrs_ |= Underline;
                    PenAttrEx_ = 1;
                }
                break;
            case 5:
                PenAttrs_ |= Blink;
                break;
            case 7:
                PenAttrs_ |= Inverse;
                break;
            case 8:
                PenAttrs_ |= Invisible;
                break;
            case 9:
                PenAttrs_ |= StrikeThrough;
                break;
            case 22:
                PenAttrs_ &= static_cast<std::uint8_t>(~(Bold | Dim));
                break;
            case 23:
                PenAttrs_ &= static_cast<std::uint8_t>(~Italic);
                break;
            case 24:
                PenAttrs_ &= static_cast<std::uint8_t>(~Underline);
                break;
            case 25:
                PenAttrs_ &= static_cast<std::uint8_t>(~Blink);
                break;
            case 27:
                PenAttrs_ &= static_cast<std::uint8_t>(~Inverse);
                break;
            case 28:
                PenAttrs_ &= static_cast<std::uint8_t>(~Invisible);
                break;
            case 29:
                PenAttrs_ &= static_cast<std::uint8_t>(~StrikeThrough);
                break;

            case 30:
            case 31:
            case 32:
            case 33:
            case 34:
            case 35:
            case 36:
            case 37:
                PenFgKind_ = EColorKind::Indexed;
                PenFg_[0] = static_cast<std::uint8_t>(p - 30);
                PenFg_[1] = PenFg_[2] = 0;
                break;
            case 38: {
                // Colon-style: 38:5:n or 38:2::r:g:b / 38:2:r:g:b
                // Semi-style: 38;5;n or 38;2;r;g;b
                if (i + 1 < count && colon[i + 1]) {
                    // Colon sub-params: skip all colon-linked entries.
                    std::size_t sub = i + 1;
                    while (sub + 1 < count && colon[sub + 1]) ++sub;
                    const std::size_t nsub = sub - i;
                    if (nsub >= 2 && params[i + 1] == 5) {
                        PenFgKind_ = EColorKind::Indexed;
                        PenFg_[0] = static_cast<std::uint8_t>(params[i + 2]);
                        PenFg_[1] = PenFg_[2] = 0;
                    } else if (nsub >= 5 && params[i + 1] == 2) {
                        // 38:2::r:g:b (5 sub-params: type, colorspace, r, g, b)
                        PenFgKind_ = EColorKind::TrueColor;
                        PenFg_[0] =
                            static_cast<std::uint8_t>(params[i + 3] & 0xFF);
                        PenFg_[1] =
                            static_cast<std::uint8_t>(params[i + 4] & 0xFF);
                        PenFg_[2] =
                            static_cast<std::uint8_t>(params[i + 5] & 0xFF);
                    } else if (nsub >= 4 && params[i + 1] == 2) {
                        // 38:2:r:g:b (4 sub-params: type, r, g, b)
                        PenFgKind_ = EColorKind::TrueColor;
                        PenFg_[0] =
                            static_cast<std::uint8_t>(params[i + 2] & 0xFF);
                        PenFg_[1] =
                            static_cast<std::uint8_t>(params[i + 3] & 0xFF);
                        PenFg_[2] =
                            static_cast<std::uint8_t>(params[i + 4] & 0xFF);
                    }
                    i = sub;
                } else if (i + 2 < count && params[i + 1] == 5) {
                    PenFgKind_ = EColorKind::Indexed;
                    PenFg_[0] = static_cast<std::uint8_t>(params[i + 2]);
                    PenFg_[1] = PenFg_[2] = 0;
                    i += 2;
                } else if (i + 4 < count && params[i + 1] == 2) {
                    PenFgKind_ = EColorKind::TrueColor;
                    PenFg_[0] = static_cast<std::uint8_t>(params[i + 2] & 0xFF);
                    PenFg_[1] = static_cast<std::uint8_t>(params[i + 3] & 0xFF);
                    PenFg_[2] = static_cast<std::uint8_t>(params[i + 4] & 0xFF);
                    i += 4;
                }
                break;
            }
            case 39:
                PenFgKind_ = EColorKind::Default;
                PenFg_[0] = PenFg_[1] = PenFg_[2] = 0;
                break;

            case 40:
            case 41:
            case 42:
            case 43:
            case 44:
            case 45:
            case 46:
            case 47:
                PenBgKind_ = EColorKind::Indexed;
                PenBg_[0] = static_cast<std::uint8_t>(p - 40);
                PenBg_[1] = PenBg_[2] = 0;
                break;
            case 48: {
                if (i + 1 < count && colon[i + 1]) {
                    std::size_t sub = i + 1;
                    while (sub + 1 < count && colon[sub + 1]) ++sub;
                    const std::size_t nsub = sub - i;
                    if (nsub >= 2 && params[i + 1] == 5) {
                        PenBgKind_ = EColorKind::Indexed;
                        PenBg_[0] = static_cast<std::uint8_t>(params[i + 2]);
                        PenBg_[1] = PenBg_[2] = 0;
                    } else if (nsub >= 5 && params[i + 1] == 2) {
                        // 48:2::r:g:b (5 sub-params: type, colorspace, r, g, b)
                        PenBgKind_ = EColorKind::TrueColor;
                        PenBg_[0] =
                            static_cast<std::uint8_t>(params[i + 3] & 0xFF);
                        PenBg_[1] =
                            static_cast<std::uint8_t>(params[i + 4] & 0xFF);
                        PenBg_[2] =
                            static_cast<std::uint8_t>(params[i + 5] & 0xFF);
                    } else if (nsub >= 4 && params[i + 1] == 2) {
                        // 48:2:r:g:b (4 sub-params: type, r, g, b)
                        PenBgKind_ = EColorKind::TrueColor;
                        PenBg_[0] =
                            static_cast<std::uint8_t>(params[i + 2] & 0xFF);
                        PenBg_[1] =
                            static_cast<std::uint8_t>(params[i + 3] & 0xFF);
                        PenBg_[2] =
                            static_cast<std::uint8_t>(params[i + 4] & 0xFF);
                    }
                    i = sub;
                } else if (i + 2 < count && params[i + 1] == 5) {
                    PenBgKind_ = EColorKind::Indexed;
                    PenBg_[0] = static_cast<std::uint8_t>(params[i + 2]);
                    PenBg_[1] = PenBg_[2] = 0;
                    i += 2;
                } else if (i + 4 < count && params[i + 1] == 2) {
                    PenBgKind_ = EColorKind::TrueColor;
                    PenBg_[0] = static_cast<std::uint8_t>(params[i + 2] & 0xFF);
                    PenBg_[1] = static_cast<std::uint8_t>(params[i + 3] & 0xFF);
                    PenBg_[2] = static_cast<std::uint8_t>(params[i + 4] & 0xFF);
                    i += 4;
                }
                break;
            }
            case 49:
                PenBgKind_ = EColorKind::Default;
                PenBg_[0] = PenBg_[1] = PenBg_[2] = 0;
                break;

            // SGR 58/59 – underline colour (consume sub-params, store index
            // when indexed colour is given, silently drop truecolour since
            // UlIdx is only 1 byte reserved for future use).
            case 58: {
                if (i + 1 < count && colon[i + 1]) {
                    std::size_t sub = i + 1;
                    while (sub + 1 < count && colon[sub + 1]) ++sub;
                    const std::size_t nsub = sub - i;
                    if (nsub >= 2 && params[i + 1] == 5) {
                        PenUlIdx_ = static_cast<std::uint8_t>(params[i + 2]);
                    }
                    i = sub;
                } else if (i + 2 < count && params[i + 1] == 5) {
                    PenUlIdx_ = static_cast<std::uint8_t>(params[i + 2]);
                    i += 2;
                } else if (i + 4 < count && params[i + 1] == 2) {
                    // Truecolour underline: consume but silently drop.
                    i += 4;
                }
                break;
            }
            case 59:
                PenUlIdx_ = 0;
                break;

            case 90:
            case 91:
            case 92:
            case 93:
            case 94:
            case 95:
            case 96:
            case 97:
                PenFgKind_ = EColorKind::Indexed;
                PenFg_[0] = static_cast<std::uint8_t>(p - 90 + 8);
                PenFg_[1] = PenFg_[2] = 0;
                break;
            case 100:
            case 101:
            case 102:
            case 103:
            case 104:
            case 105:
            case 106:
            case 107:
                PenBgKind_ = EColorKind::Indexed;
                PenBg_[0] = static_cast<std::uint8_t>(p - 100 + 8);
                PenBg_[1] = PenBg_[2] = 0;
                break;

            default:
                break;
        }
        ++i;
    }
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void Grid::Reset(bool hard) noexcept {
    // Always reset pen and scroll region.
    PenAttrs_ = 0;
    PenAttrEx_ = 0;
    PenUlIdx_ = 0;
    PenFgKind_ = EColorKind::Default;
    PenBgKind_ = EColorKind::Default;
    PenFg_[0] = PenFg_[1] = PenFg_[2] = 0;
    PenBg_[0] = PenBg_[1] = PenBg_[2] = 0;
    ScrollTop_ = 0;
    ScrollBot_ = static_cast<std::uint16_t>(Rows_ - 1);
    CharsetActive_ = 0;
    CharsetG0_ = 0;
    CharsetG1_ = 0;

    if (hard) {
        // RIS: full reset — clear screen, scrollback, cursor, saved cursors.
        CursorRow_ = 0;
        CursorCol_ = 0;
        PendingWrap_ = false;
        Modes_ = ModeFlag(EMode::Awm) | ModeFlag(EMode::Dectcem);
        for (std::uint16_t r = 0; r < Rows_; ++r) {
            ClearLine(MainLines_[r]);
            ClearLine(AltLines_[r]);
        }
        HistoryStart_ = 0;
        HistorySize_ = 0;
        memset(TabStops_, 0, sizeof(TabStops_));
        for (std::uint16_t c = 0; c < Cols_; c += DEFAULT_TAB_INTERVAL)
            TabStops_[c / 64] |= (std::uint64_t{1} << (c % 64));
        SavedCursorMain_ = {};
        SavedCursorAlt_ = {};
        CursorStyle_ = 0;
    } else {
        // DECSTR (soft reset): keep cursor position, alt-screen flag, and
        // tab stops.  Only reset DECOM, IRM, LNM, bracketed-paste, and
        // the other modes that DECSTR is specified to clear.
        const bool wasAlt = ModeSet(EMode::AltScreen);
        Modes_ = ModeFlag(EMode::Awm) | ModeFlag(EMode::Dectcem);
        if (wasAlt) Modes_ |= ModeFlag(EMode::AltScreen);
        // Cursor position and PendingWrap are preserved by DECSTR.
    }
}

// ---------------------------------------------------------------------------
// Miscellaneous
// ---------------------------------------------------------------------------

void Grid::SetCursorStyle(std::uint8_t style) noexcept { CursorStyle_ = style; }

void Grid::SetTabStop() noexcept {
    if (CursorCol_ < Cols_)
        TabStops_[CursorCol_ / 64] |= (std::uint64_t{1} << (CursorCol_ % 64));
}

void Grid::ClearTabStop(std::uint8_t param) noexcept {
    if (param == 0) {
        if (CursorCol_ < Cols_)
            TabStops_[CursorCol_ / 64] &=
                ~(std::uint64_t{1} << (CursorCol_ % 64));
    } else if (param == 3) {
        memset(TabStops_, 0, sizeof(TabStops_));
    }
}

void Grid::ShiftCharset(bool out) noexcept { CharsetActive_ = out ? 1 : 0; }

void Grid::DesignateCharset(std::uint8_t g, std::uint8_t designator) noexcept {
    const std::uint8_t mapped = (designator == '0') ? 1 : 0;
    if (g == 0)
        CharsetG0_ = mapped;
    else
        CharsetG1_ = mapped;
}

void Grid::SwitchAltScreen(bool enter) noexcept {
    const bool already = IsAltScreen();
    if (enter == already) return;

    if (enter) {
        SaveCursor();
        Modes_ |= ModeFlag(EMode::AltScreen);
        for (std::uint16_t r = 0; r < Rows_; ++r) ClearLine(AltLines_[r]);
        CursorRow_ = 0;
        CursorCol_ = 0;
        PendingWrap_ = false;
    } else {
        Modes_ &= ~ModeFlag(EMode::AltScreen);
        RestoreCursor();
    }
}

GridResult<void> Grid::Resize(std::uint16_t newCols,
                              std::uint16_t newRows) noexcept {
    if (newCols == 0) newCols = 1;
    if (newRows == 0) newRows = 1;
    if (newCols > MAX_COLS) newCols = MAX_COLS;
    if (newRows > MAX_ROWS) newRows = MAX_ROWS;

    if (newCols == Cols_ && newRows == Rows_) return {};

    const std::uint16_t oldRows = Rows_;
    const std::uint16_t clampCols = newCols < Cols_ ? newCols : Cols_;

    // -----------------------------------------------------------------------
    // Phase 1: allocate all new buffers.  Do NOT mutate any grid state yet.
    // -----------------------------------------------------------------------

    const std::size_t newScreen = static_cast<std::size_t>(newCols) * newRows;
    Cell* newMain = new (std::nothrow) Cell[newScreen]{};
    Cell* newAlt = new (std::nothrow) Cell[newScreen]{};
    Line* newMainLines = new (std::nothrow) Line[newRows]{};
    Line* newAltLines = new (std::nothrow) Line[newRows]{};

    if (!newMain || !newAlt || !newMainLines || !newAltLines) {
        delete[] newMain;
        delete[] newAlt;
        delete[] newMainLines;
        delete[] newAltLines;
        return std::unexpected(EGridError::AllocationFailed);
    }

    // History reallocation (only needed when column count changes).
    Cell* newHistCellsArena = nullptr;
    Line* newHistLines = nullptr;
    if (newCols != Cols_ && HistoryLines_) {
        const std::size_t newHistCells =
            static_cast<std::size_t>(ScrollbackLimit_) * newCols;
        newHistCellsArena =
            (newHistCells * sizeof(Cell) <= MAX_SCROLLBACK_BYTES)
                ? new (std::nothrow) Cell[newHistCells]{}
                : nullptr;
        newHistLines = new (std::nothrow) Line[ScrollbackLimit_]{};
        if (!newHistCellsArena || !newHistLines) {
            delete[] newMain;
            delete[] newAlt;
            delete[] newMainLines;
            delete[] newAltLines;
            delete[] newHistCellsArena;
            delete[] newHistLines;
            return std::unexpected(EGridError::AllocationFailed);
        }
    }

    // -----------------------------------------------------------------------
    // Phase 2: all allocations succeeded — now mutate grid state.
    // -----------------------------------------------------------------------

    // Truncate CellsUsed on the old lines before copying them (shrink case).
    auto truncateCellsUsed = [&](Line* lines, std::uint16_t rows) noexcept {
        for (std::uint16_t r = 0; r < rows; ++r)
            if (lines[r].CellsUsed > clampCols) lines[r].CellsUsed = clampCols;
    };
    truncateCellsUsed(MainLines_, oldRows);
    truncateCellsUsed(AltLines_, oldRows);

    // D1 (pre-copy): blank any wide-head cell at the new last column on the
    // OLD buffers.  CopyCells will carry the blanked cells into the new buffer,
    // so no suppression of the static analyser is needed — the old cells are
    // already initialised.  Scrollback uses the circular-buffer index (src)
    // rather than the sequential index used after the swap.
    if (newCols < Cols_) {
        for (std::uint16_t r = 0; r < oldRows; ++r) {
            if (MainLines_[r].Cells[newCols - 1].Width == 2)
                BlankCell(MainLines_[r].Cells[newCols - 1]);
            if (AltLines_[r].Cells[newCols - 1].Width == 2)
                BlankCell(AltLines_[r].Cells[newCols - 1]);
        }
        if (HistoryLines_) {
            for (std::uint32_t i = 0; i < HistorySize_; ++i) {
                const std::uint32_t src =
                    (HistoryStart_ + i) % ScrollbackLimit_;
                Cell& hcell = HistoryLines_[src].Cells[newCols - 1];
                if (hcell.Width == 2) BlankCell(hcell);
            }
        }
    }

    // When shrinking rows, push the rows that fall off the top of the
    // main screen into scrollback before swapping the line arrays.
    if (newRows < oldRows && !IsAltScreen()) {
        const std::uint16_t lost =
            static_cast<std::uint16_t>(oldRows - newRows);
        for (std::uint16_t r = 0; r < lost; ++r) PushScrollback(r);
    }

    // Wire line-to-cell pointers and copy content from old buffers.
    for (std::uint16_t r = 0; r < newRows; ++r) {
        newMainLines[r].Cells = newMain + static_cast<std::size_t>(r) * newCols;
        newAltLines[r].Cells = newAlt + static_cast<std::size_t>(r) * newCols;
    }
    const std::uint16_t copyRows = newRows < oldRows ? newRows : oldRows;
    const std::uint16_t srcStart =
        newRows < oldRows ? static_cast<std::uint16_t>(oldRows - newRows) : 0;
    for (std::uint16_t r = 0; r < copyRows; ++r) {
        const std::uint16_t src = static_cast<std::uint16_t>(srcStart + r);
        CopyCells(newMainLines[r].Cells, MainLines_[src].Cells, clampCols);
        CopyCells(newAltLines[r].Cells, AltLines_[src].Cells, clampCols);
        newMainLines[r].CellsUsed = MainLines_[src].CellsUsed < newCols
                                        ? MainLines_[src].CellsUsed
                                        : newCols;
        newAltLines[r].CellsUsed = AltLines_[src].CellsUsed < newCols
                                       ? AltLines_[src].CellsUsed
                                       : newCols;
    }

    // Swap screen buffers.
    delete[] MainCells_;
    delete[] AltCells_;
    delete[] MainLines_;
    delete[] AltLines_;
    MainCells_ = newMain;
    AltCells_ = newAlt;
    MainLines_ = newMainLines;
    AltLines_ = newAltLines;

    // Swap history buffers (if reallocated).
    if (newHistCellsArena) {
        // Wire all Cells pointers before the copy loop (clang-analyzer).
        for (std::uint32_t r = 0; r < ScrollbackLimit_; ++r) {
            newHistLines[r].Cells =
                newHistCellsArena + static_cast<std::size_t>(r) * newCols;
        }
        // HistorySize_ <= ScrollbackLimit_ is an invariant so i is always
        // in the wired range.
        for (std::uint32_t i = 0; i < HistorySize_ && i < ScrollbackLimit_;
             ++i) {
            const std::uint32_t src = (HistoryStart_ + i) % ScrollbackLimit_;
            Cell* const dstCells =
                newHistCellsArena + static_cast<std::size_t>(i) * newCols;
            newHistLines[i].Cells = dstCells;
            CopyCells(dstCells, HistoryLines_[src].Cells, clampCols);
            newHistLines[i].CellsUsed = HistoryLines_[src].CellsUsed < newCols
                                            ? HistoryLines_[src].CellsUsed
                                            : newCols;
        }
        delete[] HistoryCells_;
        delete[] HistoryLines_;
        HistoryCells_ = newHistCellsArena;
        HistoryLines_ = newHistLines;
        HistoryStart_ = 0;
    }

    // Add new tab stops for any columns that didn't exist before.
    if (newCols > Cols_) {
        // Start at the first multiple of DEFAULT_TAB_INTERVAL at or above Cols_
        // to avoid duplicating the last existing stop.
        const std::uint16_t firstStop = static_cast<std::uint16_t>(
            ((Cols_ + DEFAULT_TAB_INTERVAL - 1) / DEFAULT_TAB_INTERVAL) *
            DEFAULT_TAB_INTERVAL);
        for (std::uint16_t c = firstStop; c < newCols;
             c = static_cast<std::uint16_t>(c + DEFAULT_TAB_INTERVAL))
            TabStops_[c / 64] |= (std::uint64_t{1} << (c % 64));
    }

    // Update dimensions and reset scroll region.
    Cols_ = newCols;
    Rows_ = newRows;
    ScrollTop_ = 0;
    ScrollBot_ = static_cast<std::uint16_t>(newRows - 1);

    // Clamp cursor and saved cursors to new bounds.
    if (newRows < oldRows) {
        const std::uint16_t lost =
            static_cast<std::uint16_t>(oldRows - newRows);
        CursorRow_ = (CursorRow_ >= lost)
                         ? static_cast<std::uint16_t>(CursorRow_ - lost)
                         : 0;
    } else if (CursorRow_ >= newRows) {
        CursorRow_ = static_cast<std::uint16_t>(newRows - 1);
    }
    if (CursorCol_ >= newCols)
        CursorCol_ = static_cast<std::uint16_t>(newCols - 1);
    PendingWrap_ = false;

    auto clampSC = [&](SavedCursor& sc) noexcept {
        if (sc.Row >= newRows) sc.Row = static_cast<std::uint16_t>(newRows - 1);
        if (sc.Col >= newCols) sc.Col = static_cast<std::uint16_t>(newCols - 1);
    };
    clampSC(SavedCursorMain_);
    clampSC(SavedCursorAlt_);
    return {};
}

}  // namespace zet::grid
