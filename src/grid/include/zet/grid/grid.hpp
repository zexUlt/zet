#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

#include "zet/grid/cell.hpp"
#include "zet/grid/limits.hpp"

namespace zet::grid {

/// Errors returned by Grid operations.
enum class EGridError : std::uint8_t {
    /// Allocation failed (new(std::nothrow) returned nullptr).
    AllocationFailed,
    /// Requested size exceeds MAX_COLS × MAX_ROWS.
    SizeTooLarge,
    /// Scrollback limit exceeds implementation max (effectively 2^31).
    ScrollbackTooLarge,
};

template <typename TValue>
using GridResult = std::expected<TValue, EGridError>;

/// Terminal mode flags tracked in Grid::Modes_ (DECSET/DECRST).
enum class EMode : std::uint32_t {
    // Bit positions must be stable; new modes are assigned the next free bit.
    Awm = 1U << 0,              ///< DECAWM  ?7  auto-wrap
    Irm = 1U << 1,              ///< IRM     ?4  insert mode
    Lnm = 1U << 2,              ///< LNM     ?20 line feed / new line
    Decom = 1U << 3,            ///< DECOM   ?6  origin mode
    Dectcem = 1U << 4,          ///< DECTCEM ?25 cursor visible
    Decckm = 1U << 5,           ///< DECCKM  ?1  application cursor keys
    AltScreen = 1U << 6,        ///< ?1049 alt screen active
    Mouse1000 = 1U << 7,        ///< ?1000
    Mouse1002 = 1U << 8,        ///< ?1002
    Mouse1003 = 1U << 9,        ///< ?1003
    Mouse1005 = 1U << 10,       ///< ?1005
    Mouse1006 = 1U << 11,       ///< ?1006 (SGR mouse)
    Mouse1015 = 1U << 12,       ///< ?1015
    BracketedPaste = 1U << 13,  ///< ?2004
    FocusEvents = 1U << 14,     ///< ?1004
    SyncOutput = 1U << 15,      ///< ?2026
};

[[nodiscard]] constexpr std::uint32_t ModeFlag(EMode m) noexcept {
    return static_cast<std::uint32_t>(m);
}

/// One row of the terminal screen (or scrollback).
///
/// Allocated in a fixed arena; never grows after construction.
struct Line {
    Cell* Cells{nullptr};        ///< [0 .. grid.Cols())
    std::uint16_t CellsUsed{0};  ///< rightmost non-blank column + 1
};

/// Saved cursor state (DECSC/DECRC and alt-screen switch).
struct SavedCursor {
    std::uint16_t Row{0};
    std::uint16_t Col{0};
    std::uint8_t Attrs{0};
    EColorKind FgKind{EColorKind::Default};
    EColorKind BgKind{EColorKind::Default};
    std::uint8_t Fg[3]{};    ///< inline color (see Cell::Fg)
    std::uint8_t Bg[3]{};    ///< inline color (see Cell::Bg)
    std::uint8_t AttrEx{0};  ///< underline style
    std::uint8_t UlIdx{0};   ///< underline colour index
};

/// Sans-I/O terminal grid (design.md §9).
///
/// Owns two screen buffers (main + alt) and a scrollback ring.  All memory
/// is allocated once at construction with new(std::nothrow); there are no
/// subsequent allocations or throws. Feed bytes through the companion
/// VtParser which calls the Grid mutators.
///
/// Not copyable, not movable — the arena pointers would alias.
class Grid {
public:
    Grid(const Grid&) = delete;
    Grid& operator=(const Grid&) = delete;
    Grid(Grid&&) = delete;
    Grid& operator=(Grid&&) = delete;

    ~Grid();

    /// Construct a Grid with the given dimensions and scrollback limit.
    ///
    /// Uses new(std::nothrow) for all arenas. On allocation failure returns
    /// EGridError::AllocationFailed. cols and rows must be ≥ 1 and ≤ their
    /// respective maximums.
    [[nodiscard]] static GridResult<Grid*> Create(
        std::uint16_t cols, std::uint16_t rows,
        std::uint32_t scrollbackLimit = DEFAULT_SCROLLBACK_LIMIT) noexcept;

    // -----------------------------------------------------------------------
    // Dimensions and state accessors (all noexcept, no bounds checks on read)
    // -----------------------------------------------------------------------

    [[nodiscard]] std::uint16_t Cols() const noexcept { return Cols_; }
    [[nodiscard]] std::uint16_t Rows() const noexcept { return Rows_; }
    [[nodiscard]] std::uint32_t ScrollbackLimit() const noexcept {
        return ScrollbackLimit_;
    }
    [[nodiscard]] std::uint32_t ScrollbackSize() const noexcept {
        return HistorySize_;
    }

    [[nodiscard]] std::uint16_t CursorRow() const noexcept {
        return CursorRow_;
    }
    [[nodiscard]] std::uint16_t CursorCol() const noexcept {
        return CursorCol_;
    }
    [[nodiscard]] bool PendingWrap() const noexcept { return PendingWrap_; }

    [[nodiscard]] std::uint16_t ScrollTop() const noexcept {
        return ScrollTop_;
    }
    [[nodiscard]] std::uint16_t ScrollBot() const noexcept {
        return ScrollBot_;
    }

    [[nodiscard]] bool ModeSet(EMode m) const noexcept {
        return (Modes_ & ModeFlag(m)) != 0;
    }
    [[nodiscard]] std::uint32_t Modes() const noexcept { return Modes_; }

    [[nodiscard]] std::uint8_t CharsetG0() const noexcept { return CharsetG0_; }
    [[nodiscard]] std::uint8_t CharsetG1() const noexcept { return CharsetG1_; }
    [[nodiscard]] std::uint8_t CharsetActive() const noexcept {
        return CharsetActive_;
    }
    [[nodiscard]] std::uint8_t CursorStyle() const noexcept {
        return CursorStyle_;
    }

    [[nodiscard]] bool IsAltScreen() const noexcept {
        return ModeSet(EMode::AltScreen);
    }

    /// Saved cursor state for the main and alt screens (DECSC/DECRC).
    [[nodiscard]] const SavedCursor& SavedCursorMainState() const noexcept {
        return SavedCursorMain_;
    }
    [[nodiscard]] const SavedCursor& SavedCursorAltState() const noexcept {
        return SavedCursorAlt_;
    }

    /// Whether the parser has lost sync with the pty (sixel/DCS without ST,
    /// or a force-reset to Ground). Cleared on each new snapshot.
    [[nodiscard]] bool PossiblyDiverged() const noexcept {
        return PossiblyDiverged_;
    }
    void SetPossiblyDiverged(bool v) noexcept { PossiblyDiverged_ = v; }

    // -----------------------------------------------------------------------
    // Current pen attributes (used by VtParser when writing cells)
    // -----------------------------------------------------------------------

    [[nodiscard]] std::uint8_t PenAttrs() const noexcept { return PenAttrs_; }
    [[nodiscard]] EColorKind PenFgKind() const noexcept { return PenFgKind_; }
    [[nodiscard]] EColorKind PenBgKind() const noexcept { return PenBgKind_; }
    /// Palette index when PenFgKind() == Indexed; undefined otherwise.
    [[nodiscard]] std::uint8_t PenFgIdx() const noexcept { return PenFg_[0]; }
    /// Palette index when PenBgKind() == Indexed; undefined otherwise.
    [[nodiscard]] std::uint8_t PenBgIdx() const noexcept { return PenBg_[0]; }
    /// Raw 3-byte inline fg color: [0]=R or palette idx, [1]=G, [2]=B.
    [[nodiscard]] const std::uint8_t* PenFg() const noexcept { return PenFg_; }
    /// Raw 3-byte inline bg color.
    [[nodiscard]] const std::uint8_t* PenBg() const noexcept { return PenBg_; }
    /// Underline style (0=none, 1=single, 2=double, 3=curly, 4=dotted,
    /// 5=dashed).
    [[nodiscard]] std::uint8_t PenAttrEx() const noexcept { return PenAttrEx_; }
    /// Underline colour palette index (0 = default; set via SGR 58).
    [[nodiscard]] std::uint8_t PenUlIdx() const noexcept { return PenUlIdx_; }

    // -----------------------------------------------------------------------
    // Cell and line accessors
    // -----------------------------------------------------------------------

    /// Access a cell in the active screen buffer.
    /// row must be < Rows_, col must be < Cols_.  No bounds check.
    [[nodiscard]] const Cell& CellAt(std::uint16_t row,
                                     std::uint16_t col) const noexcept;
    [[nodiscard]] Cell& CellAt(std::uint16_t row, std::uint16_t col) noexcept;

    /// Access a scrollback line (0 = oldest, HistorySize_-1 = newest).
    [[nodiscard]] const Line* ScrollbackLine(std::uint32_t idx) const noexcept;

    // -----------------------------------------------------------------------
    // Mutators called by VtParser
    // -----------------------------------------------------------------------

    /// Write a printable codepoint at the cursor, advance, handle wrap.
    void PutChar(char32_t cp, int width) noexcept;

    /// Carriage return.
    void CarriageReturn() noexcept;
    /// Line feed (also used for VT, FF, NEL when LNM is on).
    void LineFeed() noexcept;
    /// Reverse index (RI).
    void ReverseIndex() noexcept;
    /// Index (IND) — like LF but doesn't interpret LNM.
    void Index() noexcept;

    /// Backspace.
    void Backspace() noexcept;
    /// Horizontal tab (forward, HT/CHT).
    void HorizontalTab() noexcept;
    /// Cursor backward tab (CBT — move left N tab stops).
    void HorizontalTabBack(std::uint16_t n) noexcept;

    /// CUU/CUD/CUF/CUB — relative cursor movement.
    void MoveCursorUp(std::uint16_t n) noexcept;
    void MoveCursorDown(std::uint16_t n) noexcept;
    void MoveCursorRight(std::uint16_t n) noexcept;
    void MoveCursorLeft(std::uint16_t n) noexcept;

    /// CUP/HVP — absolute cursor position (1-based from caller).
    void MoveCursorTo(std::uint16_t row1, std::uint16_t col1) noexcept;
    /// CHA — cursor to column (1-based).
    void MoveCursorToCol(std::uint16_t col1) noexcept;
    /// VPA — cursor to row (1-based).
    void MoveCursorToRow(std::uint16_t row1) noexcept;
    /// CNL/CPL — cursor next/prev line.
    void CursorNextLine(std::uint16_t n) noexcept;
    void CursorPrevLine(std::uint16_t n) noexcept;

    /// DECSC/DECRC — save/restore cursor (includes colors per design.md).
    void SaveCursor() noexcept;
    void RestoreCursor() noexcept;

    /// ED — erase in display.
    void EraseInDisplay(std::uint8_t param) noexcept;
    /// EL — erase in line.
    void EraseInLine(std::uint8_t param) noexcept;
    /// ECH — erase N chars.
    void EraseChars(std::uint16_t n) noexcept;
    /// DCH — delete N chars.
    void DeleteChars(std::uint16_t n) noexcept;
    /// ICH — insert N blank chars.
    void InsertChars(std::uint16_t n) noexcept;
    /// IL — insert N lines.
    void InsertLines(std::uint16_t n) noexcept;
    /// DL — delete N lines.
    void DeleteLines(std::uint16_t n) noexcept;

    /// SD/SU — scroll down/up N lines within scroll region.
    void ScrollDown(std::uint16_t n) noexcept;
    void ScrollUp(std::uint16_t n) noexcept;

    /// REP — repeat last printed character.
    void RepeatChar(std::uint16_t n) noexcept;

    /// DECSTBM — set scroll region (1-based from caller).
    void SetScrollRegion(std::uint16_t top1, std::uint16_t bot1) noexcept;

    /// DECSET/DECRST — set/reset private mode.
    void SetMode(std::uint16_t param, bool value) noexcept;

    /// SGR — set graphic rendition (attrs, colors).
    /// colon[k] is true when param k is a sub-param of the previous entry
    /// (separated by ':' rather than ';' in the original stream).
    void ApplySgr(const std::uint16_t* params, const bool* colon,
                  std::size_t count) noexcept;

    /// RIS/DECSTR — soft or hard reset.
    void Reset(bool hard) noexcept;

    /// DECSCUSR — set cursor style.
    void SetCursorStyle(std::uint8_t style) noexcept;

    /// HTS — set horizontal tab stop at current column.
    void SetTabStop() noexcept;
    /// TBC — clear tab stop(s).
    void ClearTabStop(std::uint8_t param) noexcept;

    /// SO/SI — shift-out / shift-in charset.
    void ShiftCharset(bool out) noexcept;
    /// Designate G0/G1 charset (ESC ( B, ESC ) B, etc.).
    void DesignateCharset(std::uint8_t g, std::uint8_t designator) noexcept;

    /// Alt screen switch (?1049 / ?47).
    void SwitchAltScreen(bool enter) noexcept;

    /// Resize (design.md §9 — truncate, no reflow).
    ///
    /// Allocates all new buffers before mutating any grid state.  On
    /// allocation failure the grid is left completely unchanged and
    /// EGridError::AllocationFailed is returned.
    [[nodiscard]] GridResult<void> Resize(std::uint16_t newCols,
                                          std::uint16_t newRows) noexcept;

private:
    explicit Grid() noexcept = default;

    // Allocate all arenas. Returns false on any allocation failure.
    bool Allocate(std::uint16_t cols, std::uint16_t rows,
                  std::uint32_t scrollbackLimit) noexcept;

    // Internal helpers.
    void ScrollRegionUp(std::uint16_t n, bool pushScrollback = true) noexcept;
    void ScrollRegionDown(std::uint16_t n) noexcept;
    void ClearLine(Line& line) noexcept;
    void ClearCells(Cell* cells, std::uint16_t count) noexcept;
    void CopyCells(Cell* dst, const Cell* src, std::uint16_t count) noexcept;
    void MoveCells(Cell* dst, const Cell* src, std::uint16_t count) noexcept;
    void BlankCell(Cell& cell) noexcept;
    void PenToCell(Cell& cell) noexcept;
    [[nodiscard]] Line& ActiveLine(std::uint16_t row) noexcept;
    [[nodiscard]] const Line& ActiveLine(std::uint16_t row) const noexcept;
    [[nodiscard]] Line& ActiveBuf(std::uint16_t row) noexcept;
    void PushScrollback(std::uint16_t row) noexcept;
    void UpdateCellsUsed(Line& line) noexcept;
    void RepairWideOrphans(Line& line, std::uint16_t first,
                           std::uint16_t last) noexcept;

    // Clamp col/row to [0, Cols_-1] / [0, Rows_-1].
    [[nodiscard]] std::uint16_t ClampCol(std::uint16_t c) const noexcept {
        return c < Cols_ ? c : static_cast<std::uint16_t>(Cols_ - 1);
    }
    [[nodiscard]] std::uint16_t ClampRow(std::uint16_t r) const noexcept {
        return r < Rows_ ? r : static_cast<std::uint16_t>(Rows_ - 1);
    }

    // -----------------------------------------------------------------------
    // Screen buffers
    // -----------------------------------------------------------------------
    Line* MainLines_{nullptr};  ///< [Rows_] line metadata for main screen
    Cell* MainCells_{nullptr};  ///< [Rows_ * Cols_] cell arena for main screen
    Line* AltLines_{nullptr};   ///< [Rows_] line metadata for alt screen
    Cell* AltCells_{nullptr};   ///< [Rows_ * Cols_] cell arena for alt screen

    // -----------------------------------------------------------------------
    // Scrollback ring (main screen only, per design.md §9)
    // -----------------------------------------------------------------------
    Line* HistoryLines_{nullptr};  ///< [ScrollbackLimit_] ring entries
    Cell* HistoryCells_{nullptr};  ///< [ScrollbackLimit_ * Cols_] cell arena

    // Ring bookkeeping.
    std::uint32_t HistoryStart_{0};  ///< index of oldest entry in the ring
    std::uint32_t HistorySize_{
        0};  ///< number of valid entries (≤ ScrollbackLimit_)

    // -----------------------------------------------------------------------
    // Dimensions
    // -----------------------------------------------------------------------
    std::uint16_t Cols_{0};
    std::uint16_t Rows_{0};
    std::uint32_t ScrollbackLimit_{0};

    // -----------------------------------------------------------------------
    // Cursor
    // -----------------------------------------------------------------------
    std::uint16_t CursorRow_{0};
    std::uint16_t CursorCol_{0};
    bool PendingWrap_{false};  ///< next printable char starts a new line

    // -----------------------------------------------------------------------
    // Scroll region (DECSTBM, 0-based inclusive)
    // -----------------------------------------------------------------------
    std::uint16_t ScrollTop_{0};
    std::uint16_t ScrollBot_{0};  ///< default = Rows_ - 1

    // -----------------------------------------------------------------------
    // Modes and appearance
    // -----------------------------------------------------------------------
    std::uint32_t Modes_{0};
    std::uint8_t PenAttrs_{0};
    EColorKind PenFgKind_{EColorKind::Default};
    EColorKind PenBgKind_{EColorKind::Default};
    std::uint8_t PenFg_[3]{};  ///< [0]=R or palette idx, [1]=G, [2]=B
    std::uint8_t PenBg_[3]{};
    std::uint8_t PenAttrEx_{0};  ///< underline style for pen
    std::uint8_t PenUlIdx_{0};   ///< underline colour pen palette index

    SavedCursor SavedCursorMain_{};
    SavedCursor SavedCursorAlt_{};

    std::uint8_t CharsetG0_{0};  ///< 0 = ASCII/Latin, 1 = graphics
    std::uint8_t CharsetG1_{0};
    std::uint8_t CharsetActive_{0};  ///< 0 = G0, 1 = G1

    std::uint8_t CursorStyle_{0};

    // Tab stops: one bit per column.  MAX_COLS=4096, 4096/64 = 64 uint64_t.
    static constexpr std::size_t TAB_WORDS = MAX_COLS / 64;
    std::uint64_t TabStops_[TAB_WORDS]{};

    // Last printed character (for REP).
    char32_t LastPrintedChar_{0};
    int LastPrintedWidth_{1};

    bool PossiblyDiverged_{false};
};

}  // namespace zet::grid
