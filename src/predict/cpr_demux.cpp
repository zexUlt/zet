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
        new (std::nothrow) CprDemux(std::unique_ptr<grid::Grid>(*grid)));
}

std::size_t CprDemux::HeldSize() const noexcept {
    return HeldLen_ - HeldForwarded_;
}

// ---------------------------------------------------------------------------
// The own entry and the program's counts
// ---------------------------------------------------------------------------

namespace {

void Bump(std::uint32_t& count) noexcept {
    if (count != UINT32_MAX) {
        ++count;
    }
}

}  // namespace

void CprDemux::Fail(CprSink& out) noexcept {
    out.Event(CprEvent{ECprEvent::Failed, 0, 0});
    Silent_ = true;
}

void CprDemux::Consume(const TryResult* position, CprSink& out) noexcept {
    const bool live = Own_ == EOwn::Live;
    Own_ = EOwn::None;
    // The program's queries written after ours are the first in line now.
    Ahead_ = Behind_;
    AheadAt_ = BehindAt_;
    Behind_ = 0;
    if (!live) {
        return;
    }
    if (position != nullptr) {
        out.Event(CprEvent{ECprEvent::Answered, position->Row, position->Col});
    } else {
        // Our end marker without our position: unanswered.
        Fail(out);
    }
}

void CprDemux::ExpireCounts(Timestamp now) noexcept {
    // A reply lost on the way must not block the alignment for good.
    if (Ahead_ > 0 && now >= AheadAt_ + CPR_TIMEOUT + CPR_FORGET) {
        Ahead_ = 0;
    }
    if (Behind_ > 0 && now >= BehindAt_ + CPR_TIMEOUT + CPR_FORGET) {
        Behind_ = 0;
    }
}

void CprDemux::StripHeld(std::size_t length, CprSink& out) const noexcept {
    out.Strip({Held_.data(), length});
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

void CprDemux::ForwardHeld(std::size_t count, CprSink& out) noexcept {
    if (HeldForwarded_ < count) {
        out.Forward({Held_.data() + HeldForwarded_, count - HeldForwarded_});
        HeldForwarded_ = count;
    }
}

void CprDemux::FlushHeld(CprSink& out) noexcept { ForwardHeld(HeldLen_, out); }

void CprDemux::Reprocess(std::size_t from, CprSink& out) noexcept {
    std::array<std::byte, CPR_HELD_MAX> rest{};
    std::array<Timestamp, CPR_HELD_MAX> restAt{};
    const std::size_t n = HeldLen_ - from;
    std::copy(Held_.begin() + from, Held_.begin() + HeldLen_, rest.begin());
    std::copy(HeldAt_.begin() + from, HeldAt_.begin() + HeldLen_,
              restAt.begin());
    const std::size_t forwarded =
        HeldForwarded_ > from ? HeldForwarded_ - from : 0;
    HeldLen_ = 0;
    HeldForwarded_ = 0;
    for (std::size_t i = 0; i < n; ++i) {
        Step(rest[i], i < forwarded, restAt[i], out);
    }
}

void CprDemux::Step(std::byte b, bool forwarded, Timestamp at,
                    CprSink& out) noexcept {
    if (InPaste_) {
        if (!forwarded) {
            out.Forward({&b, 1});
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
    if (HeldLen_ == 0 && b != ESC) {
        if (!forwarded) {
            out.Forward({&b, 1});
        }
        return;
    }
    Held_[HeldLen_] = b;
    HeldAt_[HeldLen_] = at;
    ++HeldLen_;
    if (forwarded) {
        HeldForwarded_ = HeldLen_;
    }
    Resolve(out);
}

void CprDemux::Resolve(CprSink& out) noexcept {
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
            if (Ahead_ > 0) {
                // The program's: it asked before us.
                ForwardHeld(r.Length, out);
                --Ahead_;
            } else if (Own_ == EOwn::None) {
                ForwardHeld(r.Length, out);
            } else {
                // Ours. Without a position in front of it (that one was
                // forwarded, or never came) the query is unanswered. When the
                // beginning of the reply already went to the agent (the hold
                // ran out), the rest follows it, but the entry is still
                // answered.
                if (HeldForwarded_ > 0) {
                    ForwardHeld(r.Length, out);
                } else {
                    StripHeld(r.Length, out);
                }
                Consume(nullptr, out);
            }
            Reprocess(r.Length, out);
            return;
        }
        case ETry::Position: {
            if (!Eligible() || HeldForwarded_ > 0) {
                // No DA1 reply of ours can follow, or the beginning is gone:
                // a key, or the program's answer. A DA1 reply right after it
                // is judged on its own.
                ForwardHeld(r.Length, out);
                Reprocess(r.Length, out);
                return;
            }
            // Ours only if our DA1 reply follows at once: Shift-, Ctrl- and
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
                case EDa1Look::Full:
                    StripHeld(r.Length + markerLen, out);
                    Consume(&r, out);
                    Reprocess(r.Length + markerLen, out);
                    return;
            }
            return;
        }
    }
}

void CprDemux::OnInput(ByteSpan bytes, Timestamp now, CprSink& out) noexcept {
    Settle(now, out);
    std::size_t i = 0;
    while (i < bytes.size()) {
        if (HeldLen_ == 0 && !InPaste_) {
            // Nothing is under way: a run without ESC goes in one piece.
            std::size_t j = i;
            while (j < bytes.size() && bytes[j] != ESC) {
                ++j;
            }
            if (j > i) {
                out.Forward(bytes.subspan(i, j - i));
                i = j;
                continue;
            }
        }
        Step(bytes[i++], false, now, out);
    }
    if (HeldLen_ > HeldForwarded_) {
        const bool hold =
            HeldForwarded_ == 0 && Eligible() && now < HeldAt_[0] + CPR_HOLD;
        if (!hold) {
            FlushHeld(out);
        }
    }
}

// ---------------------------------------------------------------------------
// Time and events
// ---------------------------------------------------------------------------

void CprDemux::AbandonLive() noexcept {
    if (Own_ == EOwn::Live) {
        Own_ = EOwn::Dead;
    }
}

void CprDemux::Settle(Timestamp now, CprSink& out) noexcept {
    Now_ = now;
    ExpireCounts(now);
    if (HeldLen_ > HeldForwarded_ &&
        (now >= HeldAt_[HeldForwarded_] + CPR_HOLD || !Eligible())) {
        FlushHeld(out);
    }
    if (Own_ == EOwn::Live && now >= TimeoutAt_) {
        Own_ = EOwn::Dead;
        out.Event(CprEvent{ECprEvent::TimedOut, 0, 0});
        Silent_ = true;
    }
    if (Own_ != EOwn::None && now >= ForgetAt_) {
        FlushHeld(out);
        Own_ = EOwn::None;
        Off_ = true;
        // Queries written after the forgotten one are before any new one.
        if (Behind_ > 0) {
            Ahead_ =
                Ahead_ > UINT32_MAX - Behind_ ? UINT32_MAX : Ahead_ + Behind_;
            AheadAt_ = BehindAt_;
            Behind_ = 0;
        }
    }
}

void CprDemux::OnTick(Timestamp now, CprSink& out) noexcept {
    Settle(now, out);
}

void CprDemux::OnCancel(Timestamp now, CprSink& out) noexcept {
    Settle(now, out);
    AbandonLive();
}

void CprDemux::OnResize(Timestamp now, CprSink& out) noexcept {
    OnCancel(now, out);
    Silent_ = false;
}

bool CprDemux::OnQuerySent(Timestamp now) noexcept {
    ExpireCounts(now);
    if (Own_ != EOwn::None) {
        return false;
    }
    Off_ = false;
    Own_ = EOwn::Live;
    TimeoutAt_ = now + CPR_TIMEOUT;
    ForgetAt_ = now + CPR_TIMEOUT + CPR_FORGET;
    return true;
}

void CprDemux::OnOutput(ByteSpan bytes, Timestamp now) noexcept {
    ExpireCounts(now);
    for (const std::byte b : bytes) {
        Parser_.FeedByte(b, *Grid_);
        const std::uint64_t da1 = Parser_.Da1QueryCount();
        if (da1 != SeenDa1_) {
            SeenDa1_ = da1;
            if (Own_ == EOwn::None) {
                Bump(Ahead_);
                AheadAt_ = now;
            } else {
                Bump(Behind_);
                BehindAt_ = now;
            }
        }
    }
}

std::optional<Timestamp> CprDemux::NextDeadline() const noexcept {
    std::optional<Timestamp> next;
    const auto consider = [&next](Timestamp t) {
        if (!next || t < *next) {
            next = t;
        }
    };
    if (HeldLen_ > HeldForwarded_) {
        consider(HeldAt_[HeldForwarded_] + CPR_HOLD);
    }
    if (Own_ == EOwn::Live) {
        consider(TimeoutAt_);
    }
    if (Own_ != EOwn::None) {
        consider(ForgetAt_);
    }
    return next;
}

}  // namespace zet::predict
