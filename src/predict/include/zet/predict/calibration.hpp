#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "zet/core/bytes.hpp"
#include "zet/core/time.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/vt_parser.hpp"
#include "zet/predict/cpr_demux.hpp"
#include "zet/predict/engine.hpp"
#include "zet/predict/limits.hpp"

namespace zet::predict {

/// What the calibration works on: the client's mirror and the engine that
/// watches it. Pointers, because the mirror's grid is replaced by a snapshot
/// or a resize and must not be remembered between calls. Empty (all null)
/// after the mirror was lost: the calls then only keep stdin right.
struct CalibrationTarget {
    grid::Grid* Screen{nullptr};
    grid::VtParser* Parser{nullptr};
    PredictionEngine* Engine{nullptr};

    [[nodiscard]] bool Valid() const noexcept {
        return Screen != nullptr && Parser != nullptr && Engine != nullptr;
    }
    [[nodiscard]] MirrorView View() const noexcept {
        return MirrorView{*Screen, *Parser};
    }
};

/// Why the client wants to ask the terminal where its cursor is.
enum ECalReason : std::uint8_t {
    CalStart = 1,     ///< the mirror begins empty, the shell's prompt unseen
    CalResize = 2,    ///< the terminal reflowed what it showed
    CalDcs = 4,       ///< a DCS/SOS/PM/APC string passed through
    CalAltExit = 8,   ///< the program left the alternate screen
    CalUnknown = 16,  ///< a sequence the mirror does not know
};

/// Row synchronisation of the mirror by the terminal's cursor position report
/// (design.md §9, round C, "Калибровка зеркала запросом позиции курсора").
/// Sans-I/O, after PredictionEngine: the client, the test rig and the fuzzer
/// drive it through the same calls, and no clock or system call is inside.
///
/// It owns the CprDemux (stdin side), decides when a query is due, holds the
/// mirror still at the query's point in the output until the answer comes
/// (freeze), and applies the answer there: Grid::Resync, then the held output.
/// The client keeps the bytes: it appends the query to the stdout queue, tells
/// the calibration when the queue wrote bytes, and routes stdin through it.
///
/// Call order, per event, by the client:
///   stdin bytes read                          OnInput
///   forwarded stdin bytes, after any call     TakeForwarded
///   mirror fed with output / notice           OnTerminalBytes
///   EchoAck from the agent                    OnEchoAck
///   a query may be sent                       QueryDue, BeginQuery, then the
///                                             query is queued (AbortQuery if
///                                             it could not be)
///   bytes written to the terminal             OnWritten (query excluded)
///   the query's last byte written             OnQueryWritten
///   idle                                      OnTick, at NextDeadline
///   path-B snapshot applied to the mirror     OnSnapshot
///   local terminal about to be resized        OnResize (before the mirror)
///   mirror lost                               Detach
class Calibration {
public:
    /// Longest read OnInput takes: the forward buffer is sized by it.
    static constexpr std::size_t INPUT_MAX = 4096;
    /// `CSI 6 n CSI c`.
    static constexpr std::size_t QUERY_SIZE = 7;

    /// The query the client writes.
    [[nodiscard]] static ByteSpan Query() noexcept;

    /// `seeded`: the mirror was filled from a snapshot, so the start needs no
    /// query. `target` is the mirror as it is now: what its parser has counted
    /// so far gives no reason. Null when the demultiplexer cannot be
    /// allocated.
    [[nodiscard]] static std::unique_ptr<Calibration> Create(
        bool seeded, const CalibrationTarget& target) noexcept;

    // --- The terminal side -------------------------------------------------

    /// Output (or a notice, `notice`) the terminal will receive, up to agent
    /// output offset `outputEnd`: into the mirror, or into the held buffer
    /// while the mirror waits. Raises the reasons the output gives, except for
    /// a notice.
    void OnTerminalBytes(ByteSpan bytes, std::uint64_t outputEnd, Timestamp now,
                         const CalibrationTarget& target,
                         bool notice = false) noexcept;

    /// The engine's EchoAck input; held (the latest only) while the mirror
    /// waits.
    void OnEchoAck(const EchoAck& ack, Timestamp now,
                   const CalibrationTarget& target) noexcept;

    // --- Queries -----------------------------------------------------------

    /// Whether the client may queue a query now: a reason is pending, the
    /// mirror is in a state where the cursor position can be compared (parser
    /// in Ground and not inside a UTF-8 character, main screen, no pending
    /// wrap, DECOM off, no synchronized-output frame open), no query is on its
    /// way, and the demultiplexer allows one. Room in the stdout queue is the
    /// client's to check.
    [[nodiscard]] bool QueryDue(Timestamp now,
                                const CalibrationTarget& target) noexcept;

    /// The client is about to queue the query (after QueryDue and its room
    /// check): the mirror freezes here. False when the held buffer cannot be
    /// allocated; the client then sends nothing, and no query is due again.
    [[nodiscard]] bool BeginQuery(Timestamp now,
                                  const CalibrationTarget& target) noexcept;

    /// The query could not be queued after BeginQuery.
    void AbortQuery(Timestamp now, const CalibrationTarget& target) noexcept;

    /// Bytes the client wrote to the terminal, excluding its own query.
    void OnWritten(ByteSpan bytes, Timestamp now) noexcept;

    /// The query's last byte was written to the terminal: the answer's clock
    /// starts.
    void OnQueryWritten(Timestamp now,
                        const CalibrationTarget& target) noexcept;

    // --- Stdin ---------------------------------------------------------------

    /// Bytes read from stdin, at most INPUT_MAX. What goes on to the agent is
    /// collected by TakeForwarded.
    void OnInput(ByteSpan typed, Timestamp now,
                 const CalibrationTarget& target) noexcept;

    /// Bytes for the agent that the calls since the last TakeForwarded
    /// released, in order. The span is valid until the next call. At most
    /// INPUT_MAX + 2 * CPR_HELD_MAX bytes arise between two OnInput calls.
    [[nodiscard]] ByteSpan Forwarded() const noexcept;
    void ClearForwarded() noexcept;

    void OnTick(Timestamp now, const CalibrationTarget& target) noexcept;
    [[nodiscard]] std::optional<Timestamp> NextDeadline() const noexcept;

    // --- Lifecycle -----------------------------------------------------------

    /// A snapshot was applied to the mirror (a fresh grid): the wait ends, the
    /// held output is thrown away with the old grid, the query's answer (if one
    /// is on its way) is stripped and ignored, and no query is due until the
    /// output gives a new reason.
    void OnSnapshot(Timestamp now, const CalibrationTarget& target) noexcept;

    /// The local terminal is about to change size, before the mirror does: the
    /// wait ends without calibration (the held output goes into the mirror at
    /// its old size), the silent mark is cleared, and the resize is a reason.
    void OnResize(Timestamp now, const CalibrationTarget& target) noexcept;

    /// The mirror was lost: no queries from now on; the demultiplexer goes on
    /// stripping the late answer.
    void Detach(Timestamp now) noexcept;

    // --- State ---------------------------------------------------------------

    /// The mirror waits for the answer.
    [[nodiscard]] bool Frozen() const noexcept { return Frozen_; }
    /// The query is queued and not yet written to the terminal.
    [[nodiscard]] bool QueryUnwritten() const noexcept {
        return QueryUnwritten_;
    }
    [[nodiscard]] std::uint8_t Reasons() const noexcept { return Reasons_; }
    [[nodiscard]] const CprDemux& Demux() const noexcept { return *Demux_; }
    /// Queries answered by a resync / by a match / given up (timeout, failure,
    /// overflow, resize, snapshot).
    [[nodiscard]] std::uint32_t Resynced() const noexcept { return Resynced_; }
    [[nodiscard]] std::uint32_t Matched() const noexcept { return Matched_; }
    [[nodiscard]] std::uint32_t Abandoned() const noexcept {
        return Abandoned_;
    }

private:
    /// Collects what the demultiplexer reports. Fixed arrays: no allocation.
    class Sink final : public CprSink {
    public:
        static constexpr std::size_t FORWARD_MAX = INPUT_MAX + 2 * CPR_HELD_MAX;
        static constexpr std::size_t EVENTS_MAX = 16;

        void Forward(ByteSpan bytes) noexcept override;
        void Strip(ByteSpan /*bytes*/) noexcept override {}
        void Event(const CprEvent& event) noexcept override;

        std::array<std::byte, FORWARD_MAX> Out{};
        std::size_t OutLen{0};
        std::array<CprEvent, EVENTS_MAX> Events{};
        std::size_t EventCount{0};
    };

    Calibration(std::unique_ptr<CprDemux> demux, bool seeded,
                const CalibrationTarget& target) noexcept;

    /// Raises the reasons the parser's counters show since the last look.
    /// `quiet`: advance without raising (a notice).
    void Evaluate(const CalibrationTarget& target, bool quiet) noexcept;
    void Rebase(const CalibrationTarget& target) noexcept;
    [[nodiscard]] static bool MirrorReady(const grid::Grid& g,
                                          const grid::VtParser& p) noexcept;
    /// Handles what the last demultiplexer call reported.
    void Dispatch(Timestamp now, const CalibrationTarget& target) noexcept;
    void Apply(const CprEvent& event, Timestamp now,
               const CalibrationTarget& target) noexcept;
    /// Ends the wait: the held output goes into the mirror, the held EchoAck
    /// into the engine.
    void Thaw(Timestamp now, const CalibrationTarget& target) noexcept;
    /// Ends the wait without calibration and abandons the live query.
    void GiveUp(Timestamp now, const CalibrationTarget& target) noexcept;

    std::unique_ptr<CprDemux> Demux_;
    Sink Sink_;

    std::uint8_t Reasons_{0};
    bool Frozen_{false};
    bool QueryUnwritten_{false};
    /// The query was queued and then given up: when it is written, its entry
    /// is made and abandoned at once, so that the late answer is stripped.
    bool AbandonOnWrite_{false};
    /// The parser gave up on a string at its limit: no queries, for the
    /// terminal is inside a string we cannot see the end of.
    bool Sticky_{false};
    bool Detached_{false};
    bool Disabled_{false};

    // What the query was asked for.
    std::uint8_t QueryReasons_{0};
    bool ExpectKnown_{false};
    std::uint16_t ExpectRow_{0};
    std::uint16_t ExpectCol_{0};

    // The wait: output after the query's point, and the latest EchoAck.
    std::unique_ptr<std::byte[]> Lag_;
    std::size_t LagLen_{0};
    std::uint64_t LagEnd_{0};
    bool LagHasEnd_{false};
    std::optional<EchoAck> DeferredAck_;

    // Where the parser's counters stood at the last look.
    std::uint64_t SeenUnknown_{0};
    std::uint64_t SeenDcs_{0};
    std::uint64_t SeenBypass_{0};
    bool PrevAlt_{false};

    std::uint32_t Resynced_{0};
    std::uint32_t Matched_{0};
    std::uint32_t Abandoned_{0};
};

}  // namespace zet::predict
