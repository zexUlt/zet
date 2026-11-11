#include "zet/predict/cpr_demux.hpp"

#include <algorithm>
#include <cstdint>
#include <new>
#include <utility>

namespace zet::predict {

namespace {

constexpr std::byte ESC{0x1b};

/// ESC [ 2 0 1 ~ ends a bracketed paste.
constexpr std::array<std::byte, 6> PASTE_END{ESC,
                                             std::byte{'['},
                                             std::byte{'2'},
                                             std::byte{'0'},
                                             std::byte{'1'},
                                             std::byte{'~'}};

constexpr bool IsDigit(std::byte b) noexcept {
    return b >= std::byte{'0'} && b <= std::byte{'9'};
}

constexpr std::uint32_t DigitValue(std::byte b) noexcept {
    return static_cast<std::uint32_t>(b) - '0';
}

}  // namespace

std::unique_ptr<CprDemux> CprDemux::Create() noexcept {
    auto grid = grid::Grid::Create(1, 1, 0);
    if (!grid.has_value()) {
        return nullptr;
    }
    return std::unique_ptr<CprDemux>(
        new (std::nothrow) CprDemux(std::unique_ptr<grid::Grid>(grid.value())));
}

bool CprDemux::Pending() const noexcept {
    return std::any_of(
        Queue_.begin(), Queue_.begin() + QueueLen_,
        [](const Entry& e) { return e.Kind == EKind::Own && e.Live; });
}

bool CprDemux::Outstanding() const noexcept {
    return std::any_of(Queue_.begin(), Queue_.begin() + QueueLen_,
                       [](const Entry& e) { return e.Kind == EKind::Own; });
}

std::size_t CprDemux::HeldSize() const noexcept {
    return HeldLen_ - HeldForwarded_;
}

// ---------------------------------------------------------------------------
// The queue
// ---------------------------------------------------------------------------

void CprDemux::Erase(std::size_t index) noexcept {
    for (std::size_t i = index + 1; i < QueueLen_; ++i) {
        Queue_[i - 1] = Queue_[i];
    }
    --QueueLen_;
}

void CprDemux::DropFor(std::size_t) noexcept {
    // Program entries first, then own entries that are no longer wanted. The
    // pending one is never the victim: there is at most one of it.
    for (std::size_t i = 0; i < QueueLen_; ++i) {
        if (Queue_[i].Kind != EKind::Own) {
            Erase(i);
            return;
        }
    }
    for (std::size_t i = 0; i < QueueLen_; ++i) {
        if (!Queue_[i].Live) {
            Erase(i);
            return;
        }
    }
    Erase(0);
}

void CprDemux::Push(EKind kind) noexcept {
    if (kind != EKind::Own && QueueLen_ > 0 &&
        Queue_[QueueLen_ - 1].Kind == kind) {
        auto& count = Queue_[QueueLen_ - 1].Count;
        if (count != UINT32_MAX) {
            ++count;
        }
        return;
    }
    if (QueueLen_ == Queue_.size()) {
        DropFor(1);
    }
    Entry e;
    e.Kind = kind;
    e.Live = kind == EKind::Own;
    e.Count = kind == EKind::Own ? 0 : 1;
    Queue_[QueueLen_++] = e;
}

std::optional<std::size_t> CprDemux::Target(bool position) const noexcept {
    const EKind want = position ? EKind::ProgramCpr : EKind::ProgramDa1;
    for (std::size_t i = 0; i < QueueLen_; ++i) {
        if (Queue_[i].Kind == EKind::Own || Queue_[i].Kind == want) {
            return i;
        }
    }
    return std::nullopt;
}

void CprDemux::Fail(CprOutput& out) noexcept {
    out.Events.push_back(CprEvent{ECprEvent::Failed, 0, 0});
    Silent_ = true;
}

CprDemux::Entry CprDemux::PopThrough(std::size_t target,
                                     CprOutput& out) noexcept {
    // The entries in front of the one the answer belongs to were never
    // answered. A pending query of ours among them is unanswered, as by the
    // deadline.
    for (std::size_t i = 0; i < target; ++i) {
        if (Queue_[i].Kind == EKind::Own && Queue_[i].Live) {
            Fail(out);
        }
    }
    const Entry taken = Queue_[target];
    std::size_t keepFrom = target;
    if (taken.Kind != EKind::Own && taken.Count > 1) {
        // One query of a run answered: the run stays, shorter.
        Queue_[target].Count = taken.Count - 1;
    } else {
        keepFrom = target + 1;
    }
    const std::size_t drop = keepFrom;
    std::size_t w = 0;
    for (std::size_t i = drop; i < QueueLen_; ++i) {
        Queue_[w++] = Queue_[i];
    }
    QueueLen_ = w;
    return taken;
}

void CprDemux::Strip(std::size_t length, CprOutput& out) const noexcept {
    CprStrip strip;
    strip.ForwardAt = out.Forward.size();
    strip.Length = length;
    std::copy(Held_.begin(), Held_.begin() + length, strip.Bytes.begin());
    out.Strips.push_back(strip);
}

// ---------------------------------------------------------------------------
// Recognising answers
// ---------------------------------------------------------------------------

CprDemux::TryResult CprDemux::Scan(const std::byte* d, std::size_t n) noexcept {
    TryResult r;
    if (n < 2) {
        r.Status = ETry::NeedMore;
        return r;
    }
    if (d[1] != std::byte{'['}) {
        return r;  // Fail
    }
    if (n < 3) {
        r.Status = ETry::NeedMore;
        return r;
    }
    if (d[2] == std::byte{'?'}) {
        // DA1 answer: CSI ? Pm c.
        for (std::size_t i = 3; i < n; ++i) {
            if (i + 1 > CPR_DA1_REPLY_MAX) {
                return r;
            }
            if (IsDigit(d[i]) || d[i] == std::byte{';'}) {
                continue;
            }
            if (d[i] == std::byte{'c'}) {
                r.Status = ETry::Da1;
                r.Length = i + 1;
                return r;
            }
            return r;
        }
        if (n >= CPR_DA1_REPLY_MAX) {
            return r;
        }
        r.Status = ETry::NeedMore;
        return r;
    }
    if (!IsDigit(d[2])) {
        return r;
    }
    // CSI Pr ; Pc R (position) or CSI 200 ~ (bracketed paste start).
    std::size_t i = 2;
    std::uint32_t first = 0;
    std::size_t digits = 0;
    while (i < n && IsDigit(d[i])) {
        if (++digits > 5) {
            return r;
        }
        first = first * 10 + DigitValue(d[i]);
        ++i;
    }
    if (i == n) {
        r.Status = ETry::NeedMore;
        return r;
    }
    if (d[i] == std::byte{'~'}) {
        if (digits == 3 && first == 200) {
            r.Status = ETry::PasteStart;
            r.Length = i + 1;
        }
        return r;
    }
    if (d[i] != std::byte{';'}) {
        return r;
    }
    ++i;
    std::uint32_t second = 0;
    digits = 0;
    while (i < n && IsDigit(d[i])) {
        if (++digits > 5) {
            return r;
        }
        second = second * 10 + DigitValue(d[i]);
        ++i;
    }
    if (i == n) {
        r.Status = ETry::NeedMore;
        return r;
    }
    if (digits == 0 || d[i] != std::byte{'R'}) {
        return r;
    }
    if (first < 1 || first > 0xffff || second < 1 || second > 0xffff) {
        return r;
    }
    r.Status = ETry::Position;
    r.Length = i + 1;
    r.Row = static_cast<std::uint16_t>(first);
    r.Col = static_cast<std::uint16_t>(second);
    return r;
}

CprDemux::EDa1Look CprDemux::LookDa1(const std::byte* d, std::size_t n,
                                     std::size_t& length) noexcept {
    if (n == 0) {
        return EDa1Look::Partial;
    }
    if (d[0] != ESC) {
        return EDa1Look::No;
    }
    if (n == 1) {
        return EDa1Look::Partial;
    }
    if (d[1] != std::byte{'['}) {
        return EDa1Look::No;
    }
    if (n == 2) {
        return EDa1Look::Partial;
    }
    if (d[2] != std::byte{'?'}) {
        return EDa1Look::No;
    }
    for (std::size_t i = 3; i < n; ++i) {
        if (i + 1 > CPR_DA1_REPLY_MAX) {
            return EDa1Look::No;
        }
        if (IsDigit(d[i]) || d[i] == std::byte{';'}) {
            continue;
        }
        if (d[i] == std::byte{'c'}) {
            length = i + 1;
            return EDa1Look::Full;
        }
        return EDa1Look::No;
    }
    return n >= CPR_DA1_REPLY_MAX ? EDa1Look::No : EDa1Look::Partial;
}

// ---------------------------------------------------------------------------
// Walking stdin
// ---------------------------------------------------------------------------

void CprDemux::ForwardHeld(std::size_t count, CprOutput& out) noexcept {
    if (HeldForwarded_ < count) {
        out.Forward.insert(out.Forward.end(), Held_.begin() + HeldForwarded_,
                           Held_.begin() + count);
        HeldForwarded_ = count;
    }
}

void CprDemux::FlushHeld(CprOutput& out) noexcept {
    ForwardHeld(HeldLen_, out);
}

void CprDemux::Reprocess(std::size_t from, CprOutput& out) noexcept {
    std::array<std::byte, CPR_HELD_MAX> rest{};
    const std::size_t n = HeldLen_ - from;
    std::copy(Held_.begin() + from, Held_.begin() + HeldLen_, rest.begin());
    const std::size_t forwarded =
        HeldForwarded_ > from ? HeldForwarded_ - from : 0;
    HeldLen_ = 0;
    HeldForwarded_ = 0;
    for (std::size_t i = 0; i < n; ++i) {
        Step(rest[i], i < forwarded, out);
    }
}

void CprDemux::Step(std::byte b, bool forwarded, CprOutput& out) noexcept {
    if (InPaste_) {
        if (!forwarded) {
            out.Forward.push_back(b);
        }
        if (b == ESC) {
            PasteEndMatched_ = 1;
        } else if (PasteEndMatched_ > 0 && b == PASTE_END[PasteEndMatched_]) {
            ++PasteEndMatched_;
        } else {
            PasteEndMatched_ = 0;
        }
        if (PasteEndMatched_ == PASTE_END.size()) {
            InPaste_ = false;
            PasteEndMatched_ = 0;
        }
        return;
    }
    if (HeldLen_ == 0) {
        if (b != ESC) {
            if (!forwarded) {
                out.Forward.push_back(b);
            }
            return;
        }
        Held_[0] = b;
        HeldLen_ = 1;
        HeldForwarded_ = forwarded ? 1 : 0;
        HeldSince_ = Now_;
    } else {
        Held_[HeldLen_++] = b;
        if (forwarded) {
            HeldForwarded_ = HeldLen_;
        }
    }
    Resolve(out);
}

void CprDemux::Resolve(CprOutput& out) noexcept {
    const TryResult r = Scan(Held_.data(), HeldLen_);
    switch (r.Status) {
        case ETry::NeedMore:
            return;
        case ETry::Fail:
            ForwardHeld(1, out);
            Reprocess(1, out);
            return;
        case ETry::PasteStart:
            ForwardHeld(r.Length, out);
            Reprocess(r.Length, out);  // nothing follows a complete marker
            InPaste_ = true;
            PasteEndMatched_ = 0;
            return;
        case ETry::Da1: {
            const auto target = Target(false);
            if (!target) {
                ForwardHeld(r.Length, out);
                Reprocess(r.Length, out);
                return;
            }
            if (Queue_[*target].Kind != EKind::Own) {
                ForwardHeld(r.Length, out);
                PopThrough(*target, out);
                Reprocess(r.Length, out);
                return;
            }
            if (HeldForwarded_ > 0) {
                // The beginning already went to the agent: it is not ours.
                ForwardHeld(r.Length, out);
                Reprocess(r.Length, out);
                return;
            }
            // Our end marker without our position: unanswered.
            Strip(r.Length, out);
            const Entry own = PopThrough(*target, out);
            if (own.Live) {
                Fail(out);
            }
            Reprocess(r.Length, out);
            return;
        }
        case ETry::Position: {
            const auto target = Target(true);
            if (!target) {
                ForwardHeld(r.Length, out);
                Reprocess(r.Length, out);
                return;
            }
            if (Queue_[*target].Kind != EKind::Own) {
                ForwardHeld(r.Length, out);
                PopThrough(*target, out);
                Reprocess(r.Length, out);
                return;
            }
            if (HeldForwarded_ > 0) {
                ForwardHeld(r.Length, out);
                Reprocess(r.Length, out);
                return;
            }
            // Ours only if the end marker follows at once: Shift-, Ctrl- and
            // Alt-F3 are `CSI 1 ; Pm R`.
            std::size_t markerLen = 0;
            switch (LookDa1(Held_.data() + r.Length, HeldLen_ - r.Length,
                            markerLen)) {
                case EDa1Look::Partial:
                    return;
                case EDa1Look::No:
                    ForwardHeld(r.Length, out);
                    Reprocess(r.Length, out);
                    return;
                case EDa1Look::Full: {
                    Strip(r.Length + markerLen, out);
                    const Entry own = PopThrough(*target, out);
                    if (own.Live) {
                        out.Events.push_back(
                            CprEvent{ECprEvent::Answered, r.Row, r.Col});
                    }
                    Reprocess(r.Length + markerLen, out);
                    return;
                }
            }
            return;
        }
    }
}

void CprDemux::OnInput(ByteSpan bytes, Timestamp now, CprOutput& out) noexcept {
    Settle(now, out);
    Now_ = now;
    if (Off_) {
        out.Forward.insert(out.Forward.end(), bytes.begin(), bytes.end());
        return;
    }
    for (const std::byte b : bytes) {
        Step(b, false, out);
    }
    if (HeldLen_ > HeldForwarded_) {
        const bool hold =
            HeldForwarded_ == 0 && Pending() && now < HeldSince_ + CPR_HOLD;
        if (!hold) {
            FlushHeld(out);
        }
    }
}

// ---------------------------------------------------------------------------
// Time and events
// ---------------------------------------------------------------------------

void CprDemux::AbandonLive(CprOutput& out) noexcept {
    for (std::size_t i = 0; i < QueueLen_; ++i) {
        Queue_[i].Live = false;
    }
    FlushHeld(out);
}

void CprDemux::Settle(Timestamp now, CprOutput& out) noexcept {
    Now_ = now;
    if (Off_) {
        return;
    }
    if (HeldLen_ > HeldForwarded_ && now >= HeldSince_ + CPR_HOLD) {
        FlushHeld(out);
    }
    if (Pending() && now >= TimeoutAt_) {
        AbandonLive(out);
        out.Events.push_back(CprEvent{ECprEvent::TimedOut, 0, 0});
        Silent_ = true;
    }
    if (Outstanding() && now >= ForgetAt_) {
        FlushHeld(out);
        QueueLen_ = 0;
        HeldLen_ = 0;
        HeldForwarded_ = 0;
        InPaste_ = false;
        PasteEndMatched_ = 0;
        Off_ = true;
    }
}

void CprDemux::OnTick(Timestamp now, CprOutput& out) noexcept {
    Settle(now, out);
}

void CprDemux::OnCancel(Timestamp now, CprOutput& out) noexcept {
    Settle(now, out);
    AbandonLive(out);
}

void CprDemux::OnResize(Timestamp now, CprOutput& out) noexcept {
    OnCancel(now, out);
    Silent_ = false;
}

bool CprDemux::OnQuerySent(Timestamp now) noexcept {
    if (Pending()) {
        return false;
    }
    if (Off_) {
        Off_ = false;
        InPaste_ = false;
        PasteEndMatched_ = 0;
        HeldLen_ = 0;
        HeldForwarded_ = 0;
    }
    Push(EKind::Own);
    TimeoutAt_ = now + CPR_TIMEOUT;
    ForgetAt_ = now + CPR_TIMEOUT + CPR_FORGET;
    return true;
}

void CprDemux::OnOutput(ByteSpan bytes) noexcept {
    for (const std::byte b : bytes) {
        Parser_.FeedByte(b, *Grid_);
        const std::uint64_t dsr = Parser_.Dsr6QueryCount();
        const std::uint64_t da1 = Parser_.Da1QueryCount();
        if (dsr != SeenDsr6_) {
            SeenDsr6_ = dsr;
            if (!Off_) {
                Push(EKind::ProgramCpr);
            }
        }
        if (da1 != SeenDa1_) {
            SeenDa1_ = da1;
            if (!Off_) {
                Push(EKind::ProgramDa1);
            }
        }
    }
}

std::optional<Timestamp> CprDemux::NextDeadline() const noexcept {
    if (Off_) {
        return std::nullopt;
    }
    std::optional<Timestamp> next;
    const auto consider = [&next](Timestamp t) {
        if (!next || t < *next) {
            next = t;
        }
    };
    if (HeldLen_ > HeldForwarded_) {
        consider(HeldSince_ + CPR_HOLD);
    }
    if (Pending()) {
        consider(TimeoutAt_);
    }
    if (Outstanding()) {
        consider(ForgetAt_);
    }
    return next;
}

}  // namespace zet::predict
