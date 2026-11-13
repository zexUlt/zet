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

// The classes that get a verdict (design.md §9): Cyrillic of East Asian
// ambiguous width and private use, in the BMP and in planes 15-16 apart. Every
// other glyph that is not a reference glyph keeps the doubt of step 5 for the
// whole session.
constexpr std::array<grid::EWidthClass, 3> VERDICT_CLASSES{
    grid::EWidthClass::Cyrillic, grid::EWidthClass::PrivateUseBmp,
    grid::EWidthClass::PrivateUsePlanes};

/// Index into VERDICT_CLASSES; its size for a class without a verdict.
constexpr std::size_t VerdictIndex(grid::EWidthClass cls) noexcept {
    for (std::size_t k = 0; k < VERDICT_CLASSES.size(); ++k) {
        if (VERDICT_CLASSES[k] == cls) return k;
    }
    return VERDICT_CLASSES.size();
}

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

Calibration::Calibration(std::unique_ptr<CprDemux> demux, bool seeded,
                         const CalibrationTarget& target,
                         std::size_t lagMax) noexcept
    : Demux_(std::move(demux)), LagMax_(lagMax) {
    if (!seeded) {
        Reasons_ = CalStart;
    }
    if (target.Valid()) {
        Rebase(target);
    }
}

std::unique_ptr<Calibration> Calibration::Create(
    bool seeded, const CalibrationTarget& target, std::size_t lagMax) noexcept {
    auto demux = CprDemux::Create();
    if (!demux) {
        return nullptr;
    }
    return std::unique_ptr<Calibration>(new (std::nothrow) Calibration(
        std::move(demux), seeded, target, lagMax));
}

// ---------------------------------------------------------------------------
// Reasons
// ---------------------------------------------------------------------------

bool Calibration::MirrorReady(const grid::Grid& g,
                              const grid::VtParser& p) noexcept {
    return p.State() == grid::EParserState::Ground && !p.Utf8Pending() &&
           !g.IsAltScreen() && !g.PendingWrap() && !g.AnyUnmodelledMode() &&
           !g.ModeSet(grid::EMode::Decom) &&
           !g.ModeSet(grid::EMode::SyncOutput);
}

void Calibration::Rebase(const CalibrationTarget& target) noexcept {
    const grid::VtParser& p = *target.Parser;
    SeenUnknown_ = p.UnknownSeqCount();
    SeenDcs_ = p.DcsApcCount() - p.DcsQueryCount();
    SeenBypass_ = p.BypassLimitCount();
    SeenAltExit_ = target.Screen->AltExitCount();
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
    // Counted, not compared at the call boundaries: an enter and an exit
    // inside one portion, or inside the wait, are seen too.
    if (g.AltExitCount() != SeenAltExit_) {
        SeenAltExit_ = g.AltExitCount();
        if (!quiet) {
            Reasons_ |= CalAltExit;
        }
    }
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
            Reasons_ |= CalUnknown;
        }
        if (dcs != SeenDcs_) {
            Reasons_ |= CalDcs;
        }
        if (WidthDue(g)) {
            Reasons_ |= CalWidth;
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
        if (bytes.size() <= LagMax_ - LagLen_) {
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
    // The reason of a portion's end may be gone by now (a CR restarted the
    // path): a width query that has nothing to learn is not sent.
    if ((Reasons_ & CalWidth) != 0 && !WidthDue(*target.Screen)) {
        Reasons_ = static_cast<std::uint8_t>(Reasons_ & ~CalWidth);
        if (Reasons_ == 0) {
            return false;
        }
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
    Lag_.reset(new (std::nothrow) std::byte[LagMax_]);
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
    ExpectKnown_ = (QueryReasons_ & (CalStart | CalResize | CalDcs)) == 0 &&
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
    if (ExpectKnown_ && g.CursorRow() == row) {
        // The rows agree: the answer is about the column. The mirror stood
        // still since the query, so its cursor is the one that was expected.
        const std::uint16_t expected = g.CursorCol();
        const bool colMatch = expected == col;
        // The difference is read as widths only when widths are the only
        // reason to ask: after a sequence the mirror does not know (or a
        // program leaving the alternate screen) the cursor may have moved for
        // a reason that is no glyph's.
        const bool widthOnly = QueryReasons_ == CalWidth && PathSolvable(g);
        // Before the verdicts: a Wide one turns the cursor row foreign. The
        // tail is the terminal's only when the path laid the row out from
        // what stood where the terminal has it (no marks from before the
        // path) and only pushed it right.
        const bool tailKnownBlank = !colMatch && widthOnly && col > expected &&
                                    g.MarksOnPath(row) &&
                                    TailKnownBlank(g, row, expected);
        if (widthOnly) {
            SolveWidth(g, static_cast<std::int64_t>(col) -
                              static_cast<std::int64_t>(expected));
        }
        if (colMatch) {
            // The column is the terminal's: the next path starts here.
            g.ConfirmColumn();
            ++Matched_;
        } else {
            FixColumn(g, row, col, tailKnownBlank);
            ++ColumnFixed_;
        }
        target.Engine->OnCalibrated(!colMatch, now, target.View());
        Thaw(now, target);
        return;
    }
    // The start is the only reason that says the tail of the cursor's row is
    // blank: the shell left the cursor on a fresh line.
    if (g.Resync(row, col, QueryReasons_ == CalStart)) {
        target.Engine->OnCalibrated(true, now, target.View());
        ++Resynced_;
    } else {
        ++Abandoned_;
    }
    Thaw(now, target);
}

// ---------------------------------------------------------------------------
// Widths
// ---------------------------------------------------------------------------

EWidthVerdict Calibration::Verdict(grid::EWidthClass cls) const noexcept {
    const std::size_t k = VerdictIndex(cls);
    return k < Verdicts_.size() ? Verdicts_[k] : EWidthVerdict::Unknown;
}

grid::WidthPolicy Calibration::Policy() const noexcept {
    grid::WidthPolicy policy;
    for (const grid::EWidthClass cls : VERDICT_CLASSES) {
        const EWidthVerdict v = Verdicts_[VerdictIndex(cls)];
        const std::uint8_t width = v == EWidthVerdict::Wide ? 2 : 1;
        if (v == EWidthVerdict::Narrow || v == EWidthVerdict::Wide) {
            policy.DoubtClasses = static_cast<std::uint8_t>(
                policy.DoubtClasses & ~grid::WidthClassBit(cls));
        }
        switch (cls) {
            case grid::EWidthClass::Cyrillic:
                policy.Cyrillic = width;
                break;
            case grid::EWidthClass::PrivateUseBmp:
                policy.PrivateUseBmp = width;
                break;
            case grid::EWidthClass::PrivateUsePlanes:
                policy.PrivateUsePlanes = width;
                break;
            default:
                break;
        }
    }
    return policy;
}

bool Calibration::PathSolvable(const grid::Grid& g) noexcept {
    return !g.IsAltScreen() && !g.AnyUnmodelledMode() && !g.PathBroken() &&
           g.PathForeign() == 0;
}

bool Calibration::WidthDue(const grid::Grid& g) const noexcept {
    if (!PathSolvable(g)) {
        return false;
    }
    for (const grid::EWidthClass cls : VERDICT_CLASSES) {
        if (Verdicts_[VerdictIndex(cls)] == EWidthVerdict::Unknown &&
            g.PathGlyphs(cls) != 0) {
            return true;
        }
    }
    return false;
}

void Calibration::ApplyVerdict(grid::Grid& g,
                               grid::EWidthClass cls) const noexcept {
    g.SetWidthPolicy(Policy());
    switch (Verdicts_[VerdictIndex(cls)]) {
        case EWidthVerdict::Narrow:
            g.ClearWidthClass(cls);
            break;
        case EWidthVerdict::Wide:
            g.ForgetWidthClass(cls);
            break;
        case EWidthVerdict::Unknown:
        case EWidthVerdict::Unsolved:
            break;
    }
}

void Calibration::SolveWidth(grid::Grid& g, std::int64_t d) noexcept {
    // A cursor left of the mirror's is no glyph's doing: it says nothing of a
    // class, nor counts against it. A foreign glyph on the path could hide or
    // fake any difference (WidthDue keeps such paths from being asked about).
    if (g.PathForeign() != 0 || d < 0) {
        return;
    }
    // Glyphs of the classes still open, and the glyphs whose width stays open
    // whatever the answer: classes given up on.
    std::array<std::uint32_t, VERDICT_CLASSES.size()> open{};
    std::uint64_t unresolved = 0;
    std::size_t openClasses = 0;
    std::size_t only = 0;
    for (const grid::EWidthClass cls : VERDICT_CLASSES) {
        const std::size_t k = VerdictIndex(cls);
        const std::uint32_t n = g.PathGlyphs(cls);
        if (n == 0) {
            continue;
        }
        if (Verdicts_[k] == EWidthVerdict::Unknown) {
            open[k] = n;
            ++openClasses;
            only = k;
        } else if (Verdicts_[k] == EWidthVerdict::Unsolved) {
            unresolved += n;
        }
    }
    if (openClasses == 0) {
        return;
    }
    // A glyph is as wide as the mirror drew it or one column wider, so the
    // terminal's cursor is right of the mirror's by between none and one per
    // glyph of open width.
    if (d == 0) {
        for (const grid::EWidthClass cls : VERDICT_CLASSES) {
            const std::size_t k = VerdictIndex(cls);
            if (open[k] != 0) {
                Verdicts_[k] = EWidthVerdict::Narrow;
                ApplyVerdict(g, cls);
            }
        }
        return;
    }
    if (openClasses == 1 && unresolved == 0 &&
        d == static_cast<std::int64_t>(open[only])) {
        Verdicts_[only] = EWidthVerdict::Wide;
        ApplyVerdict(g, VERDICT_CLASSES[only]);
        return;
    }
    for (std::size_t k = 0; k < open.size(); ++k) {
        if (open[k] != 0 && ++SolveFails_[k] >= CPR_SOLVE_FAILS_MAX) {
            Verdicts_[k] = EWidthVerdict::Unsolved;
        }
    }
}

bool Calibration::TailKnownBlank(const grid::Grid& g, std::uint16_t row,
                                 std::uint16_t col) noexcept {
    if (row >= g.Rows() || g.KnownFrom(row) > col) {
        return false;
    }
    for (std::uint16_t c = col; c < g.Cols(); ++c) {
        const char32_t cp = g.CellAt(row, c).Codepoint;
        if (cp != 0 && cp != U' ') {
            return false;
        }
    }
    return true;
}

void Calibration::FixColumn(grid::Grid& g, std::uint16_t row, std::uint16_t col,
                            bool tailKnownBlank) noexcept {
    // The terminal's cursor is where it says; the mirror follows it.
    g.MoveCursorToCol(static_cast<std::uint16_t>(col + 1));
    if (tailKnownBlank) {
        // The tail from the answered column is blank there too. What the
        // mirror drew left of it is the terminal's, in places it cannot name.
        g.EraseInLine(0);
        g.SetKnownFrom(row, col);
    } else {
        g.SetKnownFrom(row, g.Cols());
    }
    // Nothing the row's marks say can be checked any more.
    for (std::size_t k = 0; k < grid::WIDTH_CLASS_COUNT; ++k) {
        g.ClearWidthClass(row, static_cast<grid::EWidthClass>(k));
    }
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
        // The grid is a fresh one. The client laid the snapshot out with
        // Policy() already (Mirror::ResetFromSnapshot), as the terminal did,
        // so no row needs forgetting: the verdicts are the session's, and a
        // grid built any other way gets the policy here.
        target.Screen->SetWidthPolicy(Policy());
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
    Reasons_ |= CalResize;
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
