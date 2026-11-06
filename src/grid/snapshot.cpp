// Snapshot serializer for zet_grid (design.md §9).
//
// Produces a byte sequence that — when fed to a fresh VtParser+Grid of the
// same dimensions — reproduces the visible screen, history, cursor state and
// modes.  No dynamic allocation after the initial buffer; no exceptions; no
// abort, exit or terminate (nm-gate clean).
//
// Two-pass encoding (D12): dry-run first to count bytes, then allocate
// exactly that many bytes and write for real.  Never pins SNAPSHOT_MAX_BYTES.
//
// Degrade ladder (design.md §9), applied in order until the dry-run size
// fits the cap.  At full colour with nothing trimmed the output is the
// unchanged full-fidelity encoding.
//   1. Trim scrollback history by line count (binary search on count).
//   2. Downgrade TrueColor to the nearest xterm-256 palette colour.
//   3. Strip all foreground/background colour.
//   4. Crop screen rows from the top, then from the bottom; the cursor row
//      is always kept.

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
// EscWriter — write-ahead buffer; DryRun mode counts bytes without writing.
// ---------------------------------------------------------------------------

struct EscWriter {
    std::byte* Buf;
    std::size_t Cap;
    std::size_t Pos{0};
    bool Overflow{false};
    bool DryRun{false};

    void WriteByte(std::uint8_t b) noexcept {
        if (DryRun) {
            ++Pos;
            return;
        }
        if (Pos < Cap)
            Buf[Pos++] = static_cast<std::byte>(b);
        else
            Overflow = true;
    }

    void WriteBytes(const char* s, std::size_t n) noexcept {
        if (DryRun) {
            Pos += n;
            return;
        }
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
    if (cp == 0) return U' ';                       // blank cell → space
    if (cp < 0x20) return U'?';                     // C0 controls
    if (cp == 0x7F) return U'?';                    // DEL
    if (cp >= 0x80 && cp <= 0x9F) return U'?';      // C1 controls
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

// ---------------------------------------------------------------------------
// Pen state — tracks the "current SGR pen" while serializing.
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

// ---------------------------------------------------------------------------
// Encode history section (oldest→newest scrollback lines).
// ---------------------------------------------------------------------------

static void EncodeHistory(EscWriter& w, const Grid& g, Pen& pen,
                          std::uint32_t firstLine, std::uint32_t lineCount,
                          EColorMode cm) noexcept {
    const std::uint32_t end = firstLine + lineCount;
    for (std::uint32_t i = firstLine; i < end; ++i) {
        const Line* line = g.ScrollbackLine(i);
        if (!line) continue;

        // The extent comes from the cells, as for the screen.
        std::uint16_t used = g.Cols();
        while (used > 0 &&
               IsBlankCell(line->Cells[static_cast<std::size_t>(used) - 1]))
            --used;
        std::uint16_t col = 0;
        while (col < used) {
            const Cell& c = line->Cells[col];
            if (c.Width == 0) {
                ++col;
                continue;
            }  // skip continuation

            EmitSgr(w, pen, NormalizeCell(c, cm));
            char32_t cp = SanitizeCp(c.Codepoint);
            // D7: orphan head — valid wide char stored in a Width=1 slot.
            if (c.Width == 1 && cp != U' ' && cp != U'?' && Wcwidth(cp) == 2)
                cp = U'\xFFFD';
            w.WriteUtf8(cp);
            col += (c.Width >= 2) ? 2u : 1u;
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
// Encode screen section.
//
// CellAtFn / ExtentFn are used to read either main or alt screen cells
// without changing this function (D1).
//
// Adjacent non-blank rows are separated with CR+LF instead of per-row CUP
// (§9: minimal motion).
// ---------------------------------------------------------------------------

using CellAtFn = const Cell& (*)(const Grid&, std::uint16_t, std::uint16_t);
using ExtentFn = std::uint16_t (*)(const Grid&, std::uint16_t);

static const Cell& MainCellAtFn(const Grid& g, std::uint16_t r,
                                std::uint16_t c) noexcept {
    return g.MainCellAt(r, c);
}
static std::uint16_t MainExtentFn(const Grid& g, std::uint16_t r) noexcept {
    // Scan from right to find used boundary (consistent with alt screen path).
    std::uint16_t used = g.Cols();
    while (used > 0 && IsBlankCell(MainCellAtFn(
                           g, r, static_cast<std::uint16_t>(used - 1))))
        --used;
    return used;
}
static const Cell& AltCellAtFn(const Grid& g, std::uint16_t r,
                               std::uint16_t c) noexcept {
    return g.CellAt(r,
                    c);  // CellAt reads active (alt) screen when in alt mode.
}
static std::uint16_t AltExtentFn(const Grid& g, std::uint16_t r) noexcept {
    std::uint16_t used = g.Cols();
    while (used > 0 &&
           IsBlankCell(AltCellAtFn(g, r, static_cast<std::uint16_t>(used - 1))))
        --used;
    return used;
}

static void EncodeAnyScreen(EscWriter& w, const Grid& g, Pen& pen,
                            CellAtFn cellAt, ExtentFn extentOf,
                            std::uint16_t firstRow, std::uint16_t endRow,
                            EColorMode cm) noexcept {
    std::int32_t lastRow = -1;  // -1 = none emitted yet

    for (std::uint16_t row = firstRow; row < endRow; ++row) {
        const std::uint16_t used = extentOf(g, row);
        if (used == 0) continue;

        if (lastRow >= 0 && row == static_cast<std::uint16_t>(lastRow + 1)) {
            // Adjacent to previous emitted row — use CR+LF (cheaper than CUP).
            w.WriteLiteral("\r\n");
        } else {
            // Non-adjacent — absolute CUP (1-based, DECOM is off during paint).
            w.WriteLiteral("\x1b[");
            w.WriteDecimal(static_cast<std::uint32_t>(row + 1));
            w.WriteLiteral(";1H");
        }
        lastRow = static_cast<std::int32_t>(row);

        std::uint16_t col = 0;
        while (col < used) {
            const Cell& c = cellAt(g, row, col);
            if (c.Width == 0) {
                ++col;
                continue;
            }  // skip continuation

            EmitSgr(w, pen, NormalizeCell(c, cm));
            char32_t cp = SanitizeCp(c.Codepoint);
            // D7: orphan head — valid wide char stored in a Width=1 slot.
            if (c.Width == 1 && cp != U' ' && cp != U'?' && Wcwidth(cp) == 2)
                cp = U'\xFFFD';
            w.WriteUtf8(cp);
            col += (c.Width >= 2) ? static_cast<std::uint16_t>(2u)
                                  : static_cast<std::uint16_t>(1u);
        }
    }
}

// ---------------------------------------------------------------------------
// Mode restore section (design.md §9).
// ---------------------------------------------------------------------------

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

/// True when a saved cursor carries DECOM or a non-default charset state, so
/// the terminal's own DECSC would capture something the default state lacks.
static bool SlotNeedsState(const SavedCursor& sc) noexcept {
    return sc.Decom || sc.CharsetG0 != 0 || sc.CharsetG1 != 0 ||
           sc.CharsetActive != 0;
}

/// Put the terminal into the saved slot's DECOM and charset state, then CUP
/// to the saved position.  The caller issues DECSC / ?1049h next and then
/// undoes the state with SlotUndoState.  The saved wrap flag is not
/// reproduced (design.md §9): it would need a re-print in the last column.
static void SlotSetState(EscWriter& w, const Grid& g,
                         const SavedCursor& sc) noexcept {
    w.WriteLiteral("\x1b(");
    w.WriteByte(sc.CharsetG0 == 0 ? 'B' : '0');
    w.WriteLiteral("\x1b)");
    w.WriteByte(sc.CharsetG1 == 0 ? 'B' : '0');
    w.WriteByte(sc.CharsetActive == 1 ? '\x0e' : '\x0f');
    std::uint32_t row = static_cast<std::uint32_t>(sc.Row + 1);
    if (sc.Decom) {
        // Region first (DECSTBM homes the cursor), then DECOM: CUP is then
        // relative to the region, so a row outside it is clamped into it.
        w.WriteLiteral("\x1b[");
        w.WriteDecimal(static_cast<std::uint32_t>(g.ScrollTop() + 1));
        w.WriteByte(';');
        w.WriteDecimal(static_cast<std::uint32_t>(g.ScrollBot() + 1));
        w.WriteByte('r');
        w.WriteLiteral("\x1b[?6h");
        const std::uint16_t in = sc.Row < g.ScrollTop()   ? g.ScrollTop()
                                 : sc.Row > g.ScrollBot() ? g.ScrollBot()
                                                          : sc.Row;
        row = static_cast<std::uint32_t>(in - g.ScrollTop() + 1);
    }
    w.WriteLiteral("\x1b[");
    w.WriteDecimal(row);
    w.WriteByte(';');
    w.WriteDecimal(static_cast<std::uint32_t>(sc.Col + 1));
    w.WriteByte('H');
}

/// Back to DECOM off, the full-screen region and default charsets, so the
/// absolute-CUP paint and the cell text that follow are not offset, scrolled
/// or translated.  Step 5 of the mode restore sets the real region again.
static void SlotUndoState(EscWriter& w) noexcept {
    w.WriteLiteral("\x1b[?6l\x1b[r\x1b(B\x1b)B\x0f");
}

static void EncodeModeRestore(EscWriter& w, const Grid& g, Pen& pen,
                              EColorMode cm) noexcept {
    const bool isAlt = g.IsAltScreen();
    const SavedCursor& sc =
        isAlt ? g.SavedCursorAltState() : g.SavedCursorMainState();

    // 1. Reset pen, then set saved-cursor pen (for ESC 7 capture).
    pen = Pen{};
    w.WriteLiteral("\x1b[0m");
    const Cell scCell = NormalizeCell(SavedCursorCell(sc), cm);
    EmitSgr(w, pen, scCell);

    // 2. CUP to saved cursor position (absolute unless the slot saved DECOM,
    //    then the state is put in place first). (D3)
    const bool slotState = SlotNeedsState(sc);
    if (slotState) {
        SlotSetState(w, g, sc);
    } else {
        w.WriteLiteral("\x1b[");
        w.WriteDecimal(static_cast<std::uint32_t>(sc.Row + 1));
        w.WriteByte(';');
        w.WriteDecimal(static_cast<std::uint32_t>(sc.Col + 1));
        w.WriteByte('H');
    }

    // 3. DECSC (ESC 7) — captures position + pen + DECOM + charsets.
    w.WriteLiteral(
        "\x1b"
        "7");
    if (slotState) SlotUndoState(w);

    // 4. Reset pen, then set LIVE pen (D5: live pen, not saved cursor's pen).
    pen = Pen{};
    w.WriteLiteral("\x1b[0m");
    const Cell liveCell = NormalizeCell(LivePenCell(g), cm);
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
        // CUP is region-relative when DECOM is on: row 1 = scroll_top. A
        // cursor outside the region has no such coordinate (the subtraction
        // would wrap), so it is clamped into the region.
        const std::uint16_t inRegion = curRow < g.ScrollTop()   ? g.ScrollTop()
                                       : curRow > g.ScrollBot() ? g.ScrollBot()
                                                                : curRow;
        cupRow = static_cast<std::uint32_t>(inRegion - g.ScrollTop() + 1);
    } else {
        cupRow = static_cast<std::uint32_t>(curRow + 1);
    }

    // D4b: if PendingWrap is set and the cursor is on a wide-char continuation
    // cell (Width==0) at the last column, position the initial CUP at the lead
    // column (curCol-1) so step 12 can re-emit the lead char, advance the
    // cursor to curCol, and set PendingWrap.  Writing directly into the
    // continuation cell would trigger the left-neighbour check and break the
    // wide pair.  This applies whether AWM is currently on or off (AWM-off is
    // handled by the awmWasOff temp-enable below).
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
        EmitSgr(w, pen, NormalizeCell(wc, cm));
        char32_t cp = SanitizeCp(wc.Codepoint);
        if (wc.Width == 1 && cp != U' ' && cp != U'?' && Wcwidth(cp) == 2)
            cp = U'\xFFFD';
        w.WriteUtf8(cp);

        // Defect 3 fix: after the re-print, restore the live pen because
        // EmitSgr set pen to wc's attrs, which may differ from the live pen.
        const Cell liveCell2 = NormalizeCell(LivePenCell(g), cm);
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

    // 14. End sync-output (one window around screen paint, D13).
    w.WriteLiteral("\x1b[?2026l");
}

// ---------------------------------------------------------------------------
// EncodeAll — single pass (used for both dry-run and real write).
// ---------------------------------------------------------------------------

/// Parameters of one ladder step.  The default value is the full-fidelity
/// encoding.
struct EncodeParams {
    std::uint32_t HistStart{0};  ///< first scrollback line to emit
    std::uint32_t HistCount{0};  ///< number of scrollback lines to emit
    EColorMode Color{EColorMode::Full};
    std::uint16_t CropTop{0};  ///< screen rows omitted from the top
    std::uint16_t CropBot{0};  ///< screen rows omitted from the bottom
};

static void EncodeAll(EscWriter& w, const Grid& grid, const EncodeParams& ep,
                      std::size_t& outScrollbackLen) noexcept {
    const EColorMode cm = ep.Color;
    const std::uint16_t firstRow = ep.CropTop;
    const std::uint16_t endRow = static_cast<std::uint16_t>(
        grid.Rows() > ep.CropBot ? grid.Rows() - ep.CropBot : 0u);
    const bool altScreen = grid.IsAltScreen();
    Pen pen{};

    // -----------------------------------------------------------------------
    // Preamble
    // -----------------------------------------------------------------------
    w.WriteByte('\x18');        // CAN — aborts in-flight DCS/OSC/APC
    w.WriteLiteral("\x1b\\");   // ESC \ = ST
    w.WriteLiteral("\x1b[!p");  // DECSTR soft-reset (D2: clean stale SGR/modes)
    w.WriteLiteral("\x1b[?1049l");    // ensure main screen
    w.WriteLiteral("\x1b[2J\x1b[H");  // erase + home
    w.WriteLiteral("\x1b[?25l");      // hide cursor (D13: with preamble)

    // -----------------------------------------------------------------------
    // History section (main-screen scrollback).
    // -----------------------------------------------------------------------
    if (ep.HistCount > 0) {
        EncodeHistory(w, grid, pen, ep.HistStart, ep.HistCount, cm);
        // Push history lines into target scrollback via Rows-1 LFs (D10).
        for (std::uint16_t i = 0; i + 1 < grid.Rows(); ++i) w.WriteByte('\n');
    }

    // -----------------------------------------------------------------------
    // Clean slate for the screen paint.
    //
    // The paint that follows (the main screen when the alt screen is active,
    // the sync-window paint otherwise) writes only the cells it needs, so it
    // requires a cleared screen with the default pen.  In the grid's model the
    // Rows-1 LFs already scroll every history line off the screen and
    // EncodeHistory ends each line with a default pen; the clear and the reset
    // are defensive, for a target that is taller than the grid or holds
    // something the model does not see.  D14: reset SGR before the clear so
    // \x1b[2J erases with the default background.  Needed in both cases.
    // -----------------------------------------------------------------------
    w.WriteLiteral("\x1b[0m");
    w.WriteLiteral("\x1b[2J\x1b[H");  // second clear
    pen = Pen{};

    // -----------------------------------------------------------------------
    // Main screen cells (outside sync window), alt screen only.
    //
    // D1: when the alt screen is active, the main buffer must hold the main
    // screen before ?1049h, so the main screen and history survive the
    // snapshot and are there after ?1049l.  Without the alt screen this paint
    // would be redundant: the sync-window paint below rebuilds the whole
    // screen, so painting it here as well would only double its bytes (a full
    // screen, ~423 KB at 220x50 truecolor), push the degrade ladder one step
    // further, and on terminals that save the screen to scrollback on ED 2
    // leave a duplicate there.
    // -----------------------------------------------------------------------
    if (altScreen) {
        EncodeAnyScreen(w, grid, pen, MainCellAtFn, MainExtentFn, firstRow,
                        endRow, cm);
    }

    // -----------------------------------------------------------------------
    // Alt screen switch (D1: enter alt only after main screen is done).
    // -----------------------------------------------------------------------
    if (altScreen) {
        // Defect 1 fix: ?1049h saves the current cursor into the terminal's
        // main-saved-cursor slot.  Position cursor at the source's main
        // SavedCursor before issuing ?1049h, so the terminal records the
        // correct saved position for the future ?1049l restore.
        const SavedCursor& msc = grid.SavedCursorMainState();
        const Cell mscCell = NormalizeCell(SavedCursorCell(msc), cm);
        pen = Pen{};
        w.WriteLiteral("\x1b[0m");
        EmitSgr(w, pen, mscCell);
        const bool mscState = SlotNeedsState(msc);
        if (mscState) {
            SlotSetState(w, grid, msc);
        } else {
            w.WriteLiteral("\x1b[");
            w.WriteDecimal(static_cast<std::uint32_t>(msc.Row + 1));
            w.WriteByte(';');
            w.WriteDecimal(static_cast<std::uint32_t>(msc.Col + 1));
            w.WriteByte('H');
        }

        w.WriteLiteral("\x1b[?1049h");  // enter alt screen (saves msc position)
        if (mscState) SlotUndoState(w);
        // D14: reset SGR before the alt-screen clear.  Main-screen encoding
        // may leave a non-default pen active; without a reset, \x1b[2J would
        // erase the entire alt screen with that background colour.
        w.WriteLiteral("\x1b[0m");
        w.WriteLiteral("\x1b[2J\x1b[H");  // clear alt with default bg
        pen = Pen{};
    }

    // scrollbackLen = byte offset of the sync window start (screen section).
    // The degrade ladder (PR2) uses this to know where history ends.
    outScrollbackLen = w.Pos;

    // -----------------------------------------------------------------------
    // Screen paint (D13: one sync window, wraps only visible screen).
    // -----------------------------------------------------------------------
    w.WriteLiteral("\x1b[?2026h");
    EncodeAnyScreen(w, grid, pen, altScreen ? AltCellAtFn : MainCellAtFn,
                    altScreen ? AltExtentFn : MainExtentFn, firstRow, endRow,
                    cm);

    // Mode restore + sync end (EncodeModeRestore ends with ?2026l).
    EncodeModeRestore(w, grid, pen, cm);
}

// ---------------------------------------------------------------------------
// Snapshot — public API
// ---------------------------------------------------------------------------

Snapshot::~Snapshot() noexcept { delete[] Buf_; }

std::span<const std::byte> Snapshot::Bytes() const noexcept {
    return {Buf_, Len_};
}

/// Byte size of the encoding for `ep` (dry run, nothing is written).
static std::size_t MeasureEncoding(const Grid& grid, const EncodeParams& ep,
                                   std::size_t maxBytes) noexcept {
    std::byte dummy;
    EscWriter dry{&dummy, maxBytes, 0, false, /*DryRun=*/true};
    std::size_t scrollbackLen = 0;
    EncodeAll(dry, grid, ep, scrollbackLen);
    return dry.Pos;
}

GridResult<Snapshot*> Snapshot::Create(const Grid& grid,
                                       SnapshotOptions opts) noexcept {
    std::size_t maxBytes = opts.MaxBytes;
    if (maxBytes == 0 || maxBytes > SNAPSHOT_MAX_BYTES)
        maxBytes = SNAPSHOT_MAX_BYTES;

    const std::uint32_t sz = grid.ScrollbackSize();
    auto fits = [&](const EncodeParams& ep) noexcept {
        return MeasureEncoding(grid, ep, maxBytes) <= maxBytes;
    };

    // Degrade ladder: the first step whose dry-run fits the cap is encoded.
    EncodeParams chosen{.HistStart = 0, .HistCount = sz};
    EDegradation deg = EDegradation::None;
    bool found = fits(chosen);

    // Step 1: trim history by count, keeping the newest lines.
    if (!found && sz > 0) {
        std::uint32_t lo = 0, hi = sz - 1;  // sz lines did not fit
        while (lo < hi) {
            const std::uint32_t mid = lo + (hi - lo + 1) / 2;
            if (fits({.HistStart = sz - mid, .HistCount = mid}))
                lo = mid;
            else
                hi = mid - 1;
        }
        chosen = {.HistStart = sz - lo, .HistCount = lo};
        found = fits(chosen);  // lo == 0 can still be too large
        if (found) deg = EDegradation::HistoryTrimmed;
    }

    // Steps 2 and 3: no history, then reduced colour.
    if (!found) {
        chosen = {
            .HistStart = sz, .HistCount = 0, .Color = EColorMode::Indexed256};
        found = fits(chosen);
        if (found) deg = EDegradation::Colors256;
    }
    if (!found) {
        chosen.Color = EColorMode::None;
        found = fits(chosen);
        if (found) deg = EDegradation::NoColor;
    }

    // Step 4: crop rows.  Phase A crops from the top (at most CursorRow rows,
    // so the cursor row stays); phase B then crops from the bottom.
    if (!found) {
        const std::uint16_t curRow = grid.CursorRow();
        std::uint16_t lo = 0, hi = curRow;
        while (lo < hi) {
            const std::uint16_t mid =
                static_cast<std::uint16_t>(lo + (hi - lo) / 2);
            chosen.CropTop = mid;
            if (fits(chosen))
                hi = mid;
            else
                lo = static_cast<std::uint16_t>(mid + 1);
        }
        chosen.CropTop = lo;
        found = fits(chosen);

        if (!found && grid.Rows() > static_cast<std::uint16_t>(curRow + 1u)) {
            chosen.CropTop = curRow;
            std::uint16_t blo = 1, bhi = static_cast<std::uint16_t>(
                                       grid.Rows() - curRow - 1u);
            while (blo < bhi) {
                const std::uint16_t mid =
                    static_cast<std::uint16_t>(blo + (bhi - blo) / 2);
                chosen.CropBot = mid;
                if (fits(chosen))
                    bhi = mid;
                else
                    blo = static_cast<std::uint16_t>(mid + 1);
            }
            chosen.CropBot = blo;
            found = fits(chosen);
        }
        if (found) deg = EDegradation::RowsCropped;
    }

    // Even the minimal snapshot exceeds the cap.
    if (!found) return std::unexpected(EGridError::SnapshotTooLarge);

    // D12: allocate exactly the measured size, then write for real.
    const std::size_t totalSize = MeasureEncoding(grid, chosen, maxBytes);
    auto* buf = new (std::nothrow) std::byte[totalSize];
    if (!buf) return std::unexpected(EGridError::AllocationFailed);

    // Real write pass — totalSize was measured so overflow cannot occur.
    EscWriter w{buf, totalSize};
    std::size_t scrollbackLen = 0;
    EncodeAll(w, grid, chosen, scrollbackLen);

    auto* snap = new (std::nothrow) Snapshot;
    if (!snap) {
        delete[] buf;
        return std::unexpected(EGridError::AllocationFailed);
    }

    snap->Buf_ = buf;
    snap->Len_ = w.Pos;
    snap->ScrollbackLen_ = scrollbackLen;
    snap->Degradation_ = deg;
    snap->PossiblyDiverged_ = grid.PossiblyDiverged();
    snap->AltScreen_ = grid.IsAltScreen();
    return snap;
}

}  // namespace zet::grid
