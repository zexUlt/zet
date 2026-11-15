#include "zet/predict/probe.hpp"

#include <algorithm>
#include <new>

#include "overlay_bytes.hpp"
#include "zet/grid/cell.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/sgr.hpp"
#include "zet/grid/unicode.hpp"
#include "zet/predict/calibration.hpp"

namespace zet::predict {

namespace {

using emit::Begin;
using emit::Column;
using emit::End;
using emit::Printable;
using emit::Restore;

// The representatives of a class (design.md §9, "Глифы пробы"). Private use:
// one from each sub-range Nerd Fonts has, which tables of terminals divide a
// class along. Plane 16 has none of its own.
constexpr std::array<char32_t, 2> CYRILLIC_REPS{U'\u0416', U'\u0451'};
// nf-pl-left_hard_divider, nf-dev-git, nf-cod-add, nf-fa-home.
constexpr std::array<char32_t, 4> PRIVATE_USE_BMP_REPS{U'\uE0B0', U'\uE702',
                                                       U'\uEA60', U'\uF015'};
// nf-md-folder, nf-md-lecturn.
constexpr std::array<char32_t, 2> PRIVATE_USE_PLANES_REPS{U'\U000F024B',
                                                          U'\U000F1AF0'};

struct ClassGlyphs {
    const char32_t* Reps{nullptr};
    std::size_t Count{0};
};

ClassGlyphs RepsOf(grid::EWidthClass cls) noexcept {
    switch (cls) {
        case grid::EWidthClass::Cyrillic:
            return {CYRILLIC_REPS.data(), CYRILLIC_REPS.size()};
        case grid::EWidthClass::PrivateUseBmp:
            return {PRIVATE_USE_BMP_REPS.data(), PRIVATE_USE_BMP_REPS.size()};
        case grid::EWidthClass::PrivateUsePlanes:
            return {PRIVATE_USE_PLANES_REPS.data(),
                    PRIVATE_USE_PLANES_REPS.size()};
        default:
            return {};
    }
}

bool InClass(grid::EWidthClass cls, char32_t cp) noexcept {
    switch (cls) {
        case grid::EWidthClass::Cyrillic:
            return grid::IsCyrillicAmbiguous(cp);
        case grid::EWidthClass::PrivateUseBmp:
            return grid::IsPrivateUseBmp(cp);
        case grid::EWidthClass::PrivateUsePlanes:
            return grid::IsPrivateUsePlanes(cp);
        default:
            return false;
    }
}

/// Printable ASCII, which is what a cell left of the probe may hold.
bool IsAscii(const grid::Cell& c) noexcept {
    return c.Codepoint == 0 || (c.Codepoint >= 0x20 && c.Codepoint < 0x7F);
}

}  // namespace

std::unique_ptr<WidthProbe> WidthProbe::Create() noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return std::unique_ptr<WidthProbe>(new (std::nothrow) WidthProbe());
}

std::size_t WidthProbe::Glyphs(
    grid::EWidthClass cls, char32_t encountered,
    std::array<char32_t, PROBE_GLYPHS_MAX>& out) noexcept {
    const ClassGlyphs reps = RepsOf(cls);
    std::size_t n = 0;
    for (std::size_t i = 0; i < reps.Count; ++i) out[n++] = reps.Reps[i];
    if (reps.Count != 0 && encountered != 0 && InClass(cls, encountered) &&
        std::find(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n),
                  encountered) ==
            out.begin() + static_cast<std::ptrdiff_t>(n)) {
        out[n++] = encountered;
    }
    return n;
}

EProbeGate WidthProbe::Check(const PredictionEngine& engine,
                             const MirrorView& mirror, grid::EWidthClass cls,
                             char32_t encountered) noexcept {
    std::array<char32_t, PROBE_GLYPHS_MAX> glyphs{};
    const std::size_t k = Glyphs(cls, encountered, glyphs);
    if (k == 0) return EProbeGate::ClassNotProbed;

    const grid::Grid& g = mirror.Screen;
    if (engine.Mode() == EDisplayMode::Never) return EProbeGate::NoMirror;
    // The gates of the query and of the overlay (design.md §9).
    if (mirror.Parser.State() != grid::EParserState::Ground ||
        mirror.Parser.Utf8Pending()) {
        return EProbeGate::NotGround;
    }
    if (g.IsAltScreen()) return EProbeGate::AltScreen;
    if (g.PendingWrap()) return EProbeGate::PendingWrap;
    if (g.ModeSet(grid::EMode::Decom)) return EProbeGate::Decom;
    if (g.ModeSet(grid::EMode::Irm)) return EProbeGate::Irm;
    if (g.CharsetG0() != 0 || g.CharsetActive() != 0) {
        return EProbeGate::Charset;
    }
    // The frame is the probe's own: closing the program's would end it.
    if (g.ModeSet(grid::EMode::SyncOutput)) return EProbeGate::SyncFrame;
    if (g.AnyUnmodelledMode()) return EProbeGate::UnmodelledMode;
    // The pen is put back from the mirror, which lacks such an attribute.
    if (g.HasUnmodelledSgr()) return EProbeGate::UnmodelledSgr;
    if (!g.RowsTrusted()) return EProbeGate::RowsUntrusted;
    if (!engine.ColumnTrusted() || g.ColumnDoubt()) {
        return EProbeGate::ColumnUntrusted;
    }

    const std::uint16_t row = g.CursorRow();
    const std::uint16_t x = g.CursorCol();
    if (row >= g.Rows() || x >= g.Cols()) return EProbeGate::RightEdge;
    // A mark anywhere in the row: the terminal may have laid the row out
    // otherwise, and the restore would hit what the program has there.
    if (g.WidthDoubtClasses(row) != 0) return EProbeGate::RowMark;
    for (std::uint16_t c = 0; c < x; ++c) {
        if (!IsAscii(g.CellAt(row, c))) return EProbeGate::LeftOfCursor;
    }
    if (g.PathHasNonAscii()) return EProbeGate::PathNotAscii;

    const std::size_t end = static_cast<std::size_t>(x) + 2 * k;
    if (end >= g.Cols()) return EProbeGate::RightEdge;
    if (g.KnownFrom(row) > x) return EProbeGate::ProbeForeign;
    for (std::size_t c = x; c < end; ++c) {
        const grid::Cell& cell = g.CellAt(row, static_cast<std::uint16_t>(c));
        if (cell.Codepoint != 0 || cell.Width != 1) {
            return EProbeGate::ProbeOccupied;
        }
    }
    const grid::Grid::LastGlyph last = g.LastPrinted();
    if (last.Codepoint == 0 || last.Marked) return EProbeGate::NoLastGlyph;
    return EProbeGate::Clean;
}

std::optional<WidthProbeWrite> WidthProbe::Build(
    const PredictionEngine& engine, const MirrorView& mirror,
    grid::EWidthClass cls, char32_t encountered) noexcept {
    if (Check(engine, mirror, cls, encountered) != EProbeGate::Clean) {
        return std::nullopt;
    }
    std::array<char32_t, PROBE_GLYPHS_MAX> glyphs{};
    const std::size_t k = Glyphs(cls, encountered, glyphs);
    const grid::Grid& g = mirror.Screen;
    const std::uint16_t row = g.CursorRow();
    const std::uint16_t x = g.CursorCol();

    grid::EscWriter w{Buf_.data(), Buf_.size()};
    Begin(w, g);
    // The start is set absolutely, so the first answer is not needed.
    Column(w, x);
    // Hidden text on the background of the first cell, from a pen the
    // terminal is not assumed to hold: SGR 0 comes with it. What the glyphs
    // paint is erased again with each cell's own background.
    grid::Pen pen;
    grid::Cell hidden;
    hidden.Attrs = grid::Invisible;
    const grid::Cell& first = g.CellAt(row, x);
    hidden.BgKind = first.BgKind;
    for (int i = 0; i < 3; ++i) hidden.Bg[i] = first.Bg[i];
    grid::EmitSgr(w, pen, hidden);
    for (std::size_t i = 0; i < k; ++i) w.WriteUtf8(glyphs[i]);
    // The query as the calibration sends it: the client marks its offset.
    const std::size_t queryOffset = w.Pos;
    const ByteSpan query = Calibration::Query();
    w.WriteBytes(reinterpret_cast<const char*>(query.data()), query.size());
    // The register REP reads holds the program's glyph again. It is printed in
    // the first cell of the probe, not in its own, which may be in another
    // row; the cell is taken off below.
    Column(w, x);
    w.WriteUtf8(Printable(g.LastPrinted().Codepoint));
    for (std::size_t i = 0; i < 2 * k; ++i) {
        // Empty cells: nothing is printed again, so the register stays.
        (void)Restore(w, pen, g, row, static_cast<std::uint16_t>(x + i));
    }
    End(w, pen, g, x);
    if (w.Overflow) return std::nullopt;

    WidthProbeWrite out;
    out.Bytes = ByteSpan{Buf_.data(), w.Pos};
    out.QueryOffset = queryOffset;
    out.Row = row;
    out.Col = x;
    out.Glyphs = static_cast<std::uint8_t>(k);
    return out;
}

}  // namespace zet::predict
