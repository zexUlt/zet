#pragma once

#include <cstdint>
#include <memory>

#include "zet/core/bytes.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"

namespace zet::client {

/// The client's mirror grid (docs/design.md §9, "Предиктивное эхо"): a Grid
/// with no history that is fed everything the client writes to the local
/// terminal, in the order it is written. It models the local terminal, not
/// the agent's grid. Nothing reads it yet; the prediction overlay will.
///
/// Memory is the Grid's own clamp: two screens of at most MAX_SCREEN_CELLS
/// cells and no scrollback. No allocation after construction except in Resize
/// and ResetFromSnapshot, which allocate before they replace anything.
class Mirror {
public:
    /// Null when the arenas cannot be allocated.
    [[nodiscard]] static std::unique_ptr<Mirror> Create(
        std::uint16_t cols, std::uint16_t rows) noexcept;

    /// Bytes the terminal is about to receive.
    void Feed(ByteSpan bytes) noexcept;

    /// A path-B snapshot was written: the mirror becomes a fresh grid of the
    /// same size with the snapshot fed in, so nothing the preamble does not
    /// overwrite survives (second saved cursor, kitty keyboard, parser state).
    /// False when allocation failed; the mirror is then unchanged and no
    /// longer matches the terminal, so the caller drops it.
    [[nodiscard]] bool ResetFromSnapshot(ByteSpan snapshot) noexcept;

    /// The local terminal changed size: truncate or extend, no reflow, as the
    /// agent's grid does. False when allocation failed (mirror unchanged).
    [[nodiscard]] bool Resize(std::uint16_t cols, std::uint16_t rows) noexcept;

    [[nodiscard]] const grid::Grid& Screen() const noexcept { return *Grid_; }
    /// For the calibration, which feeds the parser itself while the mirror
    /// waits and sets the grid's cursor by the terminal's answer. The pointer
    /// is not kept: a snapshot or a resize may replace the grid.
    [[nodiscard]] grid::Grid& MutableScreen() noexcept { return *Grid_; }
    [[nodiscard]] grid::VtParser& MutableParser() noexcept { return Parser_; }
    [[nodiscard]] const grid::VtParser& Parser() const noexcept {
        return Parser_;
    }

private:
    explicit Mirror(std::unique_ptr<grid::Grid> grid) noexcept
        : Grid_(std::move(grid)) {}

    std::unique_ptr<grid::Grid> Grid_;
    grid::VtParser Parser_;
};

}  // namespace zet::client
