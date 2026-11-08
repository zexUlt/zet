#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "zet/core/bytes.hpp"
#include "zet/predict/engine.hpp"
#include "zet/predict/limits.hpp"

namespace zet::predict {

/// The most bytes one cell of the overlay takes: CHA, a full SGR of the pen
/// (two sequences, every attribute, two truecolours and an underline colour),
/// a glyph of four bytes.
inline constexpr std::size_t OVERLAY_CELL_BYTES_MAX = 80;
/// The fixed part of one overlay: sync and cursor visibility, the pen back,
/// the cursor column back.
inline constexpr std::size_t OVERLAY_FRAME_BYTES_MAX = 256;
/// The most bytes taking the overlay off the terminal can be. The client keeps
/// this much room in its stdout queue ahead of every read, because the
/// restore before the agent's bytes cannot be left out (design.md §9).
inline constexpr std::size_t OVERLAY_UNDO_BYTES_MAX =
    PREDICTIONS_MAX * OVERLAY_CELL_BYTES_MAX + OVERLAY_FRAME_BYTES_MAX;
/// The most bytes one Build call returns: a restore and a new drawing in one.
inline constexpr std::size_t OVERLAY_BYTES_MAX =
    2 * PREDICTIONS_MAX * OVERLAY_CELL_BYTES_MAX + OVERLAY_FRAME_BYTES_MAX;

/// The prediction overlay (design.md §9, ADR-037, ADR-039): the bytes that put
/// the engine's visible predictions on the terminal over the unchanged stream
/// of the agent, and the bytes that take them off again.
///
/// The client keeps a mirror of the terminal, which the overlay never touches:
/// the mirror is what the terminal shows without it. Before every piece of
/// bytes that goes to the terminal the client takes the overlay off (BuildUndo:
/// each drawn cell is written again from the mirror), lets the bytes through,
/// feeds them to the mirror, and draws what is visible now (BuildUpdate).
/// Because the overlay is drawn only when the parser is in Ground, the terminal
/// is in Ground again when the next piece, and so the restore, reaches it.
///
/// Cells are placed with CHA in the cursor row, never CUP: the mirror does not
/// know the absolute row (ADR-037). DECSC/DECRC are never used.
///
/// Sans-I/O and allocation-free after Create. A Build call returns bytes in an
/// internal buffer, valid until the next call, and changes nothing: the caller
/// says with Commit that the bytes were queued for the terminal. Bytes that
/// were not queued leave the overlay as it was, and only what was queued is
/// ever taken off.
class Overlay {
public:
    /// Null when the buffer cannot be allocated.
    [[nodiscard]] static std::unique_ptr<Overlay> Create() noexcept;

    /// The bytes that restore the cells drawn so far from `mirror`, which must
    /// be as it was when they were drawn; empty when nothing is drawn.
    [[nodiscard]] ByteSpan BuildUndo(const MirrorView& mirror) noexcept;

    /// Whether BuildUndo, written after the terminal has been resized to
    /// `newCols` wide, still puts every cell right (design.md §9). Not when a
    /// column the restore addresses no longer exists, and not when the row
    /// may be wrapped: the terminal reflows it and the row, the column or both
    /// are not the mirror's any more. The caller then calls Forget().
    [[nodiscard]] bool UndoSurvivesResize(const MirrorView& mirror,
                                          std::uint16_t newCols) const noexcept;

    /// The bytes that turn what is drawn into what the engine shows now, or
    /// empty when nothing changes. Draws only where the mirror says the
    /// terminal will take it (Ground, main screen, no deferred wrap, no insert
    /// mode, ASCII, column known); elsewhere it returns nothing and the
    /// predictions wait for the next point.
    [[nodiscard]] ByteSpan BuildUpdate(const PredictionEngine& engine,
                                       const MirrorView& mirror) noexcept;

    /// The bytes of the last Build call were queued for the terminal. Tells
    /// the engine which predictions are drawn.
    void Commit(PredictionEngine& engine) noexcept;

    /// The terminal no longer has what was drawn, or the mirror is lost:
    /// forget it without a byte.
    void Forget() noexcept;

    /// Whether anything is on the terminal.
    [[nodiscard]] bool Drawn() const noexcept {
        return Drawn_.Count > 0 || Drawn_.Cursor.has_value();
    }

private:
    struct Placed {
        std::uint16_t Row{0};
        std::uint16_t Col{0};
        /// 0 is an empty cell (a backspace).
        char32_t Codepoint{0};
        bool Underline{false};
        /// The prediction's index in the engine, for Commit.
        std::uint16_t Index{0};

        [[nodiscard]] bool SameCell(const Placed& other) const noexcept {
            return Row == other.Row && Col == other.Col &&
                   Codepoint == other.Codepoint && Underline == other.Underline;
        }
    };

    struct State {
        std::array<Placed, PREDICTIONS_MAX> Cells{};
        std::size_t Count{0};
        /// Where the cursor is put, when it is not where the mirror has it.
        std::optional<std::uint16_t> Cursor;

        [[nodiscard]] bool Same(const State& other) const noexcept;
    };

    enum class EPending : std::uint8_t { None, Undo, Update };

    Overlay() noexcept = default;

    State Drawn_;
    State Next_;
    EPending Pending_{EPending::None};
    std::array<std::byte, OVERLAY_BYTES_MAX> Buf_{};
};

}  // namespace zet::predict
