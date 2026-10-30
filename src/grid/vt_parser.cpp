#include "zet/grid/vt_parser.hpp"
#include "zet/grid/grid.hpp"
#include "zet/grid/unicode.hpp"

#include <cstring>

namespace zet::grid {

// ---------------------------------------------------------------------------
// Constructor / reset
// ---------------------------------------------------------------------------

VtParser::VtParser() noexcept { Reset(); }

void VtParser::Reset() noexcept {
    State_ = EParserState::Ground;
    Utf8Accumulator_ = 0;
    Utf8Remaining_ = 0;
    ParamLen_ = 0;
    IntermediateCount_ = 0;
    PrivateMode_ = false;
    InOsc_ = false;
    BypassBytes_ = 0;
    // Leave UnknownSeqCount_ intact (monotonic counter).
}

// ---------------------------------------------------------------------------
// UTF-8 decoding
// ---------------------------------------------------------------------------

/// Return true if byte is a UTF-8 continuation byte (10xxxxxx).
static inline bool IsUtf8Continuation(std::uint8_t b) noexcept {
    return (b & 0xC0) == 0x80;
}

/// Start a UTF-8 sequence from a leading byte.  Returns the number of
/// additional bytes needed (1, 2, or 3) or 0 on invalid byte.
static inline std::uint8_t Utf8LeadLength(std::uint8_t b) noexcept {
    if ((b & 0xE0) == 0xC0) return 1;  // 110xxxxx → 2 bytes total
    if ((b & 0xF0) == 0xE0) return 2;  // 1110xxxx → 3 bytes total
    if ((b & 0xF8) == 0xF0) return 3;  // 11110xxx → 4 bytes total
    return 0;  // invalid or single-byte (handled by caller)
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

void VtParser::Feed(const std::byte* data, std::size_t length,
                    Grid& grid) noexcept {
    for (std::size_t i = 0; i < length; ++i) {
        FeedByte(data[i], grid);
    }
}

void VtParser::FeedByte(std::byte b, Grid& grid) noexcept {
    const std::uint8_t raw = static_cast<std::uint8_t>(b);

    // -----------------------------------------------------------------------
    // Bypass state: DCS/OSC/APC/PM/SOS payload forwarding.
    // -----------------------------------------------------------------------
    if (State_ == EParserState::kBypass) {
        // CAN (0x18) / SUB (0x1A) abort the sequence.
        if (raw == 0x18 || raw == 0x1A) {
            BypassBytes_ = 0;
            InOsc_ = false;
            State_ = EParserState::Ground;
            return;
        }
        // ESC starts potential ST (ESC \).
        if (raw == 0x1B) {
            // Remember ESC; next byte decides.
            BypassBytes_ = 0;
            InOsc_ = false;
            State_ = EParserState::Escape;
            return;
        }
        // BEL terminates OSC only.
        if (raw == 0x07 && InOsc_) {
            BypassBytes_ = 0;
            InOsc_ = false;
            State_ = EParserState::Ground;
            return;
        }
        // Enforce bypass limit (design.md §9).
        ++BypassBytes_;
        if (BypassBytes_ >= BYPASS_LIMIT_BYTES) {
            BypassBytes_ = 0;
            InOsc_ = false;
            State_ = EParserState::Ground;
            return;
        }
        EmitBypass(b);
        return;
    }

    // -----------------------------------------------------------------------
    // Handle C0 controls that can interrupt any state.
    // -----------------------------------------------------------------------
    // CAN (0x18) / SUB (0x1A): abort current sequence, return to ground.
    if (raw == 0x18 || raw == 0x1A) {
        Utf8Remaining_ = 0;
        TransitionTo(EParserState::Ground, grid);
        // SUB traditionally prints a substitution character (U+FFFD).
        if (raw == 0x1A) grid.PutChar(0xFFFD, 1);
        return;
    }

    // ESC: start/restart escape processing (flushes pending UTF-8).
    if (raw == 0x1B) {
        Utf8Remaining_ = 0;
        ClearParams();
        IntermediateCount_ = 0;
        PrivateMode_ = false;
        State_ = EParserState::Escape;
        return;
    }

    // -----------------------------------------------------------------------
    // UTF-8 accumulation.
    // -----------------------------------------------------------------------
    if (Utf8Remaining_ > 0) {
        if (IsUtf8Continuation(raw)) {
            Utf8Accumulator_ = (Utf8Accumulator_ << 6) | (raw & 0x3F);
            --Utf8Remaining_;
            if (Utf8Remaining_ == 0) {
                char32_t cp = static_cast<char32_t>(Utf8Accumulator_);
                // Validate: reject overlong / surrogates / > U+10FFFF.
                if (cp < 0x80 || cp > 0x10FFFF ||
                    (cp >= 0xD800 && cp <= 0xDFFF)) {
                    cp = 0xFFFD;
                }
                if (State_ == EParserState::Ground) {
                    const int w = Wcwidth(cp);
                    grid.PutChar(cp, w > 0 ? w : 1);
                }
                // In other states, printable high codepoints are ignored per
                // Paul Williams' spec (we are not in Ground).
            }
            return;
        } else {
            // Invalid continuation: emit replacement and re-process this byte.
            if (State_ == EParserState::Ground) grid.PutChar(0xFFFD, 1);
            Utf8Remaining_ = 0;
        }
    }

    // Start a UTF-8 multi-byte sequence (only in Ground state).
    if (raw >= 0xC0 && raw <= 0xFF && State_ == EParserState::Ground) {
        const std::uint8_t extra = Utf8LeadLength(raw);
        if (extra > 0) {
            // Strip lead bits: 2-byte=0x1F, 3-byte=0x0F, 4-byte=0x07.
            static constexpr std::uint8_t kMask[] = {0, 0x1F, 0x0F, 0x07};
            Utf8Accumulator_ = raw & kMask[extra];
            Utf8Remaining_ = extra;
            return;
        }
        // 0xC0/0xC1 are overlong; 0xFE/0xFF are invalid: emit replacement.
        grid.PutChar(0xFFFD, 1);
        return;
    }

    // -----------------------------------------------------------------------
    // C0 controls in non-Ground states (already handled CAN/SUB/ESC above).
    // -----------------------------------------------------------------------
    if (raw < 0x20) {
        HandleControl(raw, grid);
        // Most C0 controls do NOT change parser state.
        return;
    }

    // DEL (0x7F) is ignored in most states.
    if (raw == 0x7F) return;

    // -----------------------------------------------------------------------
    // State machine transitions.
    // -----------------------------------------------------------------------
    switch (State_) {
        // -------------------------------------------------------------------
        case EParserState::Ground:
            // Printable ASCII (0x20–0x7E).
            if (raw >= 0x20 && raw <= 0x7E) {
                grid.PutChar(static_cast<char32_t>(raw), 1);
            }
            break;

        // -------------------------------------------------------------------
        case EParserState::Escape:
            if (raw >= 0x20 && raw <= 0x2F) {
                // Intermediate byte.
                CollectIntermediate(raw);
                State_ = EParserState::EscapeIntermediate;
            } else if (raw >= 0x30 && raw <= 0x4F) {
                // Final byte (no intermediates).
                DispatchEsc(static_cast<char32_t>(raw), grid);
                TransitionTo(EParserState::Ground, grid);
            } else if (raw == 0x50) {  // 'P' → DCS
                TransitionTo(EParserState::DcsEntry, grid);
            } else if (raw == 0x5B) {  // '[' → CSI
                ClearParams();
                IntermediateCount_ = 0;
                PrivateMode_ = false;
                State_ = EParserState::CsiEntry;
            } else if (raw == 0x5D) {  // ']' → OSC
                InOsc_ = true;
                BypassBytes_ = 0;
                State_ = EParserState::kBypass;
            } else if (raw == 0x58 || raw == 0x5E || raw == 0x5F) {
                // SOS (0x58), PM (0x5E), APC (0x5F) → bypass
                InOsc_ = false;
                BypassBytes_ = 0;
                State_ = EParserState::kBypass;
            } else if (raw >= 0x51 && raw <= 0x57) {
                // 0x51–0x57 (Q–W): single-char ESC dispatch.
                DispatchEsc(static_cast<char32_t>(raw), grid);
                TransitionTo(EParserState::Ground, grid);
            } else if (raw >= 0x59 && raw <= 0x7E) {
                DispatchEsc(static_cast<char32_t>(raw), grid);
                TransitionTo(EParserState::Ground, grid);
            } else if (raw == 0x7F) {
                // Ignore.
            }
            break;

        // -------------------------------------------------------------------
        case EParserState::EscapeIntermediate:
            if (raw >= 0x20 && raw <= 0x2F) {
                CollectIntermediate(raw);
            } else if (raw >= 0x30 && raw <= 0x7E) {
                DispatchEsc(static_cast<char32_t>(raw), grid);
                TransitionTo(EParserState::Ground, grid);
            }
            break;

        // -------------------------------------------------------------------
        case EParserState::CsiEntry:
            if (raw >= 0x30 && raw <= 0x39) {
                // Numeric parameter.
                AddParam(raw);
                State_ = EParserState::CsiParam;
            } else if (raw == 0x3A || raw == 0x3B) {
                // Sub-param separator (':') or param separator (';').
                AddParam(raw);
                State_ = EParserState::CsiParam;
            } else if (raw >= 0x3C && raw <= 0x3F) {
                // Private intro byte ('?', '>', '<', '=').
                PrivateMode_ = true;
                AddParam(raw);
                State_ = EParserState::CsiParam;
            } else if (raw >= 0x20 && raw <= 0x2F) {
                CollectIntermediate(raw);
                State_ = EParserState::CsiIntermediate;
            } else if (raw >= 0x40 && raw <= 0x7E) {
                DispatchCsi(static_cast<char32_t>(raw), grid);
                TransitionTo(EParserState::Ground, grid);
            }
            break;

        // -------------------------------------------------------------------
        case EParserState::CsiParam:
            if (raw >= 0x30 && raw <= 0x3F) {
                AddParam(raw);
            } else if (raw >= 0x20 && raw <= 0x2F) {
                CollectIntermediate(raw);
                State_ = EParserState::CsiIntermediate;
            } else if (raw >= 0x40 && raw <= 0x7E) {
                DispatchCsi(static_cast<char32_t>(raw), grid);
                TransitionTo(EParserState::Ground, grid);
            } else {
                // 0x00–0x1F: execute (handled before state machine).
                // 0x7F: ignore.
            }
            break;

        // -------------------------------------------------------------------
        case EParserState::CsiIntermediate:
            if (raw >= 0x20 && raw <= 0x2F) {
                CollectIntermediate(raw);
            } else if (raw >= 0x40 && raw <= 0x7E) {
                DispatchCsi(static_cast<char32_t>(raw), grid);
                TransitionTo(EParserState::Ground, grid);
            } else if (raw >= 0x30 && raw <= 0x3F) {
                // Param byte after intermediate → CsiIgnore.
                State_ = EParserState::CsiIgnore;
            }
            break;

        // -------------------------------------------------------------------
        case EParserState::CsiIgnore:
            if (raw >= 0x40 && raw <= 0x7E) {
                ++UnknownSeqCount_;
                TransitionTo(EParserState::Ground, grid);
            }
            break;

        // -------------------------------------------------------------------
        case EParserState::DcsEntry:
            // DCS → immediately bypass without accumulating params.
            InOsc_ = false;
            BypassBytes_ = 0;
            State_ = EParserState::kBypass;
            // Re-process this byte as a bypass payload.
            EmitBypass(b);
            break;

        // DcsParam, DcsIntermediate, DcsPassthrough, DcsIgnore are handled
        // via the kBypass shortcut above; these states are only reached if
        // a future refactor splits them.
        case EParserState::DcsParam:
        case EParserState::DcsIntermediate:
        case EParserState::DcsPassthrough:
        case EParserState::DcsIgnore:
            // Shouldn't normally land here.  Route everything to bypass.
            InOsc_ = false;
            BypassBytes_ = 0;
            State_ = EParserState::kBypass;
            EmitBypass(b);
            break;

        case EParserState::OscString:
        case EParserState::SosPmApcString:
            // Also reachable only in edge cases; route to bypass.
            State_ = EParserState::kBypass;
            EmitBypass(b);
            break;

        case EParserState::kBypass:
            // Handled at the top of FeedByte; cannot reach here.
            break;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void VtParser::ClearParams() noexcept { ParamLen_ = 0; }

void VtParser::AddParam(std::uint8_t ch) noexcept {
    if (ParamLen_ < MAX_PARAM_BYTES - 1)
        ParamBuf_[ParamLen_++] = static_cast<char>(ch);
    // Else: silently overflow (seq goes to ignore on dispatch if needed).
}

void VtParser::CollectIntermediate(std::uint8_t ch) noexcept {
    if (IntermediateCount_ < MAX_INTERMEDIATES)
        Intermediates_[IntermediateCount_++] = ch;
}

void VtParser::EmitBypass(std::byte byt) noexcept {
    if (BypassFn_) BypassFn_(byt, BypassCtx_);
}

void VtParser::TransitionTo(EParserState next, Grid& /*grid*/) noexcept {
    // Entry actions for certain states.
    switch (next) {
        case EParserState::Ground:
            ClearParams();
            IntermediateCount_ = 0;
            PrivateMode_ = false;
            BypassBytes_ = 0;
            InOsc_ = false;
            break;
        case EParserState::CsiEntry:
        case EParserState::DcsEntry:
            ClearParams();
            IntermediateCount_ = 0;
            PrivateMode_ = false;
            break;
        default:
            break;
    }
    State_ = next;
}

// ---------------------------------------------------------------------------
// C0 control handler (dispatched before the state machine).
// ---------------------------------------------------------------------------

void VtParser::HandleControl(std::uint8_t c, Grid& grid) noexcept {
    switch (c) {
        case 0x07:  // BEL
            break;
        case 0x08:  // BS
            grid.Backspace();
            break;
        case 0x09:  // HT
            grid.HorizontalTab();
            break;
        case 0x0A:  // LF
        case 0x0B:  // VT
        case 0x0C:  // FF
            grid.LineFeed();
            break;
        case 0x0D:  // CR
            grid.CarriageReturn();
            break;
        case 0x0E:  // SO / LS1
            grid.ShiftCharset(true);
            break;
        case 0x0F:  // SI / LS0
            grid.ShiftCharset(false);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// ParseParams: parse the accumulated param buffer.
// ---------------------------------------------------------------------------

std::size_t VtParser::ParseParams(std::uint16_t* out,
                                  std::size_t maxOut) const noexcept {
    // Skip the private-mode prefix byte ('?', '>', '<', '=') if present.
    std::size_t start = 0;
    if (ParamLen_ > 0) {
        const std::uint8_t first = static_cast<std::uint8_t>(ParamBuf_[0]);
        if (first >= 0x3C && first <= 0x3F) start = 1;
    }

    std::size_t count = 0;
    std::uint32_t acc = 0;
    bool hasDigit = false;

    for (std::size_t i = start; i < ParamLen_; ++i) {
        const std::uint8_t ch = static_cast<std::uint8_t>(ParamBuf_[i]);
        if (ch >= '0' && ch <= '9') {
            acc = acc * 10 + (ch - '0');
            if (acc > 0xFFFF) acc = 0xFFFF;  // clamp
            hasDigit = true;
        } else if (ch == ';' || ch == ':') {
            if (count < maxOut)
                out[count++] = static_cast<std::uint16_t>(hasDigit ? acc : 0);
            acc = 0;
            hasDigit = false;
        }
    }
    // Last (or only) param.
    if (count < maxOut)
        out[count++] = static_cast<std::uint16_t>(hasDigit ? acc : 0);

    return count;
}

// ---------------------------------------------------------------------------
// ESC dispatch
// ---------------------------------------------------------------------------

void VtParser::DispatchEsc(char32_t final, Grid& grid) noexcept {
    // With no intermediate bytes.
    if (IntermediateCount_ == 0) {
        switch (static_cast<std::uint8_t>(final)) {
            case '7':  // DECSC – save cursor
                grid.SaveCursor();
                return;
            case '8':  // DECRC – restore cursor
                grid.RestoreCursor();
                return;
            case 'D':  // IND – index (move down one line)
                grid.Index();
                return;
            case 'E':  // NEL – next line
                grid.CarriageReturn();
                grid.LineFeed();
                return;
            case 'H':  // HTS – set tab stop
                grid.SetTabStop();
                return;
            case 'M':  // RI – reverse index
                grid.ReverseIndex();
                return;
            case 'c':  // RIS – hard reset
                grid.Reset(/*hard=*/true);
                return;
            case '\\':  // ST – string terminator (ends bypass)
                // If we arrived here from kBypass (via the ESC 0x5C path),
                // the bypass state has already exited before calling Escape.
                // Nothing more to do.
                return;
            default:
                ++UnknownSeqCount_;
                return;
        }
    }

    // With one intermediate byte.
    if (IntermediateCount_ == 1) {
        const std::uint8_t inter = Intermediates_[0];
        const std::uint8_t fin = static_cast<std::uint8_t>(final);
        if (inter == '(') {
            // Designate G0 charset.
            grid.DesignateCharset(0, fin);
            return;
        }
        if (inter == ')') {
            // Designate G1 charset.
            grid.DesignateCharset(1, fin);
            return;
        }
    }
    ++UnknownSeqCount_;
}

// ---------------------------------------------------------------------------
// CSI dispatch
// ---------------------------------------------------------------------------

void VtParser::DispatchCsi(char32_t final, Grid& grid) noexcept {
    std::uint16_t params[MAX_PARAMS]{};
    const std::size_t nparams = ParseParams(params, MAX_PARAMS);

    // Helper: first param or default.
    auto P = [&](std::size_t idx, std::uint16_t def) noexcept -> std::uint16_t {
        if (idx < nparams && (params[idx] > 0 || idx < nparams))
            return params[idx] > 0 ? params[idx] : def;
        return def;
    };
    // Helper: first param, 0 if absent (no default substitution).
    auto P0 = [&](std::size_t idx) noexcept -> std::uint16_t {
        return idx < nparams ? params[idx] : 0;
    };

    // Private-mode CSI ('?' or '>').
    if (PrivateMode_) {
        if (static_cast<std::uint8_t>(final) == 'h') {
            // DECSET
            for (std::size_t i = 0; i < nparams; ++i)
                grid.SetMode(params[i], true);
            return;
        }
        if (static_cast<std::uint8_t>(final) == 'l') {
            // DECRST
            for (std::size_t i = 0; i < nparams; ++i)
                grid.SetMode(params[i], false);
            return;
        }
        ++UnknownSeqCount_;
        return;
    }

    switch (static_cast<std::uint8_t>(final)) {
        // Cursor movement.
        case 'A':  // CUU – cursor up
            grid.MoveCursorUp(P(0, 1));
            break;
        case 'B':  // CUD – cursor down
            grid.MoveCursorDown(P(0, 1));
            break;
        case 'C':  // CUF – cursor forward (right)
            grid.MoveCursorRight(P(0, 1));
            break;
        case 'D':  // CUB – cursor back (left)
            grid.MoveCursorLeft(P(0, 1));
            break;
        case 'E':  // CNL – cursor next line
            grid.CursorNextLine(P(0, 1));
            break;
        case 'F':  // CPL – cursor preceding line
            grid.CursorPrevLine(P(0, 1));
            break;
        case 'G':  // CHA – cursor horizontal absolute
            grid.MoveCursorToCol(P(0, 1));
            break;
        case 'H':  // CUP – cursor position
        case 'f':  // HVP – horizontal vertical position (same as CUP)
            grid.MoveCursorTo(P(0, 1), P(1, 1));
            break;
        case 'd':  // VPA – vertical line position absolute
            grid.MoveCursorToRow(P(0, 1));
            break;

        // Erase.
        case 'J':  // ED – erase in display
            grid.EraseInDisplay(static_cast<std::uint8_t>(P0(0)));
            break;
        case 'K':  // EL – erase in line
            grid.EraseInLine(static_cast<std::uint8_t>(P0(0)));
            break;
        case 'X':  // ECH – erase character
            grid.EraseChars(P(0, 1));
            break;

        // Insert / delete.
        case 'L':  // IL – insert lines
            grid.InsertLines(P(0, 1));
            break;
        case 'M':  // DL – delete lines
            grid.DeleteLines(P(0, 1));
            break;
        case 'P':  // DCH – delete character
            grid.DeleteChars(P(0, 1));
            break;
        case '@':  // ICH – insert character
            grid.InsertChars(P(0, 1));
            break;

        // Scroll.
        case 'S':  // SU – scroll up
            grid.ScrollUp(P(0, 1));
            break;
        case 'T':  // SD – scroll down
            grid.ScrollDown(P(0, 1));
            break;

        // SGR.
        case 'm':  // SGR – select graphic rendition
            grid.ApplySgr(params, nparams);
            break;

        // Modes (non-private).
        case 'h':  // SM – set mode
            for (std::size_t i = 0; i < nparams; ++i) {
                if (params[i] == 4) grid.SetMode(4, true);    // IRM
                if (params[i] == 20) grid.SetMode(20, true);  // LNM
            }
            break;
        case 'l':  // RM – reset mode
            for (std::size_t i = 0; i < nparams; ++i) {
                if (params[i] == 4) grid.SetMode(4, false);
                if (params[i] == 20) grid.SetMode(20, false);
            }
            break;

        // Scroll region.
        case 'r':  // DECSTBM – set top and bottom margins
            grid.SetScrollRegion(P(0, 1),
                                 P(1, static_cast<std::uint16_t>(grid.Rows())));
            break;

        // Tab stops.
        case 'g':  // TBC – tab clear
            grid.ClearTabStop(static_cast<std::uint8_t>(P0(0)));
            break;

        // Cursor save/restore (ANSI).
        case 's':  // SCP – save cursor position
            grid.SaveCursor();
            break;
        case 'u':  // RCP – restore cursor position
            grid.RestoreCursor();
            break;

        // Soft reset.
        case 'p':  // DECSTR (only with intermediate '!')
            if (IntermediateCount_ == 1 && Intermediates_[0] == '!') {
                grid.Reset(/*hard=*/false);
            } else {
                ++UnknownSeqCount_;
            }
            break;

        // Cursor style.
        case 'q':  // DECSCUSR – set cursor style (intermediate ' ')
            if (IntermediateCount_ == 1 && Intermediates_[0] == ' ') {
                grid.SetCursorStyle(static_cast<std::uint8_t>(P0(0)));
            } else {
                ++UnknownSeqCount_;
            }
            break;

        // Repeat char.
        case 'b':  // REP – repeat preceding character
            grid.RepeatChar(P(0, 1));
            break;

        // Window ops / device attributes – ignore silently (not tracked).
        case 'c':  // DA1/DA2
        case 'n':  // DSR
            break;

        default:
            ++UnknownSeqCount_;
            break;
    }
}

}  // namespace zet::grid
