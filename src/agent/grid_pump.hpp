#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

namespace zet::agent {

/// Maximum time waiting for VT parser Ground before forcing a reset. §9
/// ADR-030.
inline constexpr Duration GROUND_WAIT_TIMEOUT = std::chrono::seconds{5};
/// Maximum pty bytes processed while waiting for Ground. §9 ADR-030.
inline constexpr std::size_t GROUND_WAIT_BYTES = 1U * 1024U * 1024U;

/// Encapsulates the per-session terminal grid, VT parser, and snapshot
/// supply state.  Separated from Agent so tests can exercise it directly
/// without touching agent.cpp's anonymous-namespace internals. §9 ADR-030.
class GridPump {
public:
    GridPump(const GridPump&) = delete;
    GridPump& operator=(const GridPump&) = delete;
    GridPump(GridPump&&) = delete;
    GridPump& operator=(GridPump&&) = delete;

    ~GridPump();

    /// Creates a GridPump with an initial terminal size of cols × rows.
    [[nodiscard]] static grid::GridResult<std::unique_ptr<GridPump>> Create(
        std::uint16_t cols, std::uint16_t rows,
        std::uint32_t scrollbackLimit =
            grid::DEFAULT_SCROLLBACK_LIMIT) noexcept;

    // ── Feed ──────────────────────────────────────────────────────────────

    /// Feed raw pty bytes into the VT parser and grid.  Must be called before
    /// writing the same bytes to the session, so the grid always tracks the
    /// full output.
    void FeedBytes(const std::byte* data, std::size_t len) noexcept;

    // ── Resize ────────────────────────────────────────────────────────────

    /// Resize the grid to cols × rows.  On error the caller must not resize
    /// the pty — keeps grid and pty in sync.  Enforces the same caps as
    /// Grid::Create (MAX_COLS × MAX_ROWS). §9 ADR-030.
    [[nodiscard]] grid::GridResult<void> HandleResize(
        std::uint16_t cols, std::uint16_t rows) noexcept;

    // ── Snapshot supply ───────────────────────────────────────────────────

    /// Returned when a snapshot is ready to supply via
    /// AgentConnection::SupplySnapshot. `Bytes` points into an internally-owned
    /// Snapshot; valid until ReleaseSnapshot().
    struct SnapReady {
        std::span<const std::byte> Bytes;
        std::uint32_t ScrollbackLen{0};
        std::uint8_t Flags{0};
    };

    /// Arms the ground-wait after a NeedsSnapshot event.
    /// rows/cols == 0 means "use the grid's current dimensions".
    /// Calls TryBuild immediately; if the parser is already in Ground, returns
    /// SnapReady so the caller can call SupplySnapshot without waiting.
    [[nodiscard]] std::optional<SnapReady> OnNeedsSnapshot(
        std::uint16_t rows, std::uint16_t cols, Timestamp now) noexcept;

    /// Tries to build and return a snapshot.  Call each main-loop iteration
    /// while HasPendingSnap() is true.
    ///
    /// Returns:
    ///   - unexpected(SizeTooLarge / AllocationFailed / SnapshotTooLarge):
    ///     close the connection.  SnapshotTooLarge is what is left after the
    ///     degrade ladder: not even one row fits the snapshot cap.
    ///   - optional{nullopt}: still waiting for parser Ground.
    ///   - optional{SnapReady}: snapshot built; call SupplySnapshot.
    [[nodiscard]] grid::GridResult<std::optional<SnapReady>> TryBuild(
        Timestamp now) noexcept;

    /// Release the snapshot bytes (call after
    /// AgentConnection::GridEmissionDone() returns true — GridEnd has been sent
    /// and the wire no longer reads Bytes).
    void ReleaseSnapshot() noexcept;

    /// Cancel a pending ground-wait (e.g. requesting connection closed).
    void CancelPendingSnap() noexcept;

    [[nodiscard]] bool HasSnapshot() const noexcept { return Snap_ != nullptr; }
    [[nodiscard]] bool HasPendingSnap() const noexcept {
        return SnapWait_.has_value();
    }

    /// The ground-wait deadline (valid only when HasPendingSnap()).
    [[nodiscard]] Deadline NextDeadline() const noexcept;

    // ── Test accessors ────────────────────────────────────────────────────

    [[nodiscard]] const grid::Grid& GetGrid() const noexcept { return *Grid_; }
    [[nodiscard]] grid::EParserState ParserState() const noexcept {
        return Parser_.State();
    }
    [[nodiscard]] std::size_t TotalBytesProcessed() const noexcept {
        return TotalGridBytes_;
    }

private:
    explicit GridPump(std::unique_ptr<grid::Grid> grid) noexcept;

    [[nodiscard]] grid::GridResult<std::optional<SnapReady>> DoBuild() noexcept;

    std::unique_ptr<grid::Grid> Grid_;
    grid::VtParser Parser_;
    std::size_t TotalGridBytes_{0};

    struct SnapWait {
        Timestamp Deadline;
        std::size_t BytesCap;
        std::uint16_t Rows;
        std::uint16_t Cols;
    };
    std::optional<SnapWait> SnapWait_;
    grid::Snapshot* Snap_{nullptr};
};

}  // namespace zet::agent
