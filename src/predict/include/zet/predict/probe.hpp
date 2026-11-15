#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "zet/core/bytes.hpp"
#include "zet/grid/grid.hpp"
#include "zet/predict/engine.hpp"
#include "zet/predict/limits.hpp"
#include "zet/predict/overlay.hpp"

namespace zet::predict {

/// The most bytes one probe write is: the overlay's frame, the removal of up
/// to 2 * PROBE_GLYPHS_MAX cells (each its own CHA, SGR and ECH, as in the
/// overlay's restore), the glyphs of four bytes, the query and the glyph the
/// register gets back, with room for the hidden pen.
inline constexpr std::size_t PROBE_BYTES_MAX =
    OVERLAY_FRAME_BYTES_MAX + 2 * PROBE_GLYPHS_MAX * OVERLAY_CELL_BYTES_MAX +
    (PROBE_GLYPHS_MAX + 1) * 4 + 7 + 64;

/// Why a point is not a clean point for a width probe (design.md §9, "Проба
/// ширины", *Когда*), the first condition that fails, in this order. The ones
/// that concern the demultiplexer (own record, reasons of a query, a silent
/// terminal) are not here: the mirror does not know them.
enum class EProbeGate : std::uint8_t {
    Clean = 0,       ///< a probe can be written
    ClassNotProbed,  ///< the class has no probe (only Cyrillic and private use)
    NoMirror,        ///< the engine is in `never`: there is no mirror
    NotGround,       ///< the parser is inside a sequence or a UTF-8 character
    AltScreen,       ///< alternate screen
    PendingWrap,     ///< deferred wrap
    Decom,           ///< origin mode
    Irm,             ///< insert mode
    Charset,         ///< G0 is not ASCII, or SO is active
    SyncFrame,       ///< the program's own synchronised update is open
    UnmodelledMode,  ///< DECLRMM, `?45`, `?3`, VT52, not UTF-8
    UnmodelledSgr,   ///< an SGR attribute the pen cannot hold
    RowsUntrusted,   ///< the rows of the mirror are not the terminal's
    ColumnUntrusted,  ///< the column of the mirror is not the terminal's
    RowMark,          ///< a width doubt mark anywhere in the cursor row
    LeftOfCursor,     ///< left of the cursor is not empty or printable ASCII
    PathNotAscii,     ///< not only ASCII since the column was set absolutely
    ProbeForeign,     ///< a cell of the probe is not known to the mirror
    ProbeOccupied,    ///< a cell of the probe holds something, a space too
    RightEdge,        ///< the probe or the cursor after it reaches the last
                      ///< column
    NoLastGlyph,      ///< the register REP reads is unknown, or lost a mark
};

/// A probe written to the bytes buffer: what the client queues and what it
/// needs to read the answer.
struct WidthProbeWrite {
    /// The whole write, valid until the next call of the builder.
    ByteSpan Bytes;
    /// Where in Bytes the query `CSI 6n CSI c` (Calibration::QUERY_SIZE bytes)
    /// begins: the client marks it in the stdout queue.
    std::size_t QueryOffset{0};
    /// The probe's row and column in the mirror, counted from 0: the cursor
    /// the probe starts from, where the answer's row is compared and where the
    /// resync of a row mismatch puts the cursor. The answer is the sum of the
    /// glyph widths added to the column.
    std::uint16_t Row{0};
    std::uint16_t Col{0};
    /// The number of glyphs: the narrow answer is Col + Glyphs.
    std::uint8_t Glyphs{0};
};

/// The width probe's bytes (design.md §9, "Проба ширины"): glyphs of a class
/// printed hidden into the empty known cells of the cursor row, the cursor
/// position asked for, and the same write puts everything back from the
/// mirror. The probe is an overlay write with a query, and it takes the
/// overlay's pieces: hiding the cursor in a synchronised update, taking cells
/// off with ECH and the background of the cell, the pen and the cursor back.
///
/// Sans-I/O and allocation-free after Create. Build returns bytes in an
/// internal buffer, valid until the next call, and changes nothing: not the
/// mirror, which is never fed the probe, and not the overlay, which the
/// caller has taken off before (Overlay::BuildUndo; a drawn overlay is not in
/// the mirror's cells, and the probe's removal would put the mirror's back
/// over it).
class WidthProbe {
public:
    /// Null when the buffer cannot be allocated.
    [[nodiscard]] static std::unique_ptr<WidthProbe> Create() noexcept;

    /// The glyphs of the probe of `cls`, in the order they are printed: the
    /// class's representatives, then `encountered` (the first code point of
    /// the class the session met; 0 for none) unless it is one of them or not
    /// of the class. Returns the count; 0 for a class that has no probe.
    static std::size_t Glyphs(
        grid::EWidthClass cls, char32_t encountered,
        std::array<char32_t, PROBE_GLYPHS_MAX>& out) noexcept;

    /// Whether the cursor of the mirror stands at a clean point for the probe
    /// of `cls` (the first condition that fails, EProbeGate::Clean for none).
    [[nodiscard]] static EProbeGate Check(const PredictionEngine& engine,
                                          const MirrorView& mirror,
                                          grid::EWidthClass cls,
                                          char32_t encountered) noexcept;

    /// The probe, or nothing when the point is not clean.
    [[nodiscard]] std::optional<WidthProbeWrite> Build(
        const PredictionEngine& engine, const MirrorView& mirror,
        grid::EWidthClass cls, char32_t encountered) noexcept;

private:
    WidthProbe() noexcept = default;

    std::array<std::byte, PROBE_BYTES_MAX> Buf_{};
};

}  // namespace zet::predict
