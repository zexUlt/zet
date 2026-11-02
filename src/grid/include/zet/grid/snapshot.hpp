#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "zet/grid/grid.hpp"
#include "zet/grid/limits.hpp"

namespace zet::grid {

/// Indicates which degrade step (if any) was applied to fit the snapshot
/// within MaxBytes (design.md §9).
///
/// Steps are applied in order; each step subsumes the previous.
enum class EDegradation : std::uint8_t {
    None = 0,        ///< Full-fidelity snapshot.
    HistoryTrimmed,  ///< Scrollback history trimmed by line count.
    Colors256,       ///< TrueColor downgraded to nearest xterm-256 palette.
    NoColor,         ///< All foreground/background colour stripped.
    RowsCropped,     ///< Screen rows cropped from the top; cursor row kept.
};

/// Options controlling Snapshot::Create encoding.
///
/// Intentionally additive: PR1 call sites remain valid after the degrade
/// ladder (PR2) adds further fields to this struct.
struct SnapshotOptions {
    /// Maximum encoded size in bytes.  0 → SNAPSHOT_MAX_BYTES.
    /// Values above SNAPSHOT_MAX_BYTES are clamped to SNAPSHOT_MAX_BYTES.
    std::size_t MaxBytes{0};
};

/// A serialized terminal snapshot — immutable byte sequence.
///
/// Byte layout (design.md §9):
///   [0 .. ScrollbackLen())  preamble + history lines
///   [ScrollbackLen() .. end)  screen section (DECSET 2026 sync-output active)
///
/// Not copyable or movable.  Delete with `delete snap`.
class Snapshot {
public:
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&&) = delete;
    Snapshot& operator=(Snapshot&&) = delete;

    ~Snapshot() noexcept;

    /// Serialize grid into a Snapshot whose bytes fit within opts.MaxBytes.
    ///
    /// Applies the degrade ladder (design.md §9) in order until the encoding
    /// fits: trim history by count → 256-colour → no colour → crop top rows.
    /// Use Degradation() to discover which step was applied.
    ///
    /// Returns EGridError::SnapshotTooLarge only when even a single-row
    /// no-colour snapshot exceeds opts.MaxBytes (i.e. the cap is too small
    /// to hold any useful snapshot).
    ///
    /// Returns EGridError::AllocationFailed when new(std::nothrow) fails.
    [[nodiscard]] static GridResult<Snapshot*> Create(
        const Grid& grid, SnapshotOptions opts = {}) noexcept;

    /// The full encoded byte sequence.
    [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;

    /// Byte offset where the screen section begins within Bytes().
    ///
    ///   bytes[0 .. ScrollbackLen())  preamble + history lines (outside sync)
    ///   bytes[ScrollbackLen() .. )   screen section (inside DECSET 2026 sync)
    [[nodiscard]] std::size_t ScrollbackLen() const noexcept {
        return ScrollbackLen_;
    }

    /// Which degradation step (if any) was applied to fit within MaxBytes.
    [[nodiscard]] EDegradation Degradation() const noexcept {
        return Degradation_;
    }

    /// True when the source Grid had PossiblyDiverged() set.
    [[nodiscard]] bool PossiblyDiverged() const noexcept {
        return PossiblyDiverged_;
    }

    /// True when the snapshot was taken from an alt-screen grid.
    [[nodiscard]] bool AltScreen() const noexcept { return AltScreen_; }

private:
    explicit Snapshot() noexcept = default;

    std::byte* Buf_{nullptr};  ///< owned; freed in destructor
    std::size_t Len_{0};       ///< number of valid bytes in Buf_
    std::size_t ScrollbackLen_{0};
    EDegradation Degradation_{EDegradation::None};
    bool PossiblyDiverged_{false};
    bool AltScreen_{false};
};

}  // namespace zet::grid
