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
#include "zet/grid/unicode.hpp"

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
        if (Overflow) return;
        if (Pos < Cap)
            Buf[Pos++] = static_cast<std::byte>(b);
        else
            Overflow = true;
    }

    void WriteBytes(const char* s, std::size_t n) noexcept {
        if (Overflow) return;
        if (Pos + n <= Cap) {
            std::memcpy(Buf + Pos, s, n);
            Pos += n;
        } else {
            Overflow = true;
        }
    }

    void WriteLiteral(const char* s) noexcept {
        if (Overflow) return;
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
    static constexpr std::uint8_t CUBE_STEPS[6] = {0, 95, 135, 175, 215, 255};

    // Find nearest cube index for a single channel value.
    auto cubeIdx = [](std::uint8_t v) -> std::uint8_t {
        std::uint8_t best = 0;
        int bestDist = 255 * 255 + 1;
        for (int i = 0; i < 6; ++i) {
            int d = static_cast<int>(v) - static_cast<int>(CUBE_STEPS[i]);
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

    const int cr = CUBE_STEPS[ri];
    const int cg = CUBE_STEPS[gi];
    const int cb = CUBE_STEPS[bi];
    const int cubeDist =
        (cr - r) * (cr - r) + (cg - g) * (cg - g) + (cb - b) * (cb - b);

    // Grayscale ramp 232..255: value(i) = 8 + 10*i.
    const int gray = (static_cast<int>(r) + g + b) / 3;
    const int grayIdxRaw = (gray - 8 + 5) / 10;  // round to nearest
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

/// Emit complete SGR for a cell, split into two sequences to respect
/// MAX_PARAMS=16 (D9):
///   Seq 1: reset + text attrs + fg color  (max 15 params)
///   Seq 2: bg color + underline colour (D8) (max 8 params, only when needed)
/// The cell must already be normalised for the active colour mode.
/// Updates pen to the cell's state.
static void EmitSgr(EscWriter& w, Pen& pen, const Cell& c) noexcept {
    if (pen.MatchesCell(c)) return;

    // --- Sequence 1: reset + text attributes + fg color ---
    w.WriteLiteral("\x1b[0");

    if (c.Attrs & Bold) w.WriteLiteral(";1");
    if (c.Attrs & Dim) w.WriteLiteral(";2");
    if (c.Attrs & Italic) w.WriteLiteral(";3");
    if (c.Attrs & Underline) {
        if (c.AttrEx > 1) {
            // D8: extended underline style via colon sub-param (4:N)
            w.WriteLiteral(";4:");
            w.WriteDecimal(static_cast<std::uint32_t>(c.AttrEx));
        } else {
            w.WriteLiteral(";4");
        }
    }
    if (c.Attrs & Blink) w.WriteLiteral(";5");
    if (c.Attrs & Inverse) w.WriteLiteral(";7");
    if (c.Attrs & Invisible) w.WriteLiteral(";8");
    if (c.Attrs & StrikeThrough) w.WriteLiteral(";9");

    if (c.FgKind == EColorKind::Indexed) {
        const std::uint8_t idx = c.Fg[0];
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
    } else if (c.FgKind == EColorKind::TrueColor) {
        w.WriteLiteral(";38;2;");
        w.WriteDecimal(c.Fg[0]);
        w.WriteByte(';');
        w.WriteDecimal(c.Fg[1]);
        w.WriteByte(';');
        w.WriteDecimal(c.Fg[2]);
    }
    w.WriteByte('m');

    // --- Sequence 2: bg color + underline colour (D8) — only when needed ---
    if (c.BgKind != EColorKind::Default || c.UlIdx != 0) {
        w.WriteLiteral("\x1b[");
        bool first2 = true;
        auto sep2 = [&]() noexcept {
            if (!first2) w.WriteByte(';');
            first2 = false;
        };
        if (c.BgKind == EColorKind::Indexed) {
            sep2();
            const std::uint8_t idx = c.Bg[0];
            if (idx < 8) {
                w.WriteDecimal(40u + idx);
            } else if (idx < 16) {
                w.WriteDecimal(100u + (idx - 8u));
            } else {
                w.WriteLiteral("48;5;");
                w.WriteDecimal(idx);
            }
        } else if (c.BgKind == EColorKind::TrueColor) {
            sep2();
            w.WriteLiteral("48;2;");
            w.WriteDecimal(c.Bg[0]);
            w.WriteByte(';');
            w.WriteDecimal(c.Bg[1]);
            w.WriteByte(';');
            w.WriteDecimal(c.Bg[2]);
        }
        if (c.UlIdx != 0) {
            sep2();
            w.WriteLiteral("58;5;");
            w.WriteDecimal(static_cast<std::uint32_t>(c.UlIdx));
        }
        w.WriteByte('m');
    }

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
    if (sc.AttrEx != 0) {
        w.WriteLiteral(";4:");
        w.WriteDecimal(sc.AttrEx);
    } else if (sc.Attrs & Underline) {
        w.WriteLiteral(";4");
    }
    if (sc.Attrs & Blink) w.WriteLiteral(";5");
    if (sc.Attrs & Inverse) w.WriteLiteral(";7");
    if (sc.Attrs & Invisible) w.WriteLiteral(";8");
    if (sc.Attrs & StrikeThrough) w.WriteLiteral(";9");
    if (sc.UlIdx != 0) {
        w.WriteLiteral(";58;5;");
        w.WriteDecimal(sc.UlIdx);
    }

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

/// Emit a DECSET/DECRST private mode sequence (ESC [ ? Ps h/l).
static void EmitPrivateMode(EscWriter& w, std::uint16_t ps, bool on) noexcept {
    w.WriteLiteral("\x1b[?");
    w.WriteDecimal(ps);
    w.WriteByte(on ? 'h' : 'l');
}

/// Emit an ANSI SM/RM mode sequence (ESC [ Ps h/l — no '?'). (D6)
static void EmitAnsiMode(EscWriter& w, std::uint16_t ps, bool on) noexcept {
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(ps);
    w.WriteByte(on ? 'h' : 'l');
}

/// Build a Cell from the grid's LIVE pen state (D5: use live pen, not saved).
static Cell LivePenCell(const Grid& g) noexcept {
    Cell c{};
    c.Attrs = g.PenAttrs();
    c.FgKind = g.PenFgKind();
    c.BgKind = g.PenBgKind();
    c.Fg[0] = g.PenFg()[0];
    c.Fg[1] = g.PenFg()[1];
    c.Fg[2] = g.PenFg()[2];
    c.Bg[0] = g.PenBg()[0];
    c.Bg[1] = g.PenBg()[1];
    c.Bg[2] = g.PenBg()[2];
    c.AttrEx = g.PenAttrEx();
    c.UlIdx = g.PenUlIdx();
    return c;
}

/// Build a Cell from a SavedCursor (for DECSC pen setup).
static Cell SavedCursorCell(const SavedCursor& sc) noexcept {
    Cell c{};
    c.Attrs = sc.Attrs;
    c.FgKind = sc.FgKind;
    c.BgKind = sc.BgKind;
    c.Fg[0] = sc.Fg[0];
    c.Fg[1] = sc.Fg[1];
    c.Fg[2] = sc.Fg[2];
    c.Bg[0] = sc.Bg[0];
    c.Bg[1] = sc.Bg[1];
    c.Bg[2] = sc.Bg[2];
    c.AttrEx = sc.AttrEx;
    c.UlIdx = sc.UlIdx;
    return c;
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

        // Reset pen at end of line to prevent SGR bleeding.
        // Defect 6 fix: include AttrEx and UlIdx in the reset condition.
        if (pen.Attrs || pen.FgKind != EColorKind::Default ||
            pen.BgKind != EColorKind::Default || pen.AttrEx || pen.UlIdx) {
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
                         std::uint16_t firstRow, std::uint16_t lastRow,
                         EColorMode cm) noexcept {
    for (std::uint16_t row = firstRow; row < lastRow; ++row) {
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
    }
}

// Encode all main-screen cells explicitly (used when altScreen is active,
// to populate the target's main buffer before ?1049h — D1 requirement).
static void EncodeMainScreen(EscWriter& w, const Grid& g, Pen& pen,
                             EColorMode cm) noexcept {
    for (std::uint16_t row = 0; row < g.Rows(); ++row) {
        const std::uint16_t used = g.MainRowCellsUsed(row);
        if (used == 0) continue;

        w.WriteLiteral("\x1b[");
        w.WriteDecimal(static_cast<std::uint32_t>(row + 1));
        w.WriteLiteral(";1H");

        std::uint16_t col = 0;
        while (col < used) {
            const Cell& raw = g.MainCellAt(row, col);
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
    }
}

// ---------------------------------------------------------------------------
// Encode mode-restore section (design.md §9).
// ---------------------------------------------------------------------------

static void EncodeModeRestore(EscWriter& w, const Grid& g,
                              EColorMode cm) noexcept {
    const bool isAlt = g.IsAltScreen();
    const SavedCursor& sc =
        isAlt ? g.SavedCursorAltState() : g.SavedCursorMainState();

    // 1. Reset pen, then set saved-cursor pen (for ESC 7 capture).
    Pen pen{};
    w.WriteLiteral("\x1b[0m");
    EmitSgrFromSaved(w, sc, cm);

    // 2. CUP to saved cursor position (absolute; DECOM not yet enabled). (D3)
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(static_cast<std::uint32_t>(sc.Row + 1));
    w.WriteByte(';');
    w.WriteDecimal(static_cast<std::uint32_t>(sc.Col + 1));
    w.WriteByte('H');

    // 3. DECSC (ESC 7) — captures position + pen for future DECRC.
    w.WriteLiteral(
        "\x1b"
        "7");

    // 4. Reset pen, then set LIVE pen (D5: live pen, not saved cursor's pen).
    pen = Pen{};
    w.WriteLiteral("\x1b[0m");
    const Cell liveCell = LivePenCell(g);
    EmitSgr(w, pen, liveCell);

    // 5. DECSTBM — scroll region (1-based).
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(static_cast<std::uint32_t>(g.ScrollTop() + 1));
    w.WriteByte(';');
    w.WriteDecimal(static_cast<std::uint32_t>(g.ScrollBot() + 1));
    w.WriteByte('r');

    // 6. Modes (DECOM emitted last so final CUP is in the right coordinates).
    EmitPrivateMode(w, 1, g.ModeSet(EMode::Decckm));
    EmitPrivateMode(w, 7, g.ModeSet(EMode::Awm));
    EmitAnsiMode(w, 4, g.ModeSet(EMode::Irm));   // D6: IRM is ANSI SM/RM
    EmitAnsiMode(w, 20, g.ModeSet(EMode::Lnm));  // D6: LNM is ANSI SM/RM
    EmitPrivateMode(w, 1000, g.ModeSet(EMode::Mouse1000));
    EmitPrivateMode(w, 1002, g.ModeSet(EMode::Mouse1002));
    EmitPrivateMode(w, 1003, g.ModeSet(EMode::Mouse1003));
    EmitPrivateMode(w, 1005, g.ModeSet(EMode::Mouse1005));
    EmitPrivateMode(w, 1006, g.ModeSet(EMode::Mouse1006));
    EmitPrivateMode(w, 1015, g.ModeSet(EMode::Mouse1015));
    EmitPrivateMode(w, 2004, g.ModeSet(EMode::BracketedPaste));
    EmitPrivateMode(w, 1004, g.ModeSet(EMode::FocusEvents));

    // 7. Charset designations and active charset.
    w.WriteLiteral("\x1b(");
    w.WriteByte(g.CharsetG0() == 0 ? 'B' : '0');
    w.WriteLiteral("\x1b)");
    w.WriteByte(g.CharsetG1() == 0 ? 'B' : '0');
    w.WriteByte(g.CharsetActive() == 1 ? '\x0e' : '\x0f');

    // 8. Tab stops (D11): clear all, then re-set from grid state.
    //    Use HTS (ESC H) at each column, positioned via CUP to row 1.
    w.WriteLiteral("\x1b[3g");  // TBC 3 = clear all tab stops
    for (std::uint16_t col = 0; col < g.Cols(); ++col) {
        if (g.IsTabStop(col)) {
            w.WriteLiteral("\x1b[1;");
            w.WriteDecimal(static_cast<std::uint32_t>(col + 1));
            w.WriteByte('H');
            w.WriteLiteral(
                "\x1b"
                "H");  // ESC H = HTS
        }
    }

    // 9. DECSCUSR — cursor style (ESC [ Ps SP q).
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(g.CursorStyle());
    w.WriteLiteral(" q");

    // 10. Enable/disable DECOM BEFORE final CUP (D3: cursor set after DECOM).
    const bool decom = g.ModeSet(EMode::Decom);
    EmitPrivateMode(w, 6, decom);

    // 11. Final cursor position (D3: DECOM-relative if DECOM is on).
    const std::uint16_t curRow = g.CursorRow();
    const std::uint16_t curCol = g.CursorCol();
    std::uint32_t cupRow;
    if (decom) {
        cupRow = static_cast<std::uint32_t>(curRow - g.ScrollTop() + 1);
    } else {
        cupRow = static_cast<std::uint32_t>(curRow + 1);
    }
    // D4b: if PendingWrap is set and the cursor is on a wide-char continuation
    // cell (Width==0) at the last column, position the initial CUP at the lead
    // column (curCol-1) so step 12 can re-emit the lead char, advance the
    // cursor to curCol, and set PendingWrap.
    const bool wrapLeadReposition =
        g.PendingWrap() &&
        curCol + 1u == static_cast<std::uint32_t>(g.Cols()) && curCol > 0u &&
        g.CellAt(curRow, curCol).Width == 0;
    const std::uint16_t emitCol =
        wrapLeadReposition ? static_cast<std::uint16_t>(curCol - 1u) : curCol;
    const std::uint32_t cupCol = static_cast<std::uint32_t>(emitCol + 1);
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(cupRow);
    w.WriteByte(';');
    w.WriteDecimal(cupCol);
    w.WriteByte('H');

    // 12. Pending wrap (D4): re-emit cell at last column to trigger wrap state.
    // Defect 5 fix: PendingWrap can be true even with AWM off.  When AWM is
    // off, temporarily enable it so the re-printed char sets PendingWrap.
    if (g.PendingWrap() &&
        curCol + 1u == static_cast<std::uint32_t>(g.Cols())) {
        const bool awmWasOff = !g.ModeSet(EMode::Awm);
        if (awmWasOff)
            EmitPrivateMode(w, 7,
                            true);  // temp AWM on so re-print sets PendingWrap

        // Defect 4 fix: if a graphics charset is active the re-printed char
        // would be translated twice.  Temporarily switch to ASCII.
        const bool g0GraphicsActive =
            g.CharsetG0() != 0 && g.CharsetActive() == 0;
        const bool g1GraphicsActive =
            g.CharsetG1() != 0 && g.CharsetActive() != 0;
        if (g0GraphicsActive) w.WriteLiteral("\x1b(B");
        if (g1GraphicsActive) {
            w.WriteLiteral("\x1b)B");
            w.WriteByte('\x0f');
        }

        // D4b: emitCol already positions CUP at the wide-char lead if needed.
        const Cell& wc = g.CellAt(curRow, emitCol);
        EmitSgr(w, pen, wc);
        char32_t cp = SanitizeCp(wc.Codepoint);
        if (wc.Width == 1 && cp != U' ' && cp != U'?' && Wcwidth(cp) == 2)
            cp = U'\xFFFD';
        w.WriteUtf8(cp);

        // Defect 3 fix: after the re-print, restore the live pen because
        // EmitSgr set pen to wc's attrs, which may differ from the live pen.
        const Cell liveCell2 = LivePenCell(g);
        EmitSgr(w, pen, liveCell2);

        // Restore charset state if we temporarily switched to ASCII.
        if (g0GraphicsActive) w.WriteLiteral("\x1b(0");
        if (g1GraphicsActive) {
            w.WriteLiteral("\x1b)0");
            w.WriteByte('\x0e');
        }

        if (awmWasOff) EmitPrivateMode(w, 7, false);  // restore AWM off
    }

    // 13. DECTCEM — cursor visibility.
    EmitPrivateMode(w, 25, g.ModeSet(EMode::Dectcem));

    // 14. End sync-output.
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
/// cropTopRows rows are omitted from the top of the screen.
/// cropBotRows rows are omitted from the bottom of the screen.
static EncodeResult DoEncode(std::byte* buf, std::size_t cap, const Grid& grid,
                             std::uint32_t histStart, std::uint32_t histCount,
                             EColorMode cm, std::uint16_t cropTopRows,
                             std::uint16_t cropBotRows = 0) noexcept {
    EscWriter w{buf, cap};
    Pen pen{};
    const bool altScreen = grid.IsAltScreen();

    // -----------------------------------------------------------------------
    // Preamble
    // -----------------------------------------------------------------------
    w.WriteByte('\x18');  // CAN
    // ESC backslash (ST — terminates any open DCS/OSC/APC):
    w.WriteLiteral("\x1b\\");
    w.WriteLiteral("\x1b[!p");      // DECSTR soft-reset (D2)
    w.WriteLiteral("\x1b[?1049l");  // ensure main screen (always)
    w.WriteLiteral("\x1b[2J\x1b[H");
    w.WriteLiteral("\x1b[?25l");  // hide cursor (D13: with preamble)
    pen = Pen{};

    // -----------------------------------------------------------------------
    // History section (always, even when on alt screen)
    // -----------------------------------------------------------------------
    const std::size_t scrollbackStart = w.Pos;
    if (histCount > 0) {
        EncodeHistory(w, grid, pen, histStart, histCount, cm);
        for (std::uint16_t i = 0; i + 1 < grid.Rows(); ++i) w.WriteByte('\n');
    }
    const std::size_t scrollbackLen = w.Pos - scrollbackStart;

    // -----------------------------------------------------------------------
    // Main screen cells + optional alt-screen switch
    // -----------------------------------------------------------------------
    // D14: reset SGR pen before second clear so \x1b[2J always erases with the
    // default background.  EncodeHistory may leave a non-default pen active.
    w.WriteLiteral("\x1b[0m");
    w.WriteLiteral("\x1b[2J\x1b[H");  // second clear
    pen = Pen{};

    if (altScreen) {
        // Encode all main-screen cells so the target's main buffer is correct
        // when the user exits alt mode (D1 requirement).
        EncodeMainScreen(w, grid, pen, cm);

        // Defect 1 fix: ?1049h saves the current cursor into the terminal's
        // main-saved-cursor slot.  Position cursor at the source's main
        // SavedCursor before issuing ?1049h, so the terminal records the
        // correct saved position for the future ?1049l restore.
        const SavedCursor& msc = grid.SavedCursorMainState();
        const Cell mscCell = SavedCursorCell(msc);
        pen = Pen{};
        w.WriteLiteral("\x1b[0m");
        EmitSgr(w, pen, mscCell);
        w.WriteLiteral("\x1b[");
        w.WriteDecimal(static_cast<std::uint32_t>(msc.Row + 1));
        w.WriteByte(';');
        w.WriteDecimal(static_cast<std::uint32_t>(msc.Col + 1));
        w.WriteByte('H');
        w.WriteLiteral("\x1b[?1049h");  // enter alt screen (saves msc position)
        // D14: reset SGR before the alt-screen clear so \x1b[2J erases with
        // the default background rather than a stale colour from main encoding.
        w.WriteLiteral("\x1b[0m");
        w.WriteLiteral("\x1b[2J\x1b[H");
        pen = Pen{};
    }

    // -----------------------------------------------------------------------
    // Screen section (sync window wraps visible screen)
    // -----------------------------------------------------------------------
    w.WriteLiteral("\x1b[?2026h");

    pen = Pen{};
    const std::uint16_t lastRow = static_cast<std::uint16_t>(
        grid.Rows() > cropBotRows ? grid.Rows() - cropBotRows : 0u);
    EncodeScreen(w, grid, pen, cropTopRows, lastRow, cm);
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

    // Step 4: crop rows (no colour, no history).
    // Phase A: crop from the top, keeping the cursor row and everything below.
    //          Maximum top crop = CursorRow() (cursor row is preserved).
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

    // Phase B: with all rows above the cursor cropped, also crop rows below it.
    //          Maximum bottom crop = Rows - CursorRow - 1 (cursor row kept).
    {
        const std::uint16_t curRow = grid.CursorRow();
        const std::uint16_t topCrop = curRow;  // maximum from Phase A
        const std::uint16_t rows = grid.Rows();
        // There must be at least one row below the cursor to crop.
        if (rows > static_cast<std::uint16_t>(curRow + 1u)) {
            const std::uint16_t maxBotCrop =
                static_cast<std::uint16_t>(rows - curRow - 1u);
            std::uint16_t lo = 1, hi = maxBotCrop;
            while (lo < hi) {
                const std::uint16_t mid =
                    static_cast<std::uint16_t>(lo + (hi - lo) / 2);
                if (DoEncode(buf, maxBytes, grid, sz, 0, EColorMode::None,
                             topCrop, mid)
                        .Len > 0)
                    hi = mid;
                else
                    lo = static_cast<std::uint16_t>(mid + 1);
            }
            best = DoEncode(buf, maxBytes, grid, sz, 0, EColorMode::None,
                            topCrop, lo);
            if (best.Len > 0) {
                deg = EDegradation::RowsCropped;
                goto build;
            }
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
