// Snapshot serializer for zet_grid (design.md §9).
//
// Produces a byte sequence that — when fed to a fresh VtParser+Grid of the
// same dimensions — reproduces the visible screen, history, cursor state and
// modes.  No dynamic allocation after the initial buffer; no exceptions; no
// abort, exit or terminate (nm-gate clean).

#include "zet/grid/snapshot.hpp"

#include <cstring>
#include <new>
#include <span>

#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"

namespace zet::grid {

// ---------------------------------------------------------------------------
// EscWriter — fixed-capacity write-ahead buffer (no heap after initial alloc)
// ---------------------------------------------------------------------------

struct EscWriter {
    std::byte* Buf;
    std::size_t Cap;
    std::size_t Pos{0};
    bool Overflow{false};

    void WriteByte(std::uint8_t b) noexcept {
        if (Pos < Cap)
            Buf[Pos++] = static_cast<std::byte>(b);
        else
            Overflow = true;
    }

    void WriteBytes(const char* s, std::size_t n) noexcept {
        if (Pos + n <= Cap) {
            std::memcpy(Buf + Pos, s, n);
            Pos += n;
        } else {
            Overflow = true;
        }
    }

    void WriteLiteral(const char* s) noexcept {
        while (*s) WriteByte(static_cast<std::uint8_t>(*s++));
    }

    void WriteDecimal(std::uint32_t n) noexcept {
        // max uint32 = 4294967295 = 10 digits
        char tmp[10];
        int len = 0;
        if (n == 0) {
            WriteByte('0');
            return;
        }
        while (n > 0) {
            tmp[len++] = static_cast<char>('0' + n % 10);
            n /= 10;
        }
        // tmp is reversed
        for (int i = len - 1; i >= 0; --i)
            WriteByte(static_cast<std::uint8_t>(tmp[i]));
    }

    void WriteUtf8(char32_t cp) noexcept {
        if (cp < 0x80) {
            WriteByte(static_cast<std::uint8_t>(cp));
        } else if (cp < 0x800) {
            WriteByte(0xC0 | static_cast<std::uint8_t>(cp >> 6));
            WriteByte(0x80 | static_cast<std::uint8_t>(cp & 0x3F));
        } else if (cp < 0x10000) {
            WriteByte(0xE0 | static_cast<std::uint8_t>(cp >> 12));
            WriteByte(0x80 | static_cast<std::uint8_t>((cp >> 6) & 0x3F));
            WriteByte(0x80 | static_cast<std::uint8_t>(cp & 0x3F));
        } else {
            WriteByte(0xF0 | static_cast<std::uint8_t>(cp >> 18));
            WriteByte(0x80 | static_cast<std::uint8_t>((cp >> 12) & 0x3F));
            WriteByte(0x80 | static_cast<std::uint8_t>((cp >> 6) & 0x3F));
            WriteByte(0x80 | static_cast<std::uint8_t>(cp & 0x3F));
        }
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Replace hostile codepoints with '?' so they never appear raw in output.
static char32_t SanitizeCp(char32_t cp) noexcept {
    if (cp == 0) return U' ';                       // blank cell
    if (cp < 0x20) return U'?';                     // C0 (except space at 0x20)
    if (cp == 0x7F) return U'?';                    // DEL
    if (cp >= 0x80 && cp <= 0x9F) return U'?';      // C1
    if (cp >= 0xD800 && cp <= 0xDFFF) return U'?';  // surrogates
    if (cp > 0x10FFFF) return U'?';                 // beyond Unicode
    return cp;
}

/// True when a cell is a blank with default pen (trailing space equivalent).
static bool IsBlankCell(const Cell& c) noexcept {
    return (c.Codepoint == 0 || c.Codepoint == U' ') && c.Attrs == 0 &&
           c.AttrEx == 0 && c.UlIdx == 0 && c.FgKind == EColorKind::Default &&
           c.BgKind == EColorKind::Default;
}

/// Find the rightmost non-blank column + 1 in a screen row.
static std::uint16_t ScreenCellsUsed(const Grid& g,
                                     std::uint16_t row) noexcept {
    std::uint16_t used = g.Cols();
    while (used > 0 &&
           IsBlankCell(g.CellAt(row, static_cast<std::uint16_t>(used - 1))))
        --used;
    return used;
}

// ---------------------------------------------------------------------------
// Pen state — tracks the "current SGR pen" while serializing
// ---------------------------------------------------------------------------

struct Pen {
    std::uint8_t Attrs{0};
    EColorKind FgKind{EColorKind::Default};
    EColorKind BgKind{EColorKind::Default};
    std::uint8_t Fg[3]{};
    std::uint8_t Bg[3]{};
    std::uint8_t AttrEx{0};
    std::uint8_t UlIdx{0};

    [[nodiscard]] bool MatchesCell(const Cell& c) const noexcept {
        return Attrs == c.Attrs && FgKind == c.FgKind && BgKind == c.BgKind &&
               Fg[0] == c.Fg[0] && Fg[1] == c.Fg[1] && Fg[2] == c.Fg[2] &&
               Bg[0] == c.Bg[0] && Bg[1] == c.Bg[1] && Bg[2] == c.Bg[2] &&
               AttrEx == c.AttrEx && UlIdx == c.UlIdx;
    }
};

/// Emit a complete SGR sequence (reset + all non-default attrs) for a cell.
/// Updates pen to the cell's attrs.
static void EmitSgr(EscWriter& w, Pen& pen, const Cell& c) noexcept {
    if (pen.MatchesCell(c)) return;

    w.WriteLiteral("\x1b[");
    bool first = true;
    auto Sep = [&]() {
        if (!first) w.WriteByte(';');
        first = false;
    };

    // Always reset first to ensure a clean baseline.
    Sep();
    w.WriteByte('0');

    if (c.Attrs & Bold) {
        Sep();
        w.WriteByte('1');
    }
    if (c.Attrs & Dim) {
        Sep();
        w.WriteByte('2');
    }
    if (c.Attrs & Italic) {
        Sep();
        w.WriteByte('3');
    }
    if (c.Attrs & Underline) {
        Sep();
        w.WriteByte('4');
    }
    if (c.Attrs & Blink) {
        Sep();
        w.WriteByte('5');
    }
    if (c.Attrs & Inverse) {
        Sep();
        w.WriteByte('7');
    }
    if (c.Attrs & Invisible) {
        Sep();
        w.WriteByte('8');
    }
    if (c.Attrs & StrikeThrough) {
        Sep();
        w.WriteByte('9');
    }

    if (c.FgKind == EColorKind::Indexed) {
        std::uint8_t idx = c.Fg[0];
        if (idx < 8) {
            Sep();
            w.WriteDecimal(30u + idx);
        } else if (idx < 16) {
            Sep();
            w.WriteDecimal(90u + (idx - 8u));
        } else {
            Sep();
            w.WriteLiteral("38;5;");
            w.WriteDecimal(idx);
        }
    } else if (c.FgKind == EColorKind::TrueColor) {
        Sep();
        w.WriteLiteral("38;2;");
        w.WriteDecimal(c.Fg[0]);
        w.WriteByte(';');
        w.WriteDecimal(c.Fg[1]);
        w.WriteByte(';');
        w.WriteDecimal(c.Fg[2]);
    }

    if (c.BgKind == EColorKind::Indexed) {
        std::uint8_t idx = c.Bg[0];
        if (idx < 8) {
            Sep();
            w.WriteDecimal(40u + idx);
        } else if (idx < 16) {
            Sep();
            w.WriteDecimal(100u + (idx - 8u));
        } else {
            Sep();
            w.WriteLiteral("48;5;");
            w.WriteDecimal(idx);
        }
    } else if (c.BgKind == EColorKind::TrueColor) {
        Sep();
        w.WriteLiteral("48;2;");
        w.WriteDecimal(c.Bg[0]);
        w.WriteByte(';');
        w.WriteDecimal(c.Bg[1]);
        w.WriteByte(';');
        w.WriteDecimal(c.Bg[2]);
    }

    w.WriteByte('m');

    // Update pen state.
    pen.Attrs = c.Attrs;
    pen.FgKind = c.FgKind;
    pen.BgKind = c.BgKind;
    pen.Fg[0] = c.Fg[0];
    pen.Fg[1] = c.Fg[1];
    pen.Fg[2] = c.Fg[2];
    pen.Bg[0] = c.Bg[0];
    pen.Bg[1] = c.Bg[1];
    pen.Bg[2] = c.Bg[2];
    pen.AttrEx = c.AttrEx;
    pen.UlIdx = c.UlIdx;
}

// Emit SGR for a "virtual" cell built from SavedCursor fields (for pen
// restore).
static void EmitSgrFromSaved(EscWriter& w, const SavedCursor& sc) noexcept {
    w.WriteLiteral("\x1b[0");

    if (sc.Attrs & Bold) w.WriteLiteral(";1");
    if (sc.Attrs & Dim) w.WriteLiteral(";2");
    if (sc.Attrs & Italic) w.WriteLiteral(";3");
    if (sc.Attrs & Underline) w.WriteLiteral(";4");
    if (sc.Attrs & Blink) w.WriteLiteral(";5");
    if (sc.Attrs & Inverse) w.WriteLiteral(";7");
    if (sc.Attrs & Invisible) w.WriteLiteral(";8");
    if (sc.Attrs & StrikeThrough) w.WriteLiteral(";9");

    if (sc.FgKind == EColorKind::Indexed) {
        std::uint8_t idx = sc.Fg[0];
        if (idx < 8) {
            w.WriteByte(';');
            w.WriteDecimal(30u + idx);
        } else if (idx < 16) {
            w.WriteByte(';');
            w.WriteDecimal(90u + (idx - 8u));
        } else {
            w.WriteLiteral(";38;5;");
            w.WriteDecimal(idx);
        }
    } else if (sc.FgKind == EColorKind::TrueColor) {
        w.WriteLiteral(";38;2;");
        w.WriteDecimal(sc.Fg[0]);
        w.WriteByte(';');
        w.WriteDecimal(sc.Fg[1]);
        w.WriteByte(';');
        w.WriteDecimal(sc.Fg[2]);
    }

    if (sc.BgKind == EColorKind::Indexed) {
        std::uint8_t idx = sc.Bg[0];
        if (idx < 8) {
            w.WriteByte(';');
            w.WriteDecimal(40u + idx);
        } else if (idx < 16) {
            w.WriteByte(';');
            w.WriteDecimal(100u + (idx - 8u));
        } else {
            w.WriteLiteral(";48;5;");
            w.WriteDecimal(idx);
        }
    } else if (sc.BgKind == EColorKind::TrueColor) {
        w.WriteLiteral(";48;2;");
        w.WriteDecimal(sc.Bg[0]);
        w.WriteByte(';');
        w.WriteDecimal(sc.Bg[1]);
        w.WriteByte(';');
        w.WriteDecimal(sc.Bg[2]);
    }

    w.WriteByte('m');
}

static void EmitMode(EscWriter& w, std::uint16_t ps, bool on) noexcept {
    w.WriteLiteral("\x1b[?");
    w.WriteDecimal(ps);
    w.WriteByte(on ? 'h' : 'l');
}

// ---------------------------------------------------------------------------
// Encode history section (oldest→newest scrollback lines).
// ---------------------------------------------------------------------------

static void EncodeHistory(EscWriter& w, const Grid& g, Pen& pen) noexcept {
    const std::uint32_t sz = g.ScrollbackSize();
    for (std::uint32_t i = 0; i < sz; ++i) {
        const Line* line = g.ScrollbackLine(i);
        if (!line) continue;

        const std::uint16_t used = line->CellsUsed;
        std::uint16_t col = 0;
        while (col < used) {
            const Cell& c = line->Cells[col];
            // Skip wide continuation cells.
            if (c.Width == 0) {
                ++col;
                continue;
            }

            EmitSgr(w, pen, c);
            char32_t cp = SanitizeCp(c.Codepoint);
            w.WriteUtf8(cp);
            // Wide cells occupy 2 columns; continuation col is skipped next
            // iteration.
            col += (c.Width >= 2) ? 2 : 1;
        }

        // Reset pen at end of line for cleanliness (avoids SGR bleeding into
        // \r\n).
        if (pen.Attrs || pen.FgKind != EColorKind::Default ||
            pen.BgKind != EColorKind::Default) {
            w.WriteLiteral("\x1b[0m");
            pen = Pen{};
        }
        w.WriteLiteral("\r\n");
    }
}

// ---------------------------------------------------------------------------
// Encode screen section (cell-by-cell with CUP addressing).
// ---------------------------------------------------------------------------

static void EncodeScreen(EscWriter& w, const Grid& g, Pen& pen) noexcept {
    const std::uint16_t rows = g.Rows();
    const std::uint16_t cols = g.Cols();

    for (std::uint16_t row = 0; row < rows; ++row) {
        const std::uint16_t used = ScreenCellsUsed(g, row);
        if (used == 0) continue;

        // Move to start of this row (1-based CUP).
        w.WriteLiteral("\x1b[");
        w.WriteDecimal(static_cast<std::uint32_t>(row + 1));
        w.WriteLiteral(";1H");

        std::uint16_t col = 0;
        while (col < used) {
            const Cell& c = g.CellAt(row, col);
            // Skip wide continuation cells.
            if (c.Width == 0) {
                ++col;
                continue;
            }

            // If this cell is the continuation of a wide glyph starting before
            // the used boundary — shouldn't happen but guard anyway.
            EmitSgr(w, pen, c);
            char32_t cp = SanitizeCp(c.Codepoint);
            w.WriteUtf8(cp);
            col += (c.Width >= 2) ? static_cast<std::uint16_t>(2)
                                  : static_cast<std::uint16_t>(1);
        }

        // Ensure col count matches: if last emitted cell was wide, terminal
        // cursor is already advanced past the second column — no fix needed.
        (void)cols;
    }
}

// ---------------------------------------------------------------------------
// Encode mode-restore section (design.md §9).
// ---------------------------------------------------------------------------

static void EncodeModeRestore(EscWriter& w, const Grid& g) noexcept {
    // Reset SGR pen and restore saved cursor colours (for DECSC/DECRC).
    w.WriteLiteral("\x1b[0m");

    const SavedCursor& sc =
        g.IsAltScreen() ? g.SavedCursorAltState() : g.SavedCursorMainState();
    EmitSgrFromSaved(w, sc);

    // DECSC — save cursor (so DECRC restores the saved position).
    // We emit ESC 7 to store the saved cursor into the terminal.
    // First move to the saved cursor position so ESC 7 captures it.
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(static_cast<std::uint32_t>(sc.Row + 1));
    w.WriteByte(';');
    w.WriteDecimal(static_cast<std::uint32_t>(sc.Col + 1));
    w.WriteByte('H');
    w.WriteLiteral(
        "\x1b"
        "7");  // ESC 7 = DECSC

    // DECSTBM — scroll region (1-based).
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(static_cast<std::uint32_t>(g.ScrollTop() + 1));
    w.WriteByte(';');
    w.WriteDecimal(static_cast<std::uint32_t>(g.ScrollBot() + 1));
    w.WriteByte('r');

    // Private modes.
    EmitMode(w, 1, g.ModeSet(EMode::Decckm));
    EmitMode(w, 7, g.ModeSet(EMode::Awm));
    EmitMode(w, 4, g.ModeSet(EMode::Irm));
    EmitMode(w, 20, g.ModeSet(EMode::Lnm));
    EmitMode(w, 6, g.ModeSet(EMode::Decom));
    EmitMode(w, 1000, g.ModeSet(EMode::Mouse1000));
    EmitMode(w, 1002, g.ModeSet(EMode::Mouse1002));
    EmitMode(w, 1003, g.ModeSet(EMode::Mouse1003));
    EmitMode(w, 1005, g.ModeSet(EMode::Mouse1005));
    EmitMode(w, 1006, g.ModeSet(EMode::Mouse1006));
    EmitMode(w, 1015, g.ModeSet(EMode::Mouse1015));
    EmitMode(w, 2004, g.ModeSet(EMode::BracketedPaste));
    EmitMode(w, 1004, g.ModeSet(EMode::FocusEvents));

    // Charset designations.
    w.WriteLiteral("\x1b(");
    w.WriteByte(g.CharsetG0() == 0 ? 'B' : '0');  // G0: B=ASCII, 0=line-drawing
    w.WriteLiteral("\x1b)");
    w.WriteByte(g.CharsetG1() == 0 ? 'B' : '0');

    // Charset active (SO = G1, SI = G0).
    w.WriteByte(g.CharsetActive() == 1 ? '\x0e' : '\x0f');

    // DECSCUSR — cursor style.
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(g.CursorStyle());
    w.WriteLiteral(" q");

    // Cursor position (1-based CUP).
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(static_cast<std::uint32_t>(g.CursorRow() + 1));
    w.WriteByte(';');
    w.WriteDecimal(static_cast<std::uint32_t>(g.CursorCol() + 1));
    w.WriteByte('H');

    // DECTCEM — cursor visibility.
    EmitMode(w, 25, g.ModeSet(EMode::Dectcem));

    // End sync-output.
    w.WriteLiteral("\x1b[?2026l");
}

// ---------------------------------------------------------------------------
// Snapshot — public API
// ---------------------------------------------------------------------------

Snapshot::~Snapshot() noexcept { delete[] Buf_; }

std::span<const std::byte> Snapshot::Bytes() const noexcept {
    return {Buf_, Len_};
}

GridResult<Snapshot*> Snapshot::Create(const Grid& grid,
                                       SnapshotOptions opts) noexcept {
    std::size_t maxBytes = opts.MaxBytes;
    if (maxBytes == 0 || maxBytes > SNAPSHOT_MAX_BYTES)
        maxBytes = SNAPSHOT_MAX_BYTES;

    auto* buf = new (std::nothrow) std::byte[maxBytes];
    if (!buf) return std::unexpected(EGridError::AllocationFailed);

    EscWriter w{buf, maxBytes};
    Pen pen{};

    const bool altScreen = grid.IsAltScreen();

    // -----------------------------------------------------------------------
    // Preamble
    // -----------------------------------------------------------------------
    // CAN + ESC-ST: resets DCS/OSC/APC passthrough in the receiving terminal.
    w.WriteByte('\x18');       // CAN
    w.WriteLiteral("\x1b\\");  // ESC \

    // Alt/main screen switch + initial clear.
    if (altScreen) {
        w.WriteLiteral("\x1b[?1049h");  // enter alt screen
    } else {
        w.WriteLiteral("\x1b[?1049l");  // exit alt screen (go to main)
    }
    w.WriteLiteral("\x1b[2J\x1b[H");  // erase screen + home

    // -----------------------------------------------------------------------
    // History section (scrollback, main screen only)
    // -----------------------------------------------------------------------
    const std::size_t scrollbackStart = w.Pos;
    if (!altScreen && grid.ScrollbackSize() > 0) {
        EncodeHistory(w, grid, pen);
        // The history lines are on the target screen (rows 0..N-1).  Emit
        // grid.Rows() newlines so every history line scrolls into the target's
        // own scrollback before the second clear.
        for (std::uint16_t i = 0; i < grid.Rows(); ++i) w.WriteByte('\n');
    }
    const std::size_t scrollbackLen = w.Pos - scrollbackStart;

    // -----------------------------------------------------------------------
    // Screen section
    // -----------------------------------------------------------------------
    // Second clear (for main screen: after history, the visible area needs
    // clearing before we paint screen cells).
    if (!altScreen) {
        w.WriteLiteral("\x1b[2J\x1b[H");
    }

    // Hide cursor before paint (outside sync — avoids cursor flicker).
    w.WriteLiteral("\x1b[?25l");

    // Start sync-output: wraps only the screen paint.
    w.WriteLiteral("\x1b[?2026h");

    // Reset pen at screen section boundary.
    pen = Pen{};

    // Screen cells.
    EncodeScreen(w, grid, pen);

    // Mode restore + end sync.
    EncodeModeRestore(w, grid);

    if (w.Overflow) {
        delete[] buf;
        return std::unexpected(EGridError::SnapshotTooLarge);
    }

    auto* snap = new (std::nothrow) Snapshot;
    if (!snap) {
        delete[] buf;
        return std::unexpected(EGridError::AllocationFailed);
    }

    // ScrollbackLen_ = preamble bytes + history bytes (offset where screen
    // section starts).
    const std::size_t preambleLen = scrollbackStart;
    snap->Buf_ = buf;
    snap->Len_ = w.Pos;
    snap->ScrollbackLen_ = preambleLen + scrollbackLen;
    snap->PossiblyDiverged_ = grid.PossiblyDiverged();
    snap->AltScreen_ = altScreen;
    return snap;
}

}  // namespace zet::grid
