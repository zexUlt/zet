#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/predict/limits.hpp"

namespace zet::predict {

/// `--predict` (design.md §9). `Never` makes every call a no-op.
enum class EDisplayMode : std::uint8_t {
    Adaptive,  ///< show by SRTT with hysteresis, or after glitches
    Always,    ///< always show; epochs and checks are unchanged
    Never,
};

/// What the engine reads of the client's mirror: the grid and the parser that
/// feeds it. Read-only, and never stored: path B replaces the grid, so a
/// pointer kept across calls could dangle.
struct MirrorView {
    const grid::Grid& Screen;
    const grid::VtParser& Parser;
};

/// The engine's own echo-ack input (the wire `EchoAck`, design.md §9, without
/// a dependency on zet_wire): all output of generation `StreamGen` up to
/// `OutputOffset` was read from the pty after the input up to `InputOffset`
/// had lain there for ECHO_TIMEOUT.
struct EchoAck {
    std::uint32_t StreamGen{0};
    std::uint64_t OutputOffset{0};
    std::uint64_t InputOffset{0};
};

/// One predicted cell. Always in the cursor row of the main screen.
struct Prediction {
    std::uint16_t Row{0};
    std::uint16_t Col{0};
    /// What the cell is predicted to hold; 0 is an empty cell (backspace).
    char32_t Codepoint{0};
    /// What the mirror held there when the cell was first predicted (0 for
    /// empty). A match with it earns no credit.
    char32_t Prior{0};
    /// Input offset after the bytes of the keystroke.
    std::uint64_t InputEnd{0};
    std::uint32_t Epoch{0};
    /// Set by the overlay builder when the cell is on the terminal.
    bool Drawn{false};
    /// The mirror was seen holding the predicted glyph here, as the echo of
    /// this keystroke: the cell's change is already accounted for.
    bool Echoed{false};
    Timestamp Created;
};

/// The predicted cursor, with the same (input end, epoch) pair.
struct CursorPrediction {
    std::uint16_t Row{0};
    std::uint16_t Col{0};
    std::uint64_t InputEnd{0};
    std::uint32_t Epoch{0};
};

/// Prediction engine (design.md §9, "Предиктивное эхо"): sans-I/O, after mosh's
/// PredictionEngine. No allocation, no exceptions, no clock: time and the
/// mirror come in with every call.
///
/// Prediction lives in epochs. A prediction is visible only when its epoch is
/// at most `ConfirmedEpoch()`; any event the engine does not model raises the
/// current epoch ("becomes tentative"), and an epoch is confirmed when one of
/// its predictions is found correct. Nothing about passwords is special: with
/// echo off no epoch is ever confirmed.
///
/// Call order, per event, by the client:
///   input to the agent        OnInput
///   Data fed to the mirror    OnOutput (after the mirror was fed)
///   EchoAck from the agent    OnEchoAck
///   path-B snapshot applied   OnSnapshot (after the new grid was filled)
///   local terminal resized    OnResize (after the mirror was resized)
///   RTT sample                OnSrtt
///   idle                      OnTick
class PredictionEngine {
public:
    /// Created together with the mirror it watches, at output offset 0 of
    /// generation `streamGen`: the counters it compares against are the
    /// mirror's own, and a fresh mirror starts them at zero.
    PredictionEngine(const MirrorView& mirror, EDisplayMode mode,
                     std::uint32_t streamGen = 0) noexcept;

    /// Bytes the client puts into the input stream (after the `~` escape),
    /// ending at input offset `inputEnd`.
    void OnInput(ByteSpan bytes, std::uint64_t inputEnd, Timestamp now,
                 const MirrorView& mirror) noexcept;

    /// The mirror was fed output up to `outputEnd` (offset in the current
    /// generation). Checks what that output did to the mirror, then settles
    /// the echo-acks the output reached.
    void OnOutput(std::uint64_t outputEnd, Timestamp now,
                  const MirrorView& mirror) noexcept;

    void OnEchoAck(const EchoAck& ack, Timestamp now,
                   const MirrorView& mirror) noexcept;

    /// A snapshot opened generation `streamGen` and was applied: everything
    /// is dropped and the engine rebases on the mirror, which may be a new
    /// object. A snapshot sets the cursor absolutely, so the column is known.
    void OnSnapshot(std::uint32_t streamGen, Timestamp now,
                    const MirrorView& mirror) noexcept;

    /// The local terminal changed size (the mirror was resized).
    void OnResize(Timestamp now, const MirrorView& mirror) noexcept;

    /// The channel's smoothed RTT.
    void OnSrtt(Duration srtt, Timestamp now) noexcept;

    /// Time passed with nothing else happening.
    void OnTick(Timestamp now) noexcept;

    // --- State -------------------------------------------------------------

    [[nodiscard]] EDisplayMode Mode() const noexcept { return Mode_; }
    [[nodiscard]] std::span<const Prediction> Predictions() const noexcept {
        return {Preds_.data(), Count_};
    }
    [[nodiscard]] const std::optional<CursorPrediction>& Cursor()
        const noexcept {
        return Cursor_;
    }
    [[nodiscard]] std::uint32_t PredictionEpoch() const noexcept {
        return Epoch_;
    }
    [[nodiscard]] std::uint32_t ConfirmedEpoch() const noexcept {
        return Confirmed_;
    }
    /// Whether the mirror's cursor column is known to match the terminal's.
    [[nodiscard]] bool ColumnTrusted() const noexcept { return Trusted_; }
    [[nodiscard]] std::uint32_t GlitchTrigger() const noexcept {
        return Glitch_;
    }
    [[nodiscard]] bool SrttTrigger() const noexcept { return SrttTrigger_; }
    [[nodiscard]] std::uint32_t StreamGen() const noexcept { return Gen_; }
    [[nodiscard]] std::uint64_t OutputOffset() const noexcept {
        return OutOffset_;
    }

    /// Whether confirmed predictions are shown now (the mode and, in adaptive,
    /// SRTT and glitches). Which predictions are confirmed is IsVisible.
    [[nodiscard]] bool ShowPredictions() const noexcept;
    /// Whether shown predictions are underlined.
    [[nodiscard]] bool UnderlinePredictions() const noexcept;
    /// A prediction is shown when its epoch is confirmed and display is on.
    [[nodiscard]] bool IsVisible(const Prediction& p) const noexcept {
        return p.Epoch <= Confirmed_ && ShowPredictions();
    }

    /// For the overlay builder: record what is on the terminal. Adaptive mode
    /// stops showing only while nothing is drawn.
    void SetDrawn(std::size_t index, bool drawn) noexcept;
    void ClearDrawn() noexcept;

private:
    enum class EInputState : std::uint8_t { Ground, Esc, Csi, Ss3, Utf8 };

    struct Position {
        std::uint16_t Row{0};
        std::uint16_t Col{0};
    };

    // Input side.
    void StepInput(std::byte b, std::uint64_t end, Timestamp now,
                   const MirrorView& mirror) noexcept;
    void StepGround(std::uint8_t b, std::uint64_t end, Timestamp now,
                    const MirrorView& mirror) noexcept;
    void StepCsi(std::uint8_t b) noexcept;
    void PredictChar(char32_t cp, std::uint64_t end, Timestamp now,
                     const MirrorView& mirror) noexcept;
    void PredictBackspace(std::uint64_t end, Timestamp now,
                          const MirrorView& mirror) noexcept;
    [[nodiscard]] bool Predictable(const MirrorView& mirror) const noexcept;
    [[nodiscard]] Position BaseCursor(const MirrorView& mirror) const noexcept;
    [[nodiscard]] bool TailEmpty(const MirrorView& mirror, std::uint16_t row,
                                 std::uint16_t col) const noexcept;
    [[nodiscard]] bool Upsert(const Prediction& p) noexcept;
    [[nodiscard]] bool HasRoomFor(std::uint16_t row,
                                  std::uint16_t col) const noexcept;

    // Epochs.
    void BecomeTentative() noexcept;
    void KillEpoch(std::uint32_t epoch) noexcept;
    void ResetAll() noexcept;
    void RemoveAt(std::size_t index) noexcept;

    // Mirror side.
    void Observe(const MirrorView& mirror) noexcept;
    /// Whether what changed in the cursor row since the last look is nothing
    /// but the echo of outstanding predictions. Anything else printed there
    /// (a prompt, a cursor jump, an erase) may be a prompt with echo off.
    [[nodiscard]] bool OutputIsPredictedEcho(const MirrorView& mirror) noexcept;
    void Rebase(const MirrorView& mirror) noexcept;
    void Drain(Timestamp now, const MirrorView& mirror) noexcept;
    void Reconcile(std::uint64_t inputOffset, Timestamp now,
                   const MirrorView& mirror) noexcept;
    void ReconcileCursor(std::uint64_t inputOffset,
                         const grid::Grid& g) noexcept;
    void Cull(Timestamp now) noexcept;
    [[nodiscard]] bool AnyDrawn() const noexcept;

    EDisplayMode Mode_;

    std::array<Prediction, PREDICTIONS_MAX> Preds_{};
    std::size_t Count_{0};
    std::optional<CursorPrediction> Cursor_;
    std::uint32_t Epoch_{1};
    std::uint32_t Confirmed_{0};
    /// Column where input started in the cursor row; backspace stops there.
    std::optional<std::uint16_t> InputStartCol_;
    bool Trusted_{false};

    // Input parser state, across calls.
    EInputState In_{EInputState::Ground};
    bool InPaste_{false};
    char32_t Utf8Cp_{0};
    std::uint8_t Utf8Left_{0};
    std::uint8_t Utf8Len_{0};
    std::uint8_t CsiLen_{0};
    std::array<char, 4> CsiParam_{};
    bool CsiHasOther_{false};

    // Generation, output position, echo-acks waiting for the output.
    std::uint32_t Gen_;
    std::uint64_t OutOffset_{0};
    std::array<EchoAck, ECHO_ACK_PENDING_MAX> Acks_{};
    std::size_t AckCount_{0};

    // What the mirror looked like at the last look.
    std::uint16_t LastCols_{0};
    std::uint16_t LastRows_{0};
    std::uint16_t LastRow_{0};
    std::uint16_t LastCol_{0};
    std::uint64_t LastRowHash_{0};
    bool LastAlt_{false};
    std::uint64_t LastScroll_{0};
    std::uint64_t LastColumnSet_{0};
    std::uint64_t LastUnknown_{0};
    std::uint64_t LastDcsApc_{0};

    // Display state.
    std::optional<Duration> Srtt_;
    bool SrttTrigger_{false};
    bool Flagging_{false};
    std::uint32_t Glitch_{0};
    std::optional<Timestamp> LastQuick_;
};

}  // namespace zet::predict
