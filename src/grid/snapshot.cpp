// Snapshot serializer for zet_grid (design.md §9).
//
// Produces a byte sequence that — when fed to a fresh VtParser+Grid of the
// same dimensions — reproduces the visible screen, history, cursor state and
// modes.  No dynamic allocation after the initial buffer; no exceptions; no
// abort, exit or terminate (nm-gate clean).
//
// Degrade ladder (applied in order when the cap is exceeded):
//   1. Trim scrollback history by line count (binary search on count).
//   2. Downgrade TrueColor → nearest xterm-256 palette colour.
//   3. Strip all foreground/background colour.
//   4. Crop screen rows from the top (cursor row always kept).

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
// Colour mode (internal — controls how cells are emitted)
// ---------------------------------------------------------------------------

enum class EColorMode : std::uint8_t {
    Full,        ///< Emit all colours exactly.
    Indexed256,  ///< Downgrade TrueColor → nearest xterm-256 palette index.
    None,        ///< Strip all foreground/background colour.
};

// ---------------------------------------------------------------------------
// TrueColor → nearest xterm-256 palette index mapping
// ---------------------------------------------------------------------------

/// Map an RGB triple to the nearest index in the 256-colour xterm palette.
///
/// Considers both the 6×6×6 colour cube (indices 16–231) and the 24-step
/// grayscale ramp (indices 232–255).  The system colours (0–15) are excluded
/// because their exact RGB values are terminal-defined.
static std::uint8_t TrueColorTo256(std::uint8_t r, std::uint8_t g,
                                   std::uint8_t b) noexcept {
    // 6×6×6 cube: step values per channel are {0, 95, 135, 175, 215, 255}.
    static constexpr std::uint8_t kCubeSteps[6] = {0, 95, 135, 175, 215, 255};

    // Find nearest cube index for a single channel value.
    auto cubeIdx = [](std::uint8_t v) -> std::uint8_t {
        std::uint8_t best = 0;
        int bestDist = 255 * 255 + 1;
        for (int i = 0; i < 6; ++i) {
            int d = static_cast<int>(v) - static_cast<int>(kCubeSteps[i]);
            if (d * d < bestDist) {
                bestDist = d * d;
                best = static_cast<std::uint8_t>(i);
            }
        }
        return best;
    };

    const std::uint8_t ri = cubeIdx(r);
    const std::uint8_t gi = cubeIdx(g);
    const std::uint8_t bi = cubeIdx(b);
    const std::uint8_t cubeColor =
        static_cast<std::uint8_t>(16 + 36 * ri + 6 * gi + bi);

    const int cr = kCubeSteps[ri];
    const int cg = kCubeSteps[gi];
    const int cb = kCubeSteps[bi];
    const int cubeDist =
        (cr - r) * (cr - r) + (cg - g) * (cg - g) + (cb - b) * (cb - b);

    // Grayscale ramp 232..255: value(i) = 8 + 10*i.
    const int gray = (static_cast<int>(r) + g + b) / 3;
    const int grayIdxRaw = (gray - 8) / 10;
    const int grayIdx =
        (grayIdxRaw < 0) ? 0 : (grayIdxRaw > 23 ? 23 : grayIdxRaw);
    const int grayVal = 8 + 10 * grayIdx;
    const std::uint8_t grayColor = static_cast<std::uint8_t>(232 + grayIdx);
    const int grayDist = (grayVal - r) * (grayVal - r) +
                         (grayVal - g) * (grayVal - g) +
                         (grayVal - b) * (grayVal - b);

    return (grayDist < cubeDist) ? grayColor : cubeColor;
}

// ---------------------------------------------------------------------------
// Cell normalisation for colour modes
// ---------------------------------------------------------------------------

/// Return a copy of `c` with colour fields adjusted for the given mode.
static Cell NormalizeCell(const Cell& c, EColorMode cm) noexcept {
    if (cm == EColorMode::Full) return c;
    Cell nc = c;
    if (cm == EColorMode::Indexed256) {
        if (c.FgKind == EColorKind::TrueColor) {
            nc.FgKind = EColorKind::Indexed;
            nc.Fg[0] = TrueColorTo256(c.Fg[0], c.Fg[1], c.Fg[2]);
            nc.Fg[1] = 0;
            nc.Fg[2] = 0;
        }
        if (c.BgKind == EColorKind::TrueColor) {
            nc.BgKind = EColorKind::Indexed;
            nc.Bg[0] = TrueColorTo256(c.Bg[0], c.Bg[1], c.Bg[2]);
            nc.Bg[1] = 0;
            nc.Bg[2] = 0;
        }
    } else {  // EColorMode::None
        nc.FgKind = EColorKind::Default;
        nc.BgKind = EColorKind::Default;
        nc.Fg[0] = nc.Fg[1] = nc.Fg[2] = 0;
        nc.Bg[0] = nc.Bg[1] = nc.Bg[2] = 0;
    }
    return nc;
}

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
/// The cell passed here must already be normalised for the active colour mode.
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
// restore).  Colours are suppressed when cm is NoColor; TrueColor is
// downgraded to Indexed256 when cm is Indexed256.
static void EmitSgrFromSaved(EscWriter& w, const SavedCursor& sc,
                             EColorMode cm) noexcept {
    w.WriteLiteral("\x1b[0");

    if (sc.Attrs & Bold) w.WriteLiteral(";1");
    if (sc.Attrs & Dim) w.WriteLiteral(";2");
    if (sc.Attrs & Italic) w.WriteLiteral(";3");
    if (sc.Attrs & Underline) w.WriteLiteral(";4");
    if (sc.Attrs & Blink) w.WriteLiteral(";5");
    if (sc.Attrs & Inverse) w.WriteLiteral(";7");
    if (sc.Attrs & Invisible) w.WriteLiteral(";8");
    if (sc.Attrs & StrikeThrough) w.WriteLiteral(";9");

    if (cm != EColorMode::None) {
        EColorKind fgKind = sc.FgKind;
        std::uint8_t fgIdx = sc.Fg[0];
        if (cm == EColorMode::Indexed256 && fgKind == EColorKind::TrueColor) {
            fgKind = EColorKind::Indexed;
            fgIdx = TrueColorTo256(sc.Fg[0], sc.Fg[1], sc.Fg[2]);
        }

        if (fgKind == EColorKind::Indexed) {
            if (fgIdx < 8) {
                w.WriteByte(';');
                w.WriteDecimal(30u + fgIdx);
            } else if (fgIdx < 16) {
                w.WriteByte(';');
                w.WriteDecimal(90u + (fgIdx - 8u));
            } else {
                w.WriteLiteral(";38;5;");
                w.WriteDecimal(fgIdx);
            }
        } else if (fgKind == EColorKind::TrueColor) {
            w.WriteLiteral(";38;2;");
            w.WriteDecimal(sc.Fg[0]);
            w.WriteByte(';');
            w.WriteDecimal(sc.Fg[1]);
            w.WriteByte(';');
            w.WriteDecimal(sc.Fg[2]);
        }

        EColorKind bgKind = sc.BgKind;
        std::uint8_t bgIdx = sc.Bg[0];
        if (cm == EColorMode::Indexed256 && bgKind == EColorKind::TrueColor) {
            bgKind = EColorKind::Indexed;
            bgIdx = TrueColorTo256(sc.Bg[0], sc.Bg[1], sc.Bg[2]);
        }

        if (bgKind == EColorKind::Indexed) {
            if (bgIdx < 8) {
                w.WriteByte(';');
                w.WriteDecimal(40u + bgIdx);
            } else if (bgIdx < 16) {
                w.WriteByte(';');
                w.WriteDecimal(100u + (bgIdx - 8u));
            } else {
                w.WriteLiteral(";48;5;");
                w.WriteDecimal(bgIdx);
            }
        } else if (bgKind == EColorKind::TrueColor) {
            w.WriteLiteral(";48;2;");
            w.WriteDecimal(sc.Bg[0]);
            w.WriteByte(';');
            w.WriteDecimal(sc.Bg[1]);
            w.WriteByte(';');
            w.WriteDecimal(sc.Bg[2]);
        }
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
//
// firstLine: index of first scrollback line to emit (0 = oldest).
// lineCount: number of lines to emit.
// ---------------------------------------------------------------------------

static void EncodeHistory(EscWriter& w, const Grid& g, Pen& pen,
                          std::uint32_t firstLine, std::uint32_t lineCount,
                          EColorMode cm) noexcept {
    const std::uint32_t end = firstLine + lineCount;
    for (std::uint32_t i = firstLine; i < end; ++i) {
        const Line* line = g.ScrollbackLine(i);
        if (!line) continue;

        const std::uint16_t used = line->CellsUsed;
        std::uint16_t col = 0;
        while (col < used) {
            const Cell& raw = line->Cells[col];
            // Skip wide continuation cells.
            if (raw.Width == 0) {
                ++col;
                continue;
            }

            const Cell nc = NormalizeCell(raw, cm);
            EmitSgr(w, pen, nc);
            char32_t cp = SanitizeCp(raw.Codepoint);
            w.WriteUtf8(cp);
            col += (raw.Width >= 2) ? 2 : 1;
        }

        // Reset pen at end of line for cleanliness.
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
//
// firstRow: first screen row to emit (rows 0..firstRow-1 are skipped).
// ---------------------------------------------------------------------------

static void EncodeScreen(EscWriter& w, const Grid& g, Pen& pen,
                         std::uint16_t firstRow, EColorMode cm) noexcept {
    const std::uint16_t rows = g.Rows();
    const std::uint16_t cols = g.Cols();

    for (std::uint16_t row = firstRow; row < rows; ++row) {
        const std::uint16_t used = ScreenCellsUsed(g, row);
        if (used == 0) continue;

        // Move to start of this row (1-based CUP).
        w.WriteLiteral("\x1b[");
        w.WriteDecimal(static_cast<std::uint32_t>(row + 1));
        w.WriteLiteral(";1H");

        std::uint16_t col = 0;
        while (col < used) {
            const Cell& raw = g.CellAt(row, col);
            if (raw.Width == 0) {
                ++col;
                continue;
            }

            const Cell nc = NormalizeCell(raw, cm);
            EmitSgr(w, pen, nc);
            char32_t cp = SanitizeCp(raw.Codepoint);
            w.WriteUtf8(cp);
            col += (raw.Width >= 2) ? static_cast<std::uint16_t>(2)
                                    : static_cast<std::uint16_t>(1);
        }

        (void)cols;
    }
}

// ---------------------------------------------------------------------------
// Encode mode-restore section (design.md §9).
// ---------------------------------------------------------------------------

static void EncodeModeRestore(EscWriter& w, const Grid& g,
                              EColorMode cm) noexcept {
    // Reset SGR pen and restore saved cursor colours (for DECSC/DECRC).
    w.WriteLiteral("\x1b[0m");

    const SavedCursor& sc =
        g.IsAltScreen() ? g.SavedCursorAltState() : g.SavedCursorMainState();
    EmitSgrFromSaved(w, sc, cm);

    // DECSC — save cursor (so DECRC restores the saved position).
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
    w.WriteByte(g.CharsetG0() == 0 ? 'B' : '0');
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
// DoEncode — core serializer, parameterised for the degrade ladder
// ---------------------------------------------------------------------------

struct EncodeResult {
    std::size_t Len{0};  ///< 0 means overflow
    std::size_t ScrollbackLen{0};
};

/// Serialise `grid` into `buf[0..cap)` using the given parameters.
///
/// Returns {0,0} on overflow (result exceeds cap).
/// histStart + histCount must be ≤ grid.ScrollbackSize().
static EncodeResult DoEncode(std::byte* buf, std::size_t cap, const Grid& grid,
                             std::uint32_t histStart, std::uint32_t histCount,
                             EColorMode cm,
                             std::uint16_t cropTopRows) noexcept {
    EscWriter w{buf, cap};
    Pen pen{};
    const bool altScreen = grid.IsAltScreen();

    // -----------------------------------------------------------------------
    // Preamble
    // -----------------------------------------------------------------------
    w.WriteByte('\x18');  // CAN
    // ESC backslash (ST — terminates any open DCS/OSC/APC):
    w.WriteLiteral("\x1b\\");

    if (altScreen) {
        w.WriteLiteral("\x1b[?1049h");
    } else {
        w.WriteLiteral("\x1b[?1049l");
    }
    w.WriteLiteral("\x1b[2J\x1b[H");

    // -----------------------------------------------------------------------
    // History section
    // -----------------------------------------------------------------------
    const std::size_t scrollbackStart = w.Pos;
    if (!altScreen && histCount > 0) {
        EncodeHistory(w, grid, pen, histStart, histCount, cm);
        for (std::uint16_t i = 0; i < grid.Rows(); ++i) w.WriteByte('\n');
    }
    const std::size_t scrollbackLen = w.Pos - scrollbackStart;

    // -----------------------------------------------------------------------
    // Screen section
    // -----------------------------------------------------------------------
    if (!altScreen) {
        w.WriteLiteral("\x1b[2J\x1b[H");
    }

    w.WriteLiteral("\x1b[?25l");
    w.WriteLiteral("\x1b[?2026h");

    pen = Pen{};
    EncodeScreen(w, grid, pen, cropTopRows, cm);
    EncodeModeRestore(w, grid, cm);

    if (w.Overflow) return {};

    return {w.Pos, scrollbackStart + scrollbackLen};
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

    const std::uint32_t sz = grid.ScrollbackSize();

    // -----------------------------------------------------------------------
    // Degrade ladder.  Each step stores its result in `best` and sets `deg`;
    // the first successful step breaks out.
    // -----------------------------------------------------------------------
    EncodeResult best{};
    EDegradation deg = EDegradation::None;

    // Step 0: full fidelity.
    best = DoEncode(buf, maxBytes, grid, 0, sz, EColorMode::Full, 0);
    if (best.Len > 0) {
        deg = EDegradation::None;
        goto build;
    }

    // Step 1: trim scrollback history by count (binary-search on max lines).
    if (sz > 0) {
        // Find the maximum N lines (from the newest end) that fit.
        std::uint32_t lo = 0, hi = sz;
        while (lo < hi) {
            const std::uint32_t mid = lo + (hi - lo + 1) / 2;
            if (DoEncode(buf, maxBytes, grid, sz - mid, mid, EColorMode::Full,
                         0)
                    .Len > 0)
                lo = mid;
            else
                hi = mid - 1;
        }
        // lo lines (newest lo lines) fit; use them (lo < sz because step 0
        // already tried sz lines).
        best = DoEncode(buf, maxBytes, grid, sz - lo, lo, EColorMode::Full, 0);
        if (best.Len > 0) {
            deg = EDegradation::HistoryTrimmed;
            goto build;
        }
        // Even 0 history lines with full colour doesn't fit; continue.
    }

    // Step 2: 256-colour (no history).
    best = DoEncode(buf, maxBytes, grid, sz, 0, EColorMode::Indexed256, 0);
    if (best.Len > 0) {
        deg = EDegradation::Colors256;
        goto build;
    }

    // Step 3: no colour (no history).
    best = DoEncode(buf, maxBytes, grid, sz, 0, EColorMode::None, 0);
    if (best.Len > 0) {
        deg = EDegradation::NoColor;
        goto build;
    }

    // Step 4: crop rows from the top (no colour, no history).
    // The cursor row is always kept; maximum crop = CursorRow().
    {
        const std::uint16_t maxCrop = grid.CursorRow();
        // Binary-search for minimum cropTopRows that fits.
        std::uint16_t lo = 0, hi = maxCrop;
        while (lo < hi) {
            const std::uint16_t mid =
                static_cast<std::uint16_t>(lo + (hi - lo) / 2);
            if (DoEncode(buf, maxBytes, grid, sz, 0, EColorMode::None, mid)
                    .Len > 0)
                hi = mid;
            else
                lo = static_cast<std::uint16_t>(mid + 1);
        }
        best = DoEncode(buf, maxBytes, grid, sz, 0, EColorMode::None, lo);
        if (best.Len > 0) {
            deg = EDegradation::RowsCropped;
            goto build;
        }
    }

    // All steps exhausted — cap is too small for a minimal snapshot.
    delete[] buf;
    return std::unexpected(EGridError::SnapshotTooLarge);

build:
    auto* snap = new (std::nothrow) Snapshot;
    if (!snap) {
        delete[] buf;
        return std::unexpected(EGridError::AllocationFailed);
    }

    snap->Buf_ = buf;
    snap->Len_ = best.Len;
    snap->ScrollbackLen_ = best.ScrollbackLen;
    snap->Degradation_ = deg;
    snap->PossiblyDiverged_ = grid.PossiblyDiverged();
    snap->AltScreen_ = grid.IsAltScreen();
    return snap;
}

}  // namespace zet::grid
