#pragma once

#include <cstdint>

namespace zet::grid {

/// Attribute flags packed into a uint8_t (bit positions).
enum EAttr : std::uint8_t {
    Bold = 0x01,
    Dim = 0x02,
    Italic = 0x04,
    Underline = 0x08,
    Blink = 0x10,
    Inverse = 0x20,
    Invisible = 0x40,
    StrikeThrough = 0x80,
};

/// How a color field is interpreted.
enum class EColorKind : std::uint8_t {
    Default = 0,    ///< Terminal default (no SGR color).
    Indexed = 1,    ///< 0–255 palette index; stored in Fg[0] / Bg[0].
    TrueColor = 2,  ///< 24-bit RGB stored inline in Fg[0..2] / Bg[0..2].
};

/// One terminal cell — 16 bytes, truecolor stored inline.
///
/// Layout (no padding):
///   char32_t Codepoint  4 bytes  (offset  0)
///   uint8_t  Width      1 byte   (offset  4)
///   uint8_t  Attrs      1 byte   (offset  5)
///   EColorKind FgKind   1 byte   (offset  6)
///   EColorKind BgKind   1 byte   (offset  7)
///   uint8_t  Fg[3]      3 bytes  (offset  8)  R/palette-idx, G, B
///   uint8_t  Bg[3]      3 bytes  (offset 11)  R/palette-idx, G, B
///   uint8_t  AttrEx     1 byte   (offset 14)  underline style (0–5)
///   uint8_t  UlIdx      1 byte   (offset 15)  underline colour palette index
struct Cell {
    /// Unicode codepoint. 0 = empty/blank.
    char32_t Codepoint{0};

    /// Display width: 1 for most chars, 2 for East-Asian Wide/Fullwidth.
    /// The continuation cell of a wide glyph has Width 0 and Codepoint 0.
    std::uint8_t Width{1};

    /// SGR attribute flags (EAttr bitmask).
    std::uint8_t Attrs{0};

    /// Foreground color kind.
    EColorKind FgKind{EColorKind::Default};

    /// Background color kind.
    EColorKind BgKind{EColorKind::Default};

    /// Foreground color bytes.
    ///   Indexed:   Fg[0] = palette index (0–255), Fg[1]/Fg[2] unused.
    ///   TrueColor: Fg[0]=R, Fg[1]=G, Fg[2]=B.
    std::uint8_t Fg[3]{};

    /// Background color bytes (same layout as Fg).
    std::uint8_t Bg[3]{};

    /// Extended underline style (SGR 4:n):
    ///   0=off, 1=single, 2=double, 3=curly, 4=dotted, 5=dashed.
    std::uint8_t AttrEx{0};

    /// Underline colour palette index (SGR 58:5:n; 0 when unset).
    std::uint8_t UlIdx{0};
};
static_assert(sizeof(Cell) == 16, "Cell must be 16 bytes");

}  // namespace zet::grid
