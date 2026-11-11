#include "zet/predict/calibration.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace zet::predict {

namespace {

constexpr std::array<std::byte, Calibration::QUERY_SIZE> QUERY_BYTES{
    std::byte{0x1b}, std::byte{'['}, std::byte{'6'}, std::byte{'n'},
    std::byte{0x1b}, std::byte{'['}, std::byte{'c'}};

}  // namespace

ByteSpan Calibration::Query() noexcept { return ByteSpan{QUERY_BYTES}; }

void Calibration::Sink::Forward(ByteSpan bytes) noexcept {
    // The bound holds by the contract (INPUT_MAX per read, the client takes
    // the bytes between reads); a byte past it is dropped rather than written.
    const std::size_t room = Out.size() - OutLen;
    const std::size_t n = std::min(room, bytes.size());
    if (n != 0) {
        std::memcpy(Out.data() + OutLen, bytes.data(), n);
    }
    OutLen += n;
}

void Calibration::Sink::Event(const CprEvent& event) noexcept {
    if (EventCount < Events.size()) {
        Events[EventCount++] = event;
    }
}

std::unique_ptr<Calibration> Calibration::Create(
    bool seeded, const CalibrationTarget& target) noexcept {
    auto demux = CprDemux::Create();
    if (!demux) {
        return nullptr;
    }
    std::unique_ptr<Calibration> self(new (std::nothrow)
                                          Calibration(std::move(demux)));
    if (!self) {
        return nullptr;
    }
    if (!seeded) {
        self->Reasons_ = CAL_START;
    }
    if (target.Valid()) {
        self->Rebase(target);
    }
    return self;
}

// ---------------------------------------------------------------------------
// Reasons
// ---------------------------------------------------------------------------

bool Calibration::MirrorReady(const grid::Grid& g,
                              const grid::VtParser& p) noexcept {
    return p.State() == grid::EParserState::Ground && !p.Utf8Pending() &&
           !g.IsAltScreen() && !g.PendingWrap() &&
           !g.ModeSet(grid::EMode::Decom) &&
           !g.ModeSet(grid::EMode::SyncOutput);
}

void Calibration::Rebase(const CalibrationTarget& target) noexcept {
    const grid::VtParser& p = *target.Parser;
    SeenUnknown_ = p.UnknownSeqCount();
    SeenDcs_ = p.DcsApcCount() - p.DcsQueryCount();
    SeenBypass_ = p.BypassLimitCount();
    PrevAlt_ = target.Screen->IsAltScreen();
}

void Calibration::Evaluate(const CalibrationTarget& target,
                           bool quiet) noexcept {
    const grid::Grid& g = *target.Screen;
    const grid::VtParser& p = *target.Parser;
    const bool alt = g.IsAltScreen();
    if (p.BypassLimitCount() != SeenBypass_) {
        SeenBypass_ = p.BypassLimitCount();
        Sticky_ = true;
    }
    if (PrevAlt_ && !alt && !quiet) {
        Reasons_ |= CAL_ALT_EXIT;
    }
    PrevAlt_ = alt;
    // A string or a character in progress is judged when it is over.
    if (p.State() != grid::EParserState::Ground || p.Utf8Pending()) {
        return;
    }
    // DECRQSS and XTGETTCAP strings are questions to the terminal and change
    // nothing on the screen.
    const std::uint64_t dcs = p.DcsApcCount() - p.DcsQueryCount();
    const std::uint64_t unknown = p.UnknownSeqCount();
    if (!alt && !quiet) {
        if (unknown != SeenUnknown_) {
            Reasons_ |= CAL_UNKNOWN;
        }
        if (dcs != SeenDcs_) {
            Reasons_ |= CAL_DCS;
        }
    }
    SeenUnknown_ = unknown;
    SeenDcs_ = dcs;
}

// ---------------------------------------------------------------------------
// The terminal side
// ---------------------------------------------------------------------------

void Calibration::OnTerminalBytes(ByteSpan bytes, std::uint64_t outputEnd,
                                  Timestamp now,
                                  const CalibrationTarget& target,
                                  bool notice) noexcept {
    if (!target.Valid()) {
        return;
    }
    if (Frozen_) {
        if (bytes.size() <= CPR_LAG_BYTES_MAX - LagLen_) {
            if (!bytes.empty()) {
                std::memcpy(Lag_.get() + LagLen_, bytes.data(), bytes.size());
            }
            LagLen_ += bytes.size();
            LagEnd_ = std::max(LagEnd_, outputEnd);
            LagHasEnd_ = true;
            return;
        }
        // No room: no calibration. The held output goes in and the query's
        // answer, when it comes, is stripped and ignored.
        GiveUp(now, target);
    }
    target.Parser->Feed(bytes.data(), bytes.size(), *target.Screen);
    target.Engine->OnOutput(outputEnd, now, target.View());
    Evaluate(target, notice);
}

void Calibration::OnEchoAck(const EchoAck& ack, Timestamp now,
                            const CalibrationTarget& target) noexcept {
    if (!target.Valid()) {
        return;
    }
    if (Frozen_) {
        DeferredAck_ = ack;
        return;
    }
    target.Engine->OnEchoAck(ack, now, target.View());
}

void Calibration::Thaw(Timestamp now,
                       const CalibrationTarget& target) noexcept {
    Frozen_ = false;
    if (target.Valid()) {
        if (LagLen_ != 0) {
            target.Parser->Feed(Lag_.get(), LagLen_, *target.Screen);
        }
        if (LagHasEnd_) {
            target.Engine->OnOutput(LagEnd_, now, target.View());
        }
        target.Engine->SetLagging(false);
        if (DeferredAck_) {
            target.Engine->OnEchoAck(*DeferredAck_, now, target.View());
        }
    }
    DeferredAck_.reset();
    Lag_.reset();
    LagLen_ = 0;
    LagHasEnd_ = false;
    if (target.Valid()) {
        Evaluate(target, false);
    }
}

void Calibration::GiveUp(Timestamp now,
                         const CalibrationTarget& target) noexcept {
    ++Abandoned_;
    if (QueryUnwritten_) {
        AbandonOnWrite_ = true;
    }
    Sink_.EventCount = 0;
    Demux_->OnCancel(now, Sink_);
    Sink_.EventCount = 0;
    Thaw(now, target);
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

bool Calibration::QueryDue(Timestamp now,
                           const CalibrationTarget& target) noexcept {
    if (Detached_ || Disabled_ || Sticky_ || Frozen_ || QueryUnwritten_ ||
        Reasons_ == 0 || !target.Valid()) {
        return false;
    }
    if (!MirrorReady(*target.Screen, *target.Parser)) {
        return false;
    }
    // A silent terminal is not asked again until a resize clears the mark.
    if (Demux_->Silent()) {
        return false;
    }
    const bool allowed = Demux_->CanQuery(now, Sink_);
    Dispatch(now, target);
    return allowed;
}

bool Calibration::BeginQuery(Timestamp /*now*/,
                             const CalibrationTarget& target) noexcept {
    if (!target.Valid()) {
        return false;
    }
    Lag_.reset(new (std::nothrow) std::byte[CPR_LAG_BYTES_MAX]);
    if (!Lag_) {
        Disabled_ = true;
        return false;
    }
    const grid::Grid& g = *target.Screen;
    Frozen_ = true;
    QueryUnwritten_ = true;
    LagLen_ = 0;
    LagHasEnd_ = false;
    DeferredAck_.reset();
    QueryReasons_ = Reasons_;
    Reasons_ = 0;
    // Where the mirror says the cursor is, when that is worth comparing: the
    // reasons that cast the rows in doubt are not asking "is the mirror
    // right?" but "where are we?".
    ExpectKnown_ = (QueryReasons_ & (CAL_START | CAL_RESIZE | CAL_DCS)) == 0 &&
                   g.RowsTrusted();
    ExpectRow_ = g.CursorRow();
    ExpectCol_ = g.CursorCol();
    target.Engine->SetLagging(true);
    return true;
}

void Calibration::AbortQuery(Timestamp now,
                             const CalibrationTarget& target) noexcept {
    if (!Frozen_ || !QueryUnwritten_) {
        return;
    }
    QueryUnwritten_ = false;
    Reasons_ |= QueryReasons_;
    Thaw(now, target);
}

void Calibration::OnWritten(ByteSpan bytes, Timestamp now) noexcept {
    Demux_->OnOutput(bytes, now);
}

void Calibration::OnQueryWritten(Timestamp now,
                                 const CalibrationTarget& target) noexcept {
    QueryUnwritten_ = false;
    Sink_.EventCount = 0;
    const bool sent = Demux_->OnQuerySent(now, Sink_);
    Dispatch(now, target);
    if (AbandonOnWrite_) {
        // The query was given up while it waited in the queue: the terminal
        // will still answer it.
        AbandonOnWrite_ = false;
        if (sent) {
            Demux_->OnCancel(now, Sink_);
            Dispatch(now, target);
        }
    } else if (!sent && Frozen_) {
        // An own entry already exists: this query will not be recognised.
        GiveUp(now, target);
    }
}

void Calibration::Dispatch(Timestamp now,
                           const CalibrationTarget& target) noexcept {
    if (Sink_.EventCount == 0) {
        return;
    }
    std::array<CprEvent, Sink::EVENTS_MAX> events = Sink_.Events;
    const std::size_t count = Sink_.EventCount;
    Sink_.EventCount = 0;
    for (std::size_t i = 0; i < count; ++i) {
        // An event for a wait that is over, or that never began.
        if (!Frozen_ || QueryUnwritten_ || !target.Valid()) {
            continue;
        }
        if (events[i].Kind == ECprEvent::Answered) {
            Apply(events[i], now, target);
        } else {
            ++Abandoned_;
            Thaw(now, target);
        }
    }
}

void Calibration::Apply(const CprEvent& event, Timestamp now,
                        const CalibrationTarget& target) noexcept {
    grid::Grid& g = *target.Screen;
    const auto row =
        static_cast<std::uint16_t>(event.Row > 0 ? event.Row - 1 : 0);
    const auto col =
        static_cast<std::uint16_t>(event.Col > 0 ? event.Col - 1 : 0);
    const bool match =
        ExpectKnown_ && g.CursorRow() == row && g.CursorCol() == col;
    bool resynced = false;
    if (!match) {
        // The start is the only reason that says the tail of the cursor's row
        // is blank: the shell left the cursor on a fresh line.
        resynced = g.Resync(row, col, QueryReasons_ == CAL_START);
    }
    if (match || resynced) {
        target.Engine->OnCalibrated(resynced, now, target.View());
        if (resynced) {
            ++Resynced_;
        } else {
            ++Matched_;
        }
    } else {
        ++Abandoned_;
    }
    Thaw(now, target);
}

// ---------------------------------------------------------------------------
// Stdin
// ---------------------------------------------------------------------------

void Calibration::OnInput(ByteSpan typed, Timestamp now,
                          const CalibrationTarget& target) noexcept {
    Demux_->OnInput(typed.first(std::min(typed.size(), INPUT_MAX)), now, Sink_);
    Dispatch(now, target);
}

ByteSpan Calibration::Forwarded() const noexcept {
    return ByteSpan{Sink_.Out.data(), Sink_.OutLen};
}

void Calibration::ClearForwarded() noexcept { Sink_.OutLen = 0; }

void Calibration::OnTick(Timestamp now,
                         const CalibrationTarget& target) noexcept {
    Demux_->OnTick(now, Sink_);
    Dispatch(now, target);
}

std::optional<Timestamp> Calibration::NextDeadline() const noexcept {
    return Demux_->NextDeadline();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void Calibration::OnSnapshot(Timestamp now,
                             const CalibrationTarget& target) noexcept {
    if (Frozen_) {
        // The snapshot replaces the grid the held output would go into.
        ++Abandoned_;
        Frozen_ = false;
        Lag_.reset();
        LagLen_ = 0;
        LagHasEnd_ = false;
        DeferredAck_.reset();
        if (target.Valid()) {
            target.Engine->SetLagging(false);
        }
    }
    if (QueryUnwritten_) {
        AbandonOnWrite_ = true;
    }
    Demux_->OnCancel(now, Sink_);
    Sink_.EventCount = 0;
    Reasons_ = 0;
    Sticky_ = false;
    if (target.Valid()) {
        Rebase(target);
    }
}

void Calibration::OnResize(Timestamp now,
                           const CalibrationTarget& target) noexcept {
    if (Frozen_) {
        ++Abandoned_;
        if (QueryUnwritten_) {
            AbandonOnWrite_ = true;
        }
        Thaw(now, target);
    } else if (QueryUnwritten_) {
        AbandonOnWrite_ = true;
    }
    Demux_->OnResize(now, Sink_);
    Sink_.EventCount = 0;
    Reasons_ |= CAL_RESIZE;
}

void Calibration::Detach(Timestamp now) noexcept {
    Detached_ = true;
    Frozen_ = false;
    Lag_.reset();
    LagLen_ = 0;
    LagHasEnd_ = false;
    DeferredAck_.reset();
    Reasons_ = 0;
    if (QueryUnwritten_) {
        AbandonOnWrite_ = true;
    }
    Demux_->OnCancel(now, Sink_);
    Sink_.EventCount = 0;
}

}  // namespace zet::predict
