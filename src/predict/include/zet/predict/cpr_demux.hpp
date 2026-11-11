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
#include "zet/predict/limits.hpp"

namespace zet::predict {

/// What the client learns from the demultiplexer about its own query.
enum class ECprEvent : std::uint8_t {
    /// The terminal answered: Row and Col are the reported, 1-based position.
    Answered,
    /// The terminal answered the end marker but not the position, or an
    /// answer was lost: the query is unanswered, as by the deadline.
    Failed,
    /// CPR_TIMEOUT passed. The terminal is now silent.
    TimedOut,
};

struct CprEvent {
    ECprEvent Kind{ECprEvent::Failed};
    std::uint16_t Row{0};
    std::uint16_t Col{0};
};

/// Where a call puts what it produced. The core allocates nothing, so the
/// caller owns the buffers; the calls come in stream order, so the forwarded
/// runs and the strips interleave as the bytes were read.
class CprSink {
public:
    virtual ~CprSink() = default;
    /// Bytes for the agent.
    virtual void Forward(ByteSpan bytes) noexcept = 0;
    /// A reply of ours, taken out of the stream (not for the agent).
    virtual void Strip(ByteSpan bytes) noexcept = 0;
    virtual void Event(const CprEvent& event) noexcept = 0;
};

/// Strips the terminal's answers to the client's own `CSI 6 n CSI c` out of
/// stdin (design.md §9, "Разбор stdin"). Sans-I/O, after PredictionEngine:
/// OnX(now, ...) calls and NextDeadline, no clock, no system calls. The client
/// builds one only when calibration is on; `never` does not construct it.
///
/// DA1 replies (`CSI ? Pm c`) are the anchor: no key sends one. The remote
/// program's DA1 queries (`CSI c`, `CSI 0 c`, `ESC Z`), which a private
/// VtParser counts in the outgoing stream, are kept as two counts: `ahead`,
/// written before the client's own query and not yet answered, and `behind`,
/// written after it. A DA1 reply with `ahead > 0` is the program's. With
/// `ahead == 0` and an own entry it is ours, and so is the position reply
/// right before it, if any; a position reply with no DA1 reply right after it
/// (a key such as Shift-F3 `CSI 1;2R`, or a program's answer) is forwarded.
/// The program's `CSI 6 n` is not tracked at all. There is at most one own
/// entry, live (before CPR_TIMEOUT) or dead (timed out or cancelled, until
/// CPR_FORGET), so the state does not grow with the number of queries.
///
/// Call order, per event, by the client:
///   bytes written to the terminal         OnOutput
///   own query `CSI 6 n CSI c` written     OnQuerySent (not through OnOutput)
///   bytes read from stdin                 OnInput
///   idle                                  OnTick, at NextDeadline
///   calibration cancelled by a snapshot   OnCancel
///   local terminal resized                OnResize
class CprDemux {
public:
    /// Null when the private grid cannot be allocated.
    [[nodiscard]] static std::unique_ptr<CprDemux> Create() noexcept;

    /// Bytes the client writes to the terminal, excluding its own query. The
    /// remote program's DA1 queries in them are counted, each on its last
    /// byte, never inside an OSC or DCS string. Counted also while Off.
    void OnOutput(ByteSpan bytes, Timestamp now) noexcept;

    /// The client wrote its own query. False, and nothing happens, while an
    /// own entry exists, live or dead: the client asks for at most one at a
    /// time, and after a timeout it waits for the late answer or CPR_FORGET.
    bool OnQuerySent(Timestamp now) noexcept;

    /// Bytes read from stdin. Reports the bytes for the agent and the events
    /// to `out`. A possible beginning of the client's own answer at the end
    /// of `bytes` is held while an own entry exists and no DA1 reply of the
    /// program is expected before it, for CPR_HOLD at most from its first
    /// byte. While Off, program DA1 replies are still recognised, to keep
    /// the count, and stdin is passed on unchanged.
    void OnInput(ByteSpan bytes, Timestamp now, CprSink& out) noexcept;

    /// Time passed: releases what is held, expires the live entry, forgets
    /// the dead one. Also done at the start of every call.
    void OnTick(Timestamp now, CprSink& out) noexcept;

    /// The live entry is abandoned (a snapshot): its answer, when it comes, is
    /// stripped and thrown away, and no event is raised for it. Held bytes
    /// keep their CPR_HOLD.
    void OnCancel(Timestamp now, CprSink& out) noexcept;

    /// The local terminal changed size: the live entry is abandoned as in
    /// OnCancel, and the silent mark is cleared. The client may ask again
    /// once the dead entry is gone (its late answer came, or CPR_FORGET).
    void OnResize(Timestamp now, CprSink& out) noexcept;

    /// The earliest moment OnTick changes something: end of the hold, of the
    /// live entry, or of the wait for a late answer. Unset when there is
    /// none. The client adds it to its poll deadline.
    [[nodiscard]] std::optional<Timestamp> NextDeadline() const noexcept;

    /// The terminal did not answer in time (or answered wrongly) and the
    /// client has not been told to ask again by a resize.
    [[nodiscard]] bool Silent() const noexcept { return Silent_; }
    /// The client's own query is waiting for its answer.
    [[nodiscard]] bool Pending() const noexcept { return Own_ == EOwn::Live; }
    /// An own entry exists, live or dead: while there is none, stdin passes
    /// through unchanged.
    [[nodiscard]] bool Outstanding() const noexcept {
        return Own_ != EOwn::None;
    }
    /// The dead entry was forgotten by CPR_FORGET. Stdin is not stripped;
    /// the program's DA1 queries are still counted.
    [[nodiscard]] bool Off() const noexcept { return Off_; }

    /// Program DA1 queries written before / after the own entry and not yet
    /// answered.
    [[nodiscard]] std::uint32_t Ahead() const noexcept { return Ahead_; }
    [[nodiscard]] std::uint32_t Behind() const noexcept { return Behind_; }
    /// Bytes read from stdin and not yet passed on.
    [[nodiscard]] std::size_t HeldSize() const noexcept;
    [[nodiscard]] ByteSpan HeldBytes() const noexcept {
        return {Held_.data() + HeldForwarded_, HeldLen_ - HeldForwarded_};
    }

private:
    enum class EOwn : std::uint8_t { None, Live, Dead };

    enum class ETry : std::uint8_t {
        NeedMore,
        Fail,
        Position,
        Da1,
        PasteStart
    };
    struct TryResult {
        ETry Status{ETry::Fail};
        std::size_t Length{0};
        std::uint16_t Row{0};
        std::uint16_t Col{0};
    };
    enum class EDa1Look : std::uint8_t { No, Partial, Full };

    explicit CprDemux(std::unique_ptr<grid::Grid> grid) noexcept
        : Grid_(std::move(grid)) {}

    /// An own entry exists and the next DA1 reply would land on it.
    [[nodiscard]] bool Eligible() const noexcept {
        return Own_ != EOwn::None && Ahead_ == 0;
    }
    /// The own entry got its DA1 reply: gone; the queries written after it
    /// are now the first in line. `position` is set when the position reply
    /// came with it.
    void Consume(const TryResult* position, CprSink& out) noexcept;
    void Fail(CprSink& out) noexcept;
    void ExpireCounts(Timestamp now) noexcept;
    void StripHeld(std::size_t length, CprSink& out) const noexcept;

    [[nodiscard]] static TryResult Scan(const std::byte* data,
                                        std::size_t size) noexcept;
    [[nodiscard]] static EDa1Look LookDa1(const std::byte* data,
                                          std::size_t size,
                                          std::size_t& length) noexcept;

    void Step(std::byte b, bool forwarded, Timestamp at, CprSink& out) noexcept;
    void Resolve(CprSink& out) noexcept;
    void FlushHeld(CprSink& out) noexcept;
    void Reprocess(std::size_t from, CprSink& out) noexcept;
    void ForwardHeld(std::size_t count, CprSink& out) noexcept;
    void Settle(Timestamp now, CprSink& out) noexcept;
    void AbandonLive() noexcept;

    std::unique_ptr<grid::Grid> Grid_;
    grid::VtParser Parser_;
    std::uint64_t SeenDa1_{0};

    EOwn Own_{EOwn::None};
    Timestamp TimeoutAt_;
    Timestamp ForgetAt_;
    /// Program DA1 queries written before / after the own entry, and when the
    /// newest of each was written.
    std::uint32_t Ahead_{0};
    std::uint32_t Behind_{0};
    Timestamp AheadAt_;
    Timestamp BehindAt_;

    bool Silent_{false};
    bool Off_{false};

    // A possible beginning of an answer: the tail of the stream, in order,
    // with the time each byte arrived. The first HeldForwarded_ bytes already
    // went to the agent (the hold ran out, or nothing is to be held): they
    // stay only to recognise a reply or a bracketed paste that starts there.
    std::array<std::byte, CPR_HELD_MAX> Held_{};
    std::array<Timestamp, CPR_HELD_MAX> HeldAt_{};
    std::size_t HeldLen_{0};
    std::size_t HeldForwarded_{0};
    Timestamp Now_;

    bool InPaste_{false};
    std::uint8_t PasteEndMatched_{0};
};

}  // namespace zet::predict
