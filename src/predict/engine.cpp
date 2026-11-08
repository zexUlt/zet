#include "zet/predict/engine.hpp"

#include <algorithm>
#include <limits>

#include "zet/grid/cell.hpp"
#include "zet/grid/unicode.hpp"

namespace zet::predict {

namespace {

constexpr char32_t NOT_A_GLYPH = std::numeric_limits<char32_t>::max();

constexpr bool IsBlank(char32_t cp) noexcept { return cp == 0 || cp == U' '; }

/// What a cell holds, for comparing with a prediction: 0 for an empty cell
/// (no codepoint or a space), the codepoint otherwise. The halves of a wide
/// glyph are never a match for anything we predict.
char32_t Held(const grid::Cell& c) noexcept {
    if (c.Width != 1) return NOT_A_GLYPH;
    return IsBlank(c.Codepoint) ? 0 : c.Codepoint;
}

constexpr char32_t Normalized(char32_t cp) noexcept {
    return IsBlank(cp) ? 0 : cp;
}

/// The glyphs a program prints for every key of a hidden line (pwfeedback,
/// getpass with stars). A typed key that matches one proves nothing about
/// echo: the program would print it for any key.
constexpr bool IsMaskGlyph(char32_t cp) noexcept {
    return cp == U'*' || cp == 0x2022 || cp == 0x25CF;
}

/// One cell's share of a row hash: 0 for an empty cell, so that the hash is
/// a sum over the cells that hold something and a cell's change is an
/// addition and a subtraction.
std::uint64_t CellTerm(std::uint16_t col, char32_t v) noexcept {
    if (v == 0) return 0;
    std::uint64_t x = (static_cast<std::uint64_t>(col) << 32) | v;
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

std::uint64_t RowHash(const grid::Grid& g, std::uint16_t row) noexcept {
    if (row >= g.Rows()) return 0;
    std::uint64_t h = 0;
    for (std::uint16_t c = 0; c < g.Cols(); ++c)
        h += CellTerm(c, Held(g.CellAt(row, c)));
    return h;
}

}  // namespace

PredictionEngine::PredictionEngine(const MirrorView& mirror, EDisplayMode mode,
                                   std::uint32_t streamGen) noexcept
    : Mode_(mode), Gen_(streamGen) {
    Rebase(mirror);
}

// ---------------------------------------------------------------------------
// Epochs
// ---------------------------------------------------------------------------

void PredictionEngine::BecomeTentative() noexcept {
    if (Epoch_ != std::numeric_limits<std::uint32_t>::max()) ++Epoch_;
}

void PredictionEngine::RemoveAt(std::size_t index) noexcept {
    if (index >= Count_) return;
    for (std::size_t i = index + 1; i < Count_; ++i) Preds_[i - 1] = Preds_[i];
    --Count_;
}

void PredictionEngine::KillEpoch(std::uint32_t epoch) noexcept {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < Count_; ++i) {
        if (Preds_[i].Epoch < epoch) Preds_[kept++] = Preds_[i];
    }
    Count_ = kept;
    if (Cursor_ && Cursor_->Epoch >= epoch) Cursor_.reset();
    BecomeTentative();
}

void PredictionEngine::ResetAll() noexcept {
    Count_ = 0;
    Cursor_.reset();
    InputStartCol_.reset();
    BecomeTentative();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void PredictionEngine::OnInput(ByteSpan bytes, std::uint64_t inputEnd,
                               Timestamp now,
                               const MirrorView& mirror) noexcept {
    if (Mode_ == EDisplayMode::Never) return;
    Cull(now);
    const std::uint64_t n = bytes.size();
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        // Offsets are contiguous: the last byte ends at inputEnd.
        const std::uint64_t end = inputEnd - (n - 1 - i);
        StepInput(bytes[i], end, now, mirror);
    }
}

void PredictionEngine::StepInput(std::byte b, std::uint64_t end, Timestamp now,
                                 const MirrorView& mirror) noexcept {
    const auto u = static_cast<std::uint8_t>(b);
    switch (In_) {
        case EInputState::Esc:
            if (u == '[') {
                In_ = EInputState::Csi;
                CsiLen_ = 0;
                CsiHasOther_ = false;
            } else if (u == 'O') {
                In_ = EInputState::Ss3;
            } else if (u == 0x1B) {
                BecomeTentative();
            } else {
                In_ = EInputState::Ground;  // ESC x: one key, consumed
            }
            return;
        case EInputState::Ss3:
            In_ = EInputState::Ground;
            return;
        case EInputState::Csi:
            if (u == 0x1B) {
                BecomeTentative();
                In_ = EInputState::Esc;
            } else {
                StepCsi(u);
                if (In_ == EInputState::Ground && (u < 0x20 || u >= 0x7F)) {
                    // A control byte ended the sequence early: it is a key of
                    // its own.
                    StepGround(u, end, now, mirror);
                }
            }
            return;
        case EInputState::Utf8:
            if ((u & 0xC0U) == 0x80U) {
                Utf8Cp_ = (Utf8Cp_ << 6) | (u & 0x3FU);
                if (--Utf8Left_ != 0) return;
                In_ = EInputState::Ground;
                if (InPaste_) return;
                const char32_t cp = Utf8Cp_;
                const char32_t minimum = Utf8Len_ == 2   ? 0x80
                                         : Utf8Len_ == 3 ? 0x800
                                                         : 0x10000;
                const bool valid = cp >= minimum && cp <= 0x10FFFF &&
                                   !(cp >= 0xD800 && cp <= 0xDFFF);
                if (valid && grid::Wcwidth(cp) == 1) {
                    PredictChar(cp, end, now, mirror);
                } else {
                    BecomeTentative();
                }
                return;
            }
            In_ = EInputState::Ground;
            if (!InPaste_) BecomeTentative();
            StepGround(u, end, now, mirror);
            return;
        case EInputState::Ground:
            StepGround(u, end, now, mirror);
            return;
    }
}

void PredictionEngine::StepGround(std::uint8_t b, std::uint64_t end,
                                  Timestamp now,
                                  const MirrorView& mirror) noexcept {
    if (b == 0x1B) {
        BecomeTentative();
        In_ = EInputState::Esc;
        return;
    }
    // Between ESC[200~ and ESC[201~ nothing is predicted.
    if (InPaste_) return;

    if (b == 0x08 || b == 0x7F) {
        PredictBackspace(end, now, mirror);
    } else if (b < 0x20) {
        BecomeTentative();
        if (b == 0x0D || b == 0x0A) InputStartCol_.reset();
    } else if (b < 0x7F) {
        PredictChar(b, end, now, mirror);
    } else if (b >= 0xC2 && b <= 0xF4) {
        In_ = EInputState::Utf8;
        Utf8Len_ = b < 0xE0 ? 2 : (b < 0xF0 ? 3 : 4);
        Utf8Left_ = static_cast<std::uint8_t>(Utf8Len_ - 1);
        Utf8Cp_ = b & (b < 0xE0 ? 0x1FU : (b < 0xF0 ? 0x0FU : 0x07U));
    } else {
        BecomeTentative();  // stray continuation byte or invalid lead
    }
}

void PredictionEngine::StepCsi(std::uint8_t b) noexcept {
    if (b >= 0x40 && b <= 0x7E) {
        In_ = EInputState::Ground;
        if (b == '~' && !CsiHasOther_ && CsiLen_ == 3) {
            if (CsiParam_[0] == '2' && CsiParam_[1] == '0') {
                if (CsiParam_[2] == '0') InPaste_ = true;
                if (CsiParam_[2] == '1') InPaste_ = false;
            }
        }
    } else if (b >= 0x30 && b <= 0x39) {
        if (CsiLen_ < CsiParam_.size())
            CsiParam_[CsiLen_] = static_cast<char>(b);
        if (CsiLen_ < std::numeric_limits<std::uint8_t>::max()) ++CsiLen_;
    } else if (b >= 0x20 && b <= 0x3F) {
        CsiHasOther_ = true;  // ';', ':', private marker or intermediate
    } else {
        In_ = EInputState::Ground;  // C0 or high byte: the sequence is over
    }
}

bool PredictionEngine::Predictable(const MirrorView& mirror) const noexcept {
    const grid::Grid& g = mirror.Screen;
    return Trusted_ && !g.IsAltScreen() && !g.ModeSet(grid::EMode::Irm) &&
           g.CharsetG0() == 0 && g.CharsetActive() == 0 && !g.PendingWrap() &&
           g.CursorRow() < g.Rows() && g.CursorCol() < g.Cols();
}

PredictionEngine::Position PredictionEngine::BaseCursor(
    const MirrorView& mirror) const noexcept {
    if (Cursor_) return {Cursor_->Row, Cursor_->Col};
    return {mirror.Screen.CursorRow(), mirror.Screen.CursorCol()};
}

bool PredictionEngine::TailEmpty(const MirrorView& mirror, std::uint16_t row,
                                 std::uint16_t col) const noexcept {
    const grid::Grid& g = mirror.Screen;
    if (row >= g.Rows()) return false;
    for (std::uint16_t c = col; c < g.Cols(); ++c) {
        if (Held(g.CellAt(row, c)) == 0) continue;
        // A cell the mirror still shows may already be predicted away.
        bool erased = false;
        for (std::size_t i = 0; i < Count_; ++i) {
            if (Preds_[i].Row == row && Preds_[i].Col == c &&
                Normalized(Preds_[i].Codepoint) == 0) {
                erased = true;
                break;
            }
        }
        if (!erased) return false;
    }
    return true;
}

bool PredictionEngine::HasRoomFor(std::uint16_t row,
                                  std::uint16_t col) const noexcept {
    if (Count_ < PREDICTIONS_MAX) return true;
    for (std::size_t i = 0; i < Count_; ++i) {
        if (Preds_[i].Row == row && Preds_[i].Col == col) return true;
    }
    return false;
}

bool PredictionEngine::Upsert(const Prediction& p) noexcept {
    // One prediction per cell: the later keystroke replaces the earlier, so
    // that type, backspace, type again is checked against the final cell only.
    for (std::size_t i = 0; i < Count_; ++i) {
        if (Preds_[i].Row == p.Row && Preds_[i].Col == p.Col) {
            RemoveAt(i);
            break;
        }
    }
    if (Count_ >= PREDICTIONS_MAX) return false;
    Preds_[Count_++] = p;
    return true;
}

void PredictionEngine::PredictChar(char32_t cp, std::uint64_t end,
                                   Timestamp now,
                                   const MirrorView& mirror) noexcept {
    if (!Predictable(mirror)) {
        BecomeTentative();
        return;
    }
    const Position at = BaseCursor(mirror);
    const grid::Grid& g = mirror.Screen;
    if (at.Row >= g.Rows() || at.Col + 1 >= g.Cols() ||
        !TailEmpty(mirror, at.Row, at.Col) || !HasRoomFor(at.Row, at.Col)) {
        // Last column, text to the right, or the list is full.
        BecomeTentative();
        return;
    }
    Prediction p;
    p.Row = at.Row;
    p.Col = at.Col;
    p.Codepoint = cp;
    p.Prior = Held(g.CellAt(at.Row, at.Col));
    p.InputEnd = end;
    p.Epoch = Epoch_;
    p.Created = now;
    if (!Upsert(p)) {
        BecomeTentative();
        return;
    }
    if (!InputStartCol_) InputStartCol_ = at.Col;
    Cursor_ = CursorPrediction{at.Row, static_cast<std::uint16_t>(at.Col + 1),
                               end, Epoch_};
}

void PredictionEngine::PredictBackspace(std::uint64_t end, Timestamp now,
                                        const MirrorView& mirror) noexcept {
    if (!Predictable(mirror)) {
        BecomeTentative();
        return;
    }
    const Position at = BaseCursor(mirror);
    const grid::Grid& g = mirror.Screen;
    // At or before the start of input the cell to the left is not ours.
    if (!InputStartCol_ || at.Col == 0 || at.Col <= *InputStartCol_ ||
        at.Row >= g.Rows() || at.Col >= g.Cols()) {
        BecomeTentative();
        return;
    }
    const auto left = static_cast<std::uint16_t>(at.Col - 1);
    // Only at the end of the line: in the middle the program shifts the tail.
    if (!TailEmpty(mirror, at.Row, at.Col) ||
        g.CellAt(at.Row, left).Width != 1 || !HasRoomFor(at.Row, left)) {
        BecomeTentative();
        return;
    }
    Prediction p;
    p.Row = at.Row;
    p.Col = left;
    p.Codepoint = 0;
    p.Prior = Held(g.CellAt(at.Row, left));
    p.InputEnd = end;
    p.Epoch = Epoch_;
    p.Created = now;
    if (!Upsert(p)) {
        BecomeTentative();
        return;
    }
    Cursor_ = CursorPrediction{at.Row, left, end, Epoch_};
}

// ---------------------------------------------------------------------------
// The mirror
// ---------------------------------------------------------------------------

void PredictionEngine::Rebase(const MirrorView& mirror) noexcept {
    const grid::Grid& g = mirror.Screen;
    LastCols_ = g.Cols();
    LastRows_ = g.Rows();
    LastRow_ = g.CursorRow();
    LastCol_ = g.CursorCol();
    LastRowHash_ = RowHash(g, g.CursorRow());
    LastAlt_ = g.IsAltScreen();
    LastScroll_ = g.ScrollCount();
    LastColumnSet_ = g.ColumnSetCount();
    LastUnknown_ = mirror.Parser.UnknownSeqCount();
    LastDcsApc_ = mirror.Parser.DcsApcCount();
}

bool PredictionEngine::OutputIsPredictedEcho(
    const MirrorView& mirror) noexcept {
    const grid::Grid& g = mirror.Screen;
    const std::uint16_t row = g.CursorRow();
    const std::uint16_t col = g.CursorCol();
    // The row hash as it would be if exactly the predictions the mirror now
    // shows had been echoed; a prediction not echoed (yet) changes nothing.
    std::uint64_t expected = LastRowHash_;
    bool cursorOk = col == LastCol_;
    for (std::size_t i = 0; i < Count_; ++i) {
        Prediction& p = Preds_[i];
        if (p.Row != row || p.Col >= g.Cols()) continue;
        const char32_t want = Normalized(p.Codepoint);
        if (Held(g.CellAt(p.Row, p.Col)) != want) continue;
        // An echo seen at an earlier look is already in the last hash.
        if (!p.Echoed) {
            expected +=
                CellTerm(p.Col, want) - CellTerm(p.Col, Normalized(p.Prior));
            p.Echoed = true;
        }
        if (p.Codepoint != 0 ? col == p.Col + 1 : col == p.Col) cursorOk = true;
    }
    return cursorOk && expected == RowHash(g, row);
}

void PredictionEngine::Observe(const MirrorView& mirror) noexcept {
    const grid::Grid& g = mirror.Screen;
    bool reset = false;
    bool untrust = false;

    if (g.Cols() != LastCols_ || g.Rows() != LastRows_) {
        reset = true;
        untrust = true;
    }
    if (g.IsAltScreen() != LastAlt_) reset = true;
    if (g.CursorRow() != LastRow_) reset = true;
    if (g.ScrollCount() != LastScroll_) reset = true;
    if (mirror.Parser.UnknownSeqCount() != LastUnknown_) {
        reset = true;
        untrust = true;
    }
    if (mirror.Parser.DcsApcCount() != LastDcsApc_) {
        reset = true;
        untrust = true;
    }
    const bool gained = g.ColumnSetCount() != LastColumnSet_;
    const bool unsolicited = !reset && !OutputIsPredictedEcho(mirror);

    Rebase(mirror);
    if (reset) {
        ResetAll();
    } else if (unsolicited && (Count_ > 0 || Epoch_ <= Confirmed_)) {
        // An epoch nobody confirmed and holding nothing cannot become
        // visible by itself, so raising it would only renumber it.
        BecomeTentative();
    }
    // Losing the column wins over regaining it in the same stretch of output:
    // the order inside it is not known, and a wrong column is what costs.
    if (untrust) {
        Trusted_ = false;
    } else if (gained) {
        Trusted_ = true;
    }
}

void PredictionEngine::OnOutput(std::uint64_t outputEnd, Timestamp now,
                                const MirrorView& mirror) noexcept {
    if (Mode_ == EDisplayMode::Never) return;
    Cull(now);
    OutOffset_ = std::max(OutOffset_, outputEnd);
    Observe(mirror);
    Drain(now, mirror);
}

void PredictionEngine::OnSnapshot(std::uint32_t streamGen, Timestamp now,
                                  const MirrorView& mirror) noexcept {
    if (Mode_ == EDisplayMode::Never) return;
    Cull(now);
    Gen_ = streamGen;
    OutOffset_ = 0;
    Rebase(mirror);
    ResetAll();
    Trusted_ = true;
    Drain(now, mirror);
}

void PredictionEngine::OnResize(Timestamp now,
                                const MirrorView& mirror) noexcept {
    if (Mode_ == EDisplayMode::Never) return;
    Cull(now);
    Rebase(mirror);
    ResetAll();
    Trusted_ = false;
}

// ---------------------------------------------------------------------------
// Echo-acks
// ---------------------------------------------------------------------------

void PredictionEngine::OnEchoAck(const EchoAck& ack, Timestamp now,
                                 const MirrorView& mirror) noexcept {
    if (Mode_ == EDisplayMode::Never) return;
    Cull(now);
    if (AckCount_ == Acks_.size()) {
        // Drop the oldest: the next one covers at least as much input.
        for (std::size_t i = 1; i < AckCount_; ++i) Acks_[i - 1] = Acks_[i];
        --AckCount_;
    }
    Acks_[AckCount_++] = ack;
    Drain(now, mirror);
}

void PredictionEngine::Drain(Timestamp now, const MirrorView& mirror) noexcept {
    while (AckCount_ > 0) {
        const EchoAck a = Acks_[0];
        if (a.StreamGen > Gen_ ||
            (a.StreamGen == Gen_ && a.OutputOffset > OutOffset_)) {
            break;  // the output it speaks of has not been fed to the mirror
        }
        for (std::size_t i = 1; i < AckCount_; ++i) Acks_[i - 1] = Acks_[i];
        --AckCount_;
        if (a.StreamGen == Gen_) Reconcile(a.InputOffset, now, mirror);
    }
}

void PredictionEngine::Reconcile(std::uint64_t inputOffset, Timestamp now,
                                 const MirrorView& mirror) noexcept {
    const grid::Grid& g = mirror.Screen;
    for (;;) {
        std::size_t i = 0;
        while (i < Count_ && Preds_[i].InputEnd > inputOffset) ++i;
        if (i == Count_) break;

        const Prediction p = Preds_[i];
        if (p.Row >= g.Rows() || p.Col >= g.Cols()) {
            RemoveAt(i);
            continue;
        }
        if (Held(g.CellAt(p.Row, p.Col)) == Normalized(p.Codepoint)) {
            // Right. Only a glyph that was not already there proves the echo
            // works: empty cells match too easily (mosh, CorrectNoCredit), and
            // a mask glyph is what a hidden line prints for any key.
            if (Normalized(p.Codepoint) != 0 &&
                Normalized(p.Prior) != Normalized(p.Codepoint) &&
                !IsMaskGlyph(p.Codepoint)) {
                Confirmed_ = std::max(Confirmed_, p.Epoch);
                if (Glitch_ > 0 && now - p.Created < GLITCH_THRESHOLD &&
                    (!LastQuick_ ||
                     now - *LastQuick_ >= GLITCH_REPAIR_MININTERVAL)) {
                    --Glitch_;
                    LastQuick_ = now;
                }
            }
            RemoveAt(i);
        } else if (p.Epoch > Confirmed_) {
            // Wrong in an epoch nobody saw: it and everything above go.
            KillEpoch(p.Epoch);
        } else {
            // Wrong where the user could see it: everything goes, and it
            // counts as a glitch.
            ResetAll();
            Glitch_ = std::max(Glitch_, GLITCH_REPAIR_COUNT);
        }
    }
    ReconcileCursor(inputOffset, g);
}

void PredictionEngine::ReconcileCursor(std::uint64_t inputOffset,
                                       const grid::Grid& g) noexcept {
    if (!Cursor_ || Cursor_->InputEnd > inputOffset) return;
    const CursorPrediction c = *Cursor_;
    if (c.Row == g.CursorRow() && c.Col == g.CursorCol()) {
        Cursor_.reset();
    } else if (c.Epoch > Confirmed_) {
        KillEpoch(c.Epoch);
    } else {
        ResetAll();
        Glitch_ = std::max(Glitch_, GLITCH_REPAIR_COUNT);
    }
}

// ---------------------------------------------------------------------------
// Time and display
// ---------------------------------------------------------------------------

void PredictionEngine::OnSrtt(Duration srtt, Timestamp now) noexcept {
    if (Mode_ == EDisplayMode::Never) return;
    Srtt_ = srtt;
    Cull(now);
}

void PredictionEngine::OnTick(Timestamp now) noexcept {
    if (Mode_ == EDisplayMode::Never) return;
    Cull(now);
}

bool PredictionEngine::AnyDrawn() const noexcept {
    for (std::size_t i = 0; i < Count_; ++i) {
        if (Preds_[i].Drawn) return true;
    }
    return false;
}

void PredictionEngine::Cull(Timestamp now) noexcept {
    if (Srtt_) {
        // mosh compares its send interval, SRTT / 2, with the thresholds; as
        // they are whole milliseconds, "SRTT > 2 * threshold" says the same.
        const Duration srtt = *Srtt_;
        if (srtt > 2 * PREDICT_SHOW_HIGH) {
            SrttTrigger_ = true;
        } else if (SrttTrigger_ && srtt <= 2 * PREDICT_SHOW_LOW &&
                   !AnyDrawn()) {
            SrttTrigger_ = false;
        }
        if (srtt > 2 * PREDICT_FLAG_HIGH) {
            Flagging_ = true;
        } else if (srtt <= 2 * PREDICT_FLAG_LOW) {
            Flagging_ = false;
        }
    }
    // A prediction nobody confirms for long is a glitch of its own, whatever
    // the SRTT says. The list is in order of creation: the first is oldest.
    if (Count_ > 0) {
        const Duration age = now - Preds_[0].Created;
        if (age >= GLITCH_FLAG_THRESHOLD) {
            Glitch_ = 2 * GLITCH_REPAIR_COUNT;
        } else if (age >= GLITCH_THRESHOLD && Glitch_ < GLITCH_REPAIR_COUNT) {
            Glitch_ = GLITCH_REPAIR_COUNT;
        }
    }
}

bool PredictionEngine::ShowPredictions() const noexcept {
    switch (Mode_) {
        case EDisplayMode::Always:
            return true;
        case EDisplayMode::Never:
            return false;
        case EDisplayMode::Adaptive:
            return SrttTrigger_ || Glitch_ > 0;
    }
    return false;
}

bool PredictionEngine::UnderlinePredictions() const noexcept {
    return Mode_ != EDisplayMode::Never &&
           (Flagging_ || Glitch_ > GLITCH_REPAIR_COUNT);
}

void PredictionEngine::SetDrawn(std::size_t index, bool drawn) noexcept {
    if (index < Count_) Preds_[index].Drawn = drawn;
}

void PredictionEngine::ClearDrawn() noexcept {
    for (std::size_t i = 0; i < Count_; ++i) Preds_[i].Drawn = false;
}

}  // namespace zet::predict
