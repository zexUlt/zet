#pragma once

// The pieces of the snapshot encoder that other users of zet_grid share: the
// byte writer, the SGR pen and the encoder of a cell's SGR. The snapshot and
// the prediction overlay (zet_predict, design.md §9) write cells the same way.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "zet/grid/cell.hpp"

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
inline void EmitSgr(EscWriter& w, Pen& pen, const Cell& c) noexcept {
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

}  // namespace zet::grid
