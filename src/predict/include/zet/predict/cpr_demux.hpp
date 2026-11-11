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
    /// The query got no position: the terminal answered the end marker
    /// alone, or our position had already gone to the agent when its hold
    /// ran out. A lost answer is not this, it raises TimedOut.
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
/// A count ages by its oldest query: once it has not been zero for
/// CPR_TIMEOUT + CPR_FORGET, one query is dropped and the clock restarts (a
/// lost reply). The program's `CSI 6 n` is not tracked at all. There is at most
/// one own entry, live (before CPR_TIMEOUT) or dead (timed out or cancelled,
/// until CPR_FORGET), so the state does not grow with the number of queries.
///
/// Contract with the client (the stdout writer and the stdin reader):
///  - OnOutput and OnQuerySent are called at the moment the bytes are handed
///    to write(2) on stdout, in stdout byte order. Then `ahead` and `behind`
///    are right, and the timeout starts when the terminal can see the query.
///  - OnOutput gets the agent's bytes only, never the client's own query.
///  - The client checks CanQuery before it enqueues a query, and calls
///    OnQuerySent when that query is written.
///  - Silent() is not checked here: whether to ask while the terminal is
///    silent is the client's policy.
///  - After a resize, while the dead entry still exists, the client checks
///    CanQuery again after each OnInput and OnTick.
///  - The sink's callbacks must not call back into the demultiplexer.
///
/// Call order, per event, by the client:
///   query about to be enqueued            CanQuery
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

    /// Settles to `now` first, then tells whether a query may be sent: no own
    /// entry exists, live or dead. Off does not matter: a forgotten entry is
    /// gone, and the client's policy decides whether to ask a silent terminal.
    [[nodiscard]] bool CanQuery(Timestamp now, CprSink& out) noexcept;

    /// The client wrote its own query. Settles first. False, and nothing
    /// happens, while an own entry exists: the client asks for at most one at
    /// a time, and after a timeout it waits for the late answer or
    /// CPR_FORGET.
    bool OnQuerySent(Timestamp now, CprSink& out) noexcept;

    /// Bytes read from stdin. Reports the bytes for the agent and the events
    /// to `out`. A possible beginning of the client's own answer at the end
    /// of `bytes` is held while an own entry exists and no DA1 reply of the
    /// program is expected before it, for CPR_HOLD at most from its first
    /// byte. A complete position reply is held longer while the entry is
    /// live: until its DA1 reply comes, a byte that is not one arrives (then
    /// it is a key or the program's answer, and goes at once), or the entry's
    /// CPR_TIMEOUT passes; from there CPR_HOLD applies. So Shift-, Ctrl- and
    /// Alt-F3 pressed while a query waits can be delayed up to CPR_TIMEOUT.
    /// While Off, program DA1 replies are still recognised, to keep the
    /// count, and stdin is passed on unchanged.
    void OnInput(ByteSpan bytes, Timestamp now, CprSink& out) noexcept;

    /// Time passed: releases what is held, expires the live entry, forgets
    /// the dead one (which releases whatever is still held, even before its
    /// CPR_HOLD). Also done at the start of every call that takes a sink.
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

    // Observers, for the tests and the fuzzer; the client does not use them.

    /// Program DA1 queries written before / after the own entry and not yet
    /// answered.
    [[nodiscard]] std::uint32_t Ahead() const noexcept { return Ahead_; }
    [[nodiscard]] std::uint32_t Behind() const noexcept { return Behind_; }
    /// DA1 queries of the program seen in the output so far, answered or not
    /// and never aged out: a plain parser fed the same output byte by byte
    /// counts as many (the Ground fast path of OnOutput skips bytes).
    [[nodiscard]] std::uint64_t Da1Seen() const noexcept { return SeenDa1_; }
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
    /// Releases the held bytes whose hold ran out.
    void FlushDue(Timestamp now, CprSink& out) noexcept;
    /// The held bytes start with a complete position reply.
    [[nodiscard]] bool HoldsPosition() const noexcept;
    /// When the unforwarded held bytes must go to the agent.
    [[nodiscard]] Timestamp HoldDeadline() const noexcept;
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
    /// When each count last left zero: its oldest query is no older. A count
    /// that stays non-zero for CPR_TIMEOUT + CPR_FORGET holds a lost reply.
    Timestamp AheadSince_;
    Timestamp BehindSince_;

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

    bool InPaste_{false};
    std::uint8_t PasteEndMatched_{0};
};

}  // namespace zet::predict
