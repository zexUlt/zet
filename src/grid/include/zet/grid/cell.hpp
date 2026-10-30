#pragma once

#include <cstdint>

namespace zet::grid {

/// Attribute flags packed into a uint8_t (bit positions).
enum EAttr : std::uint8_t {
    kAttrBold = 0x01,
    kAttrDim = 0x02,
    kAttrItalic = 0x04,
    kAttrUnderline = 0x08,
    kAttrBlink = 0x10,
    kAttrReverse = 0x20,
    kAttrInvisible = 0x40,
    kAttrStrikethrough = 0x80,
};

/// How a color field is interpreted.
enum class EColorKind : std::uint8_t {
    Default = 0,    ///< Terminal default (no SGR color).
    Indexed = 1,    ///< 0–255 palette index.
    TrueColor = 2,  ///< 24-bit RGB stored in an ExtCell entry.
};

/// A truecolor (R, G, B) tuple stored in the side-table.
struct ExtColor {
    std::uint8_t R{0};
    std::uint8_t G{0};
    std::uint8_t B{0};
};

/// One terminal cell.
///
/// Layout is kept to 16 bytes so a 220×50 grid (11 000 cells) fits in 176 KiB.
/// Cells with truecolor fg/bg rent slots from the Grid's ExtColor arena.
struct Cell {
    /// Unicode codepoint. 0 = empty/blank.
    char32_t Codepoint{0};

    /// Display width: 1 for most chars, 2 for East-Asian Wide/Fullwidth.
    /// The continuation cell of a wide glyph has width 0 and Codepoint 0.
    std::uint8_t Width{1};

    /// SGR attribute flags (EAttr bitmask).
    std::uint8_t Attrs{0};

    /// Foreground color kind.
    EColorKind FgKind{EColorKind::Default};

    /// Background color kind.
    EColorKind BgKind{EColorKind::Default};

    /// Foreground palette index (valid when FgKind == Indexed).
    std::uint8_t FgIdx{0};

    /// Background palette index (valid when BgKind == Indexed).
    std::uint8_t BgIdx{0};

    /// Index into the Grid's ExtColor arena for fg (valid when FgKind ==
    /// TrueColor).
    std::uint16_t FgExtIdx{0};

    /// Index into the Grid's ExtColor arena for bg (valid when BgKind ==
    /// TrueColor).
    std::uint16_t BgExtIdx{0};
};
static_assert(sizeof(Cell) == 16, "Cell must be 16 bytes");

}  // namespace zet::grid
