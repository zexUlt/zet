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
/// The terminal answers in the order of the queries, so an answer belongs to
/// the head of a queue of expected answers. The queue holds the client's own
/// queries and the remote program's `CSI 6 n` and DA1, which the demultiplexer
/// counts itself in the outgoing stream with a private VtParser.
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
    /// remote program's `CSI 6 n` and DA1 in them are put in the queue, each
    /// on its last byte, never inside an OSC or DCS string.
    void OnOutput(ByteSpan bytes) noexcept;

    /// The client wrote its own query. False, and nothing happens, when one is
    /// already pending (the client asks for at most one at a time). A query
    /// after the queue was forgotten switches the demultiplexer on again.
    bool OnQuerySent(Timestamp now) noexcept;

    /// Bytes read from stdin. Reports the bytes for the agent and the events
    /// to `out`. A possible beginning of the client's own answer at the end
    /// of `bytes` is held while the query is pending, for CPR_HOLD at most.
    void OnInput(ByteSpan bytes, Timestamp now, CprSink& out) noexcept;

    /// Time passed: releases what is held, expires the pending query, forgets
    /// the queue. Also done at the start of every OnInput.
    void OnTick(Timestamp now, CprSink& out) noexcept;

    /// The pending query is abandoned (a snapshot): its answer, when it comes,
    /// is stripped and thrown away, and no event is raised for it.
    void OnCancel(Timestamp now, CprSink& out) noexcept;

    /// The local terminal changed size: the pending query is abandoned as in
    /// OnCancel, and the silent mark is cleared so that the client may ask
    /// again.
    void OnResize(Timestamp now, CprSink& out) noexcept;

    /// The earliest moment OnTick changes something: end of the hold, of the
    /// pending query, or of the wait for a late answer. Unset when there is
    /// none. The client adds it to its poll deadline.
    [[nodiscard]] std::optional<Timestamp> NextDeadline() const noexcept;

    /// The terminal did not answer in time (or answered wrongly) and the
    /// client has not been told to ask again by a resize.
    [[nodiscard]] bool Silent() const noexcept { return Silent_; }
    /// The client's own query is waiting for its answer.
    [[nodiscard]] bool Pending() const noexcept;
    /// Some entry of the client's own is in the queue, pending or not: while
    /// there is none, stdin passes through unchanged.
    [[nodiscard]] bool Outstanding() const noexcept;
    /// The queue was forgotten and nothing is parsed.
    [[nodiscard]] bool Off() const noexcept { return Off_; }

    [[nodiscard]] std::size_t QueueSize() const noexcept { return QueueLen_; }
    /// Bytes read from stdin and not yet passed on.
    [[nodiscard]] std::size_t HeldSize() const noexcept;
    [[nodiscard]] ByteSpan HeldBytes() const noexcept {
        return {Held_.data() + HeldForwarded_, HeldLen_ - HeldForwarded_};
    }

private:
    enum class EKind : std::uint8_t { ProgramCpr, ProgramDa1, Own };

    struct Entry {
        EKind Kind{EKind::Own};
        /// Own only: the answer is wanted (false after a deadline or cancel).
        bool Live{false};
        /// Program only: queries in a row of this kind.
        std::uint32_t Count{0};
    };

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

    void Push(EKind kind) noexcept;
    void DropFor(std::size_t room) noexcept;
    void Erase(std::size_t index) noexcept;
    /// Index of the first entry that can take an answer of this kind.
    [[nodiscard]] std::optional<std::size_t> Target(
        bool position) const noexcept;
    /// Removes entries up to and including `target`'s one answer; own live
    /// entries passed over raise Failed.
    Entry PopThrough(std::size_t target, CprSink& out) noexcept;
    void Fail(CprSink& out) noexcept;
    void StripHeld(std::size_t length, CprSink& out) const noexcept;

    [[nodiscard]] static TryResult Scan(const std::byte* data,
                                        std::size_t size) noexcept;
    [[nodiscard]] static EDa1Look LookDa1(const std::byte* data,
                                          std::size_t size,
                                          std::size_t& length) noexcept;

    void Step(std::byte b, bool forwarded, CprSink& out) noexcept;
    void Resolve(CprSink& out) noexcept;
    void FlushHeld(CprSink& out) noexcept;
    void Reprocess(std::size_t from, CprSink& out) noexcept;
    void ForwardHeld(std::size_t count, CprSink& out) noexcept;
    void Settle(Timestamp now, CprSink& out) noexcept;
    void AbandonLive(CprSink& out) noexcept;

    std::unique_ptr<grid::Grid> Grid_;
    grid::VtParser Parser_;
    std::uint64_t SeenDsr6_{0};
    std::uint64_t SeenDa1_{0};

    std::array<Entry, CPR_QUEUE_MAX> Queue_{};
    std::size_t QueueLen_{0};

    bool Silent_{false};
    bool Off_{false};
    Timestamp TimeoutAt_;
    Timestamp ForgetAt_;

    // A possible beginning of an answer: the tail of the stream, in order.
    // The first HeldForwarded_ bytes already went to the agent (the hold ran
    // out): they stay only to recognise a bracketed paste that starts there.
    std::array<std::byte, CPR_HELD_MAX> Held_{};
    std::size_t HeldLen_{0};
    std::size_t HeldForwarded_{0};
    Timestamp HeldSince_;
    Timestamp Now_;

    bool InPaste_{false};
    std::uint8_t PasteEndMatched_{0};
};

}  // namespace zet::predict
