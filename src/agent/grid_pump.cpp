#include "grid_pump.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

#include "zet/grid/grid.hpp"
#include "zet/grid/snapshot.hpp"
#include "zet/grid/vt_parser.hpp"

namespace zet::agent {

GridPump::GridPump(grid::Grid* grid) noexcept : Grid_(grid) {}

GridPump::~GridPump() { delete Snap_; }

grid::GridResult<std::unique_ptr<GridPump>> GridPump::Create(
    std::uint16_t cols, std::uint16_t rows) noexcept {
    auto g = grid::Grid::Create(cols, rows);
    if (!g) {
        return std::unexpected(g.error());
    }
    return std::unique_ptr<GridPump>(new GridPump(*g));
}

void GridPump::FeedBytes(const std::byte* data, std::size_t len) noexcept {
    if (len == 0) {
        return;
    }
    Parser_.Feed(data, len, *Grid_);
    TotalGridBytes_ += len;
}

grid::GridResult<void> GridPump::HandleResize(std::uint16_t cols,
                                              std::uint16_t rows) noexcept {
    return Grid_->Resize(cols, rows);
}

std::optional<GridPump::SnapReady> GridPump::OnNeedsSnapshot(
    std::uint16_t rows, std::uint16_t cols, Timestamp now) noexcept {
    const std::uint16_t targetRows = (rows == 0) ? Grid_->Rows() : rows;
    const std::uint16_t targetCols = (cols == 0) ? Grid_->Cols() : cols;
    SnapWait_ = SnapWait{
        .Deadline = now + GROUND_WAIT_TIMEOUT,
        .BytesCap = TotalGridBytes_ + GROUND_WAIT_BYTES,
        .Rows = targetRows,
        .Cols = targetCols,
    };
    // Only supply immediately when already in Ground — no force-reset here.
    if (Parser_.State() != grid::EParserState::Ground) {
        return std::nullopt;
    }
    auto result = DoBuild();
    if (!result || !result->has_value()) {
        return std::nullopt;
    }
    return **result;
}

grid::GridResult<std::optional<GridPump::SnapReady>> GridPump::TryBuild(
    Timestamp now) noexcept {
    if (!SnapWait_) {
        return std::optional<SnapReady>{std::nullopt};
    }
    const bool inGround = (Parser_.State() == grid::EParserState::Ground);
    const bool timeExpired = (now >= SnapWait_->Deadline);
    const bool bytesExpired = (TotalGridBytes_ >= SnapWait_->BytesCap);
    if (!inGround && !timeExpired && !bytesExpired) {
        return std::optional<SnapReady>{std::nullopt};
    }
    return DoBuild();
}

grid::GridResult<std::optional<GridPump::SnapReady>>
GridPump::DoBuild() noexcept {
    if (!SnapWait_) {
        return std::optional<SnapReady>{std::nullopt};
    }

    const bool inGround = (Parser_.State() == grid::EParserState::Ground);
    if (!inGround) {
        Parser_.Reset();
        Grid_->SetPossiblyDiverged(true);
    }

    // Resize to the requested dimensions if they differ.
    if (SnapWait_->Cols != 0 && SnapWait_->Rows != 0 &&
        (Grid_->Cols() != SnapWait_->Cols ||
         Grid_->Rows() != SnapWait_->Rows)) {
        static_cast<void>(Grid_->Resize(SnapWait_->Cols, SnapWait_->Rows));
    }

    auto snap = grid::Snapshot::Create(*Grid_);
    if (!snap) {
        SnapWait_.reset();
        // FIXME: ladder PR (m6-snapshot-ladder) makes SnapshotTooLarge
        // impossible; remove this path when that lands.
        return std::unexpected(snap.error());
    }

    delete Snap_;
    Snap_ = *snap;
    SnapWait_.reset();
    Grid_->SetPossiblyDiverged(false);

    return std::optional<SnapReady>{SnapReady{
        .Bytes = Snap_->Bytes(),
        .ScrollbackLen = static_cast<std::uint32_t>(Snap_->ScrollbackLen()),
        .Flags = Snap_->PossiblyDiverged() ? std::uint8_t{1} : std::uint8_t{0},
    }};
}

void GridPump::ReleaseSnapshot() noexcept {
    delete Snap_;
    Snap_ = nullptr;
}

void GridPump::CancelPendingSnap() noexcept { SnapWait_.reset(); }

Deadline GridPump::NextDeadline() const noexcept {
    if (SnapWait_) {
        return Deadline{SnapWait_->Deadline};
    }
    return Deadline{};
}

}  // namespace zet::agent
