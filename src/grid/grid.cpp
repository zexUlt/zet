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

    for (std::uint16_t r = ScrollTop_; r + n <= ScrollBot_; ++r) {
        Line& dst = ActiveLine(r);
        const Line& src = ActiveLine(static_cast<std::uint16_t>(r + n));
        CopyCells(dst.Cells, src.Cells, Cols_);
        dst.CellsUsed = src.CellsUsed;
    }
    for (std::uint16_t r = static_cast<std::uint16_t>(ScrollBot_ - n + 1);
         r <= ScrollBot_; ++r)
        ClearLine(ActiveLine(r));
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

void Grid::PutChar(char32_t cp, int width) noexcept {
    // Zero-width combining mark: per design.md §9 GRAPHEME_MAX_CODEPOINTS = 1.
    // Overwrite the previous cell's codepoint; discard if there is none.
    if (width == 0) {
        if (CursorCol_ > 0) {
            Cell& prev =
                CellAt(CursorRow_, static_cast<std::uint16_t>(CursorCol_ - 1));
            if (prev.Codepoint != 0) prev.Codepoint = cp;
        } else if (PendingWrap_ && CursorRow_ > 0) {
            Cell& prev = CellAt(static_cast<std::uint16_t>(CursorRow_ - 1),
                                static_cast<std::uint16_t>(Cols_ - 1));
            if (prev.Codepoint != 0) prev.Codepoint = cp;
        }
        return;
    }

    if (PendingWrap_ && ModeSet(EMode::Awm)) {
        ActiveLine(CursorRow_).CellsUsed = Cols_;
        CursorCol_ = 0;
        LineFeed();
    }
    PendingWrap_ = false;

    if (ModeSet(EMode::Irm)) {
        InsertChars(static_cast<std::uint16_t>(width));
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

// ---------------------------------------------------------------------------
// Relative cursor movement
// ---------------------------------------------------------------------------

void Grid::MoveCursorUp(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    const std::uint16_t limit = ModeSet(EMode::Decom) ? ScrollTop_ : 0;
    if (n > CursorRow_ - limit)
        n = static_cast<std::uint16_t>(CursorRow_ - limit);
    CursorRow_ = static_cast<std::uint16_t>(CursorRow_ - n);
}

void Grid::MoveCursorDown(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    const std::uint16_t limit = ModeSet(EMode::Decom)
                                    ? ScrollBot_
                                    : static_cast<std::uint16_t>(Rows_ - 1);
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
    CursorRow_ = ClampRow(row1 > 0 ? static_cast<std::uint16_t>(row1 - 1) : 0);
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
            for (std::uint16_t c = CursorCol_; c < Cols_; ++c)
                BlankCell(line.Cells[c]);
            UpdateCellsUsed(line);
            break;
        case 1:
            for (std::uint16_t c = 0; c <= CursorCol_ && c < Cols_; ++c)
                BlankCell(line.Cells[c]);
            UpdateCellsUsed(line);
            break;
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
    for (std::uint16_t c = CursorCol_; c < end; ++c) BlankCell(line.Cells[c]);
    UpdateCellsUsed(line);
}

void Grid::DeleteChars(std::uint16_t n) noexcept {
    PendingWrap_ = false;
    if (n == 0) n = 1;
    Line& line = ActiveLine(CursorRow_);
    const std::uint16_t avail = static_cast<std::uint16_t>(Cols_ - CursorCol_);
    if (n > avail) n = avail;
    MoveCells(line.Cells + CursorCol_, line.Cells + CursorCol_ + n,
              static_cast<std::uint16_t>(avail - n));
    ClearCells(line.Cells + Cols_ - n, n);
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

void Grid::ApplySgr(const std::uint16_t* params, std::size_t count) noexcept {
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
                PenAttrs_ |= Underline;
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
                if (i + 2 < count && params[i + 1] == 5) {
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
                if (i + 2 < count && params[i + 1] == 5) {
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
    PenAttrs_ = 0;
    PenAttrEx_ = 0;
    PenUlIdx_ = 0;
    PenFgKind_ = EColorKind::Default;
    PenBgKind_ = EColorKind::Default;
    PenFg_[0] = PenFg_[1] = PenFg_[2] = 0;
    PenBg_[0] = PenBg_[1] = PenBg_[2] = 0;
    ScrollTop_ = 0;
    ScrollBot_ = static_cast<std::uint16_t>(Rows_ - 1);
    CursorRow_ = 0;
    CursorCol_ = 0;
    PendingWrap_ = false;
    CharsetActive_ = 0;
    CharsetG0_ = 0;
    CharsetG1_ = 0;
    Modes_ = ModeFlag(EMode::Awm) | ModeFlag(EMode::Dectcem);

    if (hard) {
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

void Grid::Resize(std::uint16_t newCols, std::uint16_t newRows) noexcept {
    if (newCols == 0) newCols = 1;
    if (newRows == 0) newRows = 1;
    if (newCols > MAX_COLS) newCols = MAX_COLS;
    if (newRows > MAX_ROWS) newRows = MAX_ROWS;

    if (newCols == Cols_ && newRows == Rows_) return;

    if (CursorRow_ >= newRows)
        CursorRow_ = static_cast<std::uint16_t>(newRows - 1);
    if (CursorCol_ >= newCols)
        CursorCol_ = static_cast<std::uint16_t>(newCols - 1);
    PendingWrap_ = false;

    const std::uint16_t clampCols = newCols < Cols_ ? newCols : Cols_;
    const std::uint16_t oldRows = Rows_;
    auto truncateCellsUsed = [&](Line* lines, std::uint16_t rows) noexcept {
        for (std::uint16_t r = 0; r < rows; ++r)
            if (lines[r].CellsUsed > clampCols) lines[r].CellsUsed = clampCols;
    };
    truncateCellsUsed(MainLines_, oldRows);
    truncateCellsUsed(AltLines_, oldRows);

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
        Cols_ = (newCols < Cols_) ? newCols : Cols_;
        Rows_ = (newRows < Rows_) ? newRows : Rows_;
        ScrollTop_ = 0;
        ScrollBot_ = static_cast<std::uint16_t>(Rows_ - 1);
        return;
    }

    for (std::uint16_t r = 0; r < newRows; ++r) {
        newMainLines[r].Cells = newMain + static_cast<std::size_t>(r) * newCols;
        newAltLines[r].Cells = newAlt + static_cast<std::size_t>(r) * newCols;
    }

    const std::uint16_t copyRows = newRows < oldRows ? newRows : oldRows;
    for (std::uint16_t r = 0; r < copyRows; ++r) {
        CopyCells(newMainLines[r].Cells, MainLines_[r].Cells, clampCols);
        CopyCells(newAltLines[r].Cells, AltLines_[r].Cells, clampCols);
        newMainLines[r].CellsUsed = MainLines_[r].CellsUsed < newCols
                                        ? MainLines_[r].CellsUsed
                                        : newCols;
        newAltLines[r].CellsUsed =
            AltLines_[r].CellsUsed < newCols ? AltLines_[r].CellsUsed : newCols;
    }

    delete[] MainCells_;
    delete[] AltCells_;
    delete[] MainLines_;
    delete[] AltLines_;
    MainCells_ = newMain;
    AltCells_ = newAlt;
    MainLines_ = newMainLines;
    AltLines_ = newAltLines;

    if (newCols != Cols_ && HistoryLines_) {
        const std::size_t newHistCells =
            static_cast<std::size_t>(ScrollbackLimit_) * newCols;
        Cell* newHistCellsArena = new (std::nothrow) Cell[newHistCells]{};
        Line* newHistLines = new (std::nothrow) Line[ScrollbackLimit_]{};
        if (newHistCellsArena && newHistLines) {
            // Wire all Cells pointers before the copy loop (clang-analyzer).
            for (std::uint32_t r = 0; r < ScrollbackLimit_; ++r) {
                newHistLines[r].Cells =
                    newHistCellsArena + static_cast<std::size_t>(r) * newCols;
            }
            for (std::uint32_t i = 0; i < HistorySize_; ++i) {
                const std::uint32_t src =
                    (HistoryStart_ + i) % ScrollbackLimit_;
                const std::uint32_t dst = i % ScrollbackLimit_;
                CopyCells(newHistLines[dst].Cells, HistoryLines_[src].Cells,
                          clampCols);
                newHistLines[dst].CellsUsed =
                    HistoryLines_[src].CellsUsed < newCols
                        ? HistoryLines_[src].CellsUsed
                        : newCols;
            }
            delete[] HistoryCells_;
            delete[] HistoryLines_;
            HistoryCells_ = newHistCellsArena;
            HistoryLines_ = newHistLines;
            HistoryStart_ = 0;
        } else {
            delete[] newHistCellsArena;
            delete[] newHistLines;
        }
    }

    if (newCols > Cols_) {
        for (std::uint16_t c = Cols_; c < newCols; c += DEFAULT_TAB_INTERVAL)
            TabStops_[c / 64] |= (std::uint64_t{1} << (c % 64));
    }

    Cols_ = newCols;
    Rows_ = newRows;
    ScrollTop_ = 0;
    ScrollBot_ = static_cast<std::uint16_t>(newRows - 1);

    auto clampSC = [&](SavedCursor& sc) noexcept {
        if (sc.Row >= newRows) sc.Row = static_cast<std::uint16_t>(newRows - 1);
        if (sc.Col >= newCols) sc.Col = static_cast<std::uint16_t>(newCols - 1);
    };
    clampSC(SavedCursorMain_);
    clampSC(SavedCursorAlt_);
}

}  // namespace zet::grid
