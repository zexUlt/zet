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
    PrivateMarker_ = 0;
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
    if (State_ == EParserState::Bypass) {
        // CAN (0x18) / SUB (0x1A) abort the sequence.
        if (raw == 0x18 || raw == 0x1A) {
            EmitBypassEnd();
            BypassBytes_ = 0;
            InOsc_ = false;
            State_ = EParserState::Ground;
            return;
        }
        // ESC starts potential ST (ESC \).
        if (raw == 0x1B) {
            // Remember ESC; next byte decides.
            EmitBypassEnd();
            BypassBytes_ = 0;
            InOsc_ = false;
            State_ = EParserState::Escape;
            return;
        }
        // BEL terminates OSC only.
        if (raw == 0x07 && InOsc_) {
            EmitBypassEnd();
            BypassBytes_ = 0;
            InOsc_ = false;
            State_ = EParserState::Ground;
            return;
        }
        // Enforce bypass limit (design.md §9).
        ++BypassBytes_;
        if (BypassBytes_ >= BYPASS_LIMIT_BYTES) {
            EmitBypassEnd();
            BypassBytes_ = 0;
            InOsc_ = false;
            State_ = EParserState::Ground;
            // Signal that the parser lost sync: the output of the pty
            // may have diverged from what we tracked.
            grid.SetPossiblyDiverged(true);
            ++BypassLimitCount_;
            return;
        }
        // DECRQSS and XTGETTCAP: `$ q` or `+ q` opens the string.
        if (BypassBytes_ == 1 && !InOsc_ && raw == 'q' &&
            (DcsHead_ == '$' || DcsHead_ == '+')) {
            ++DcsQueryCount_;
        }
        EmitBypassByte(b);
        return;
    }

    // -----------------------------------------------------------------------
    // Handle C0 controls that can interrupt any state.
    // -----------------------------------------------------------------------
    // CAN (0x18) / SUB (0x1A): abort current sequence, return to ground.
    if (raw == 0x18 || raw == 0x1A) {
        if (Utf8Remaining_ > 0) CountUnknown(grid);  // a cut-off character
        Utf8Remaining_ = 0;
        TransitionTo(EParserState::Ground, grid);
        // SUB traditionally prints a substitution character (U+FFFD).
        if (raw == 0x1A) grid.PutChar(0xFFFD, 1);
        return;
    }

    // ESC: start/restart escape processing (flushes pending UTF-8).
    if (raw == 0x1B) {
        if (Utf8Remaining_ > 0) CountUnknown(grid);  // a cut-off character
        Utf8Remaining_ = 0;
        ClearParams();
        IntermediateCount_ = 0;
        PrivateMarker_ = 0;
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
                // Per-length overlong minimums (RFC 3629 §3).
                static constexpr char32_t MIN_CP[] = {0, 0x80, 0x800, 0x10000};
                const std::uint8_t sl = Utf8SeqLen_;
                if (cp < MIN_CP[sl < 4 ? sl : 3] || cp > 0x10FFFF ||
                    (cp >= 0xD800 && cp <= 0xDFFF)) {
                    cp = 0xFFFD;
                    // Terminals draw U+FFFD for it, one for the sequence or
                    // one per byte: the mirror draws one, and the column
                    // after it is not certain.
                    CountUnknown(grid);
                }
                // C1 controls (U+0080–U+009F): do not store as cell content.
                //
                // VTE acts on them when they come decoded from UTF-8 (U+009B
                // as CSI, U+009D as OSC); xterm in UTF-8 mode does not. The
                // mirror does not execute them and cannot tell which terminal
                // it is talking to, so they count as a sequence it does not
                // know. C1 ST (U+009C) still does not end an open string: that
                // terminator is ESC \.
                if (cp >= 0x0080 && cp <= 0x009F) {
                    CountUnknown(grid);
                    return;
                }
                if (State_ == EParserState::Ground) {
                    const int w = Wcwidth(cp);
                    // Zero-width combining marks (w==0) attach to prev cell.
                    grid.PutChar(cp, w >= 0 ? w : 1);
                }
                // In other states, printable high codepoints are ignored per
                // Paul Williams' spec (we are not in Ground).
            }
            return;
        } else {
            // Invalid continuation: emit replacement and re-process this byte.
            if (State_ == EParserState::Ground) grid.PutChar(0xFFFD, 1);
            Utf8Remaining_ = 0;
            CountUnknown(grid);
        }
    }

    // Start a UTF-8 multi-byte sequence (only in Ground state).
    if (raw >= 0xC0 && raw <= 0xFF && State_ == EParserState::Ground) {
        const std::uint8_t extra = Utf8LeadLength(raw);
        if (extra > 0) {
            // Strip lead bits: 2-byte=0x1F, 3-byte=0x0F, 4-byte=0x07.
            static constexpr std::uint8_t MASK[] = {0, 0x1F, 0x0F, 0x07};
            Utf8Accumulator_ = raw & MASK[extra];
            Utf8Remaining_ = extra;
            Utf8SeqLen_ = extra;
            return;
        }
        // 0xF8-0xFF are invalid: emit replacement.
        grid.PutChar(0xFFFD, 1);
        CountUnknown(grid);
        return;
    }

    // A continuation byte with no lead before it in Ground, or any 8-bit byte
    // inside an escape sequence: terminals differ in what they do with it
    // (draw U+FFFD, ignore it, end the sequence), the mirror ignores it.
    if (raw >= 0x80) {
        CountUnknown(grid);
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
                PrivateMarker_ = 0;
                State_ = EParserState::CsiEntry;
            } else if (raw == 0x5D) {  // ']' → OSC
                InOsc_ = true;
                BypassBytes_ = 0;
                DcsHead_ = 0;
                State_ = EParserState::Bypass;
                EmitBypassStart();
            } else if (raw == 0x58 || raw == 0x5E || raw == 0x5F) {
                // SOS (0x58), PM (0x5E), APC (0x5F) → bypass
                InOsc_ = false;
                BypassBytes_ = 0;
                DcsHead_ = 0;
                State_ = EParserState::Bypass;
                EmitBypassStart();
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
                PrivateMarker_ = raw;
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
                grid.NoteUnknownSequence();
                TransitionTo(EParserState::Ground, grid);
            }
            break;

        // -------------------------------------------------------------------
        case EParserState::DcsEntry:
            // DCS → immediately bypass without accumulating params.
            InOsc_ = false;
            BypassBytes_ = 0;
            DcsHead_ = raw;
            State_ = EParserState::Bypass;
            EmitBypassStart();
            // Re-process this byte as a bypass payload.
            EmitBypassByte(b);
            break;

        // DcsParam, DcsIntermediate, DcsPassthrough, DcsIgnore are handled
        // via the Bypass shortcut above; these states are only reached if
        // a future refactor splits them.
        case EParserState::DcsParam:
        case EParserState::DcsIntermediate:
        case EParserState::DcsPassthrough:
        case EParserState::DcsIgnore:
            // Shouldn't normally land here.  Route everything to bypass.
            InOsc_ = false;
            BypassBytes_ = 0;
            DcsHead_ = 0;
            State_ = EParserState::Bypass;
            EmitBypassStart();
            EmitBypassByte(b);
            break;

        case EParserState::OscString:
        case EParserState::SosPmApcString:
            // Also reachable only in edge cases; route to bypass.
            State_ = EParserState::Bypass;
            EmitBypassStart();
            EmitBypassByte(b);
            break;

        case EParserState::Bypass:
            // Handled at the top of FeedByte; cannot reach here.
            break;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void VtParser::CountUnknown(Grid& grid) noexcept {
    ++UnknownSeqCount_;
    grid.NoteUnknownSequence();
}

void VtParser::ClearParams() noexcept { ParamLen_ = 0; }

void VtParser::AddParam(std::uint8_t ch) noexcept {
    if (ParamLen_ < MAX_PARAM_BYTES - 1)
        ParamBuf_[ParamLen_++] = static_cast<char>(ch);
    // Else: silently overflow (seq goes to ignore on dispatch if needed).
}

bool VtParser::HasMidParamMarker() const noexcept {
    // xterm's parser takes 0x3C-0x3F after a parameter byte as an error and
    // ignores the sequence (`CSI 0 ? c` gets no reply).
    for (std::size_t i = 1; i < ParamLen_; ++i) {
        const auto ch = static_cast<std::uint8_t>(ParamBuf_[i]);
        if (ch >= 0x3C && ch <= 0x3F) {
            return true;
        }
    }
    return false;
}

void VtParser::CollectIntermediate(std::uint8_t ch) noexcept {
    if (IntermediateCount_ < MAX_INTERMEDIATES)
        Intermediates_[IntermediateCount_++] = ch;
}

void VtParser::EmitBypassByte(std::byte byt) noexcept {
    if (BypassFn_) BypassFn_(EBypassEvent::Byte, byt, BypassCtx_);
}

void VtParser::EmitBypassStart() noexcept {
    if (!InOsc_) ++DcsApcCount_;
    if (BypassFn_) BypassFn_(EBypassEvent::Start, std::byte{0}, BypassCtx_);
}

void VtParser::EmitBypassEnd() noexcept {
    if (BypassFn_) BypassFn_(EBypassEvent::End, std::byte{0}, BypassCtx_);
}

void VtParser::TransitionTo(EParserState next, Grid& /*grid*/) noexcept {
    // Entry actions for certain states.
    switch (next) {
        case EParserState::Ground:
            ClearParams();
            IntermediateCount_ = 0;
            PrivateMarker_ = 0;
            BypassBytes_ = 0;
            InOsc_ = false;
            break;
        case EParserState::CsiEntry:
        case EParserState::DcsEntry:
            ClearParams();
            IntermediateCount_ = 0;
            PrivateMarker_ = 0;
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

std::size_t VtParser::ParseParams(std::uint16_t* out, bool* colon,
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
    bool prevColon = false;  // separator before the next value

    for (std::size_t i = start; i < ParamLen_; ++i) {
        const std::uint8_t ch = static_cast<std::uint8_t>(ParamBuf_[i]);
        if (ch >= '0' && ch <= '9') {
            acc = acc * 10 + (ch - '0');
            if (acc > 0xFFFF) acc = 0xFFFF;  // clamp
            hasDigit = true;
        } else if (ch == ';' || ch == ':') {
            if (count < maxOut) {
                out[count] = static_cast<std::uint16_t>(hasDigit ? acc : 0);
                colon[count] = prevColon;
                ++count;
            }
            prevColon = (ch == ':');
            acc = 0;
            hasDigit = false;
        }
    }
    // Last (or only) param.
    if (count < maxOut) {
        out[count] = static_cast<std::uint16_t>(hasDigit ? acc : 0);
        colon[count] = prevColon;
        ++count;
    }

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
            case '=':  // DECKPAM – application keypad: no effect on the screen
            case '>':  // DECKPNM – numeric keypad: the same
                return;
            case 'Z':  // DECID – xterm answers it as DA1; counted, not acted on
                ++Da1QueryCount_;
                // Still an unknown sequence for the mirror, as before the
                // DA1 count existed. The cursor stays: the path goes on.
                ++UnknownSeqCount_;
                return;
            case '\\':  // ST – string terminator (ends bypass)
                // If we arrived here from Bypass (via the ESC 0x5C path),
                // the bypass state has already exited before calling Escape.
                // Nothing more to do.
                return;
            default:
                ++UnknownSeqCount_;
                grid.NoteUnknownSequence();
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
    grid.NoteUnknownSequence();
}

// ---------------------------------------------------------------------------
// CSI dispatch
// ---------------------------------------------------------------------------

bool VtParser::CsiFormHandled(char32_t final, const bool* colon,
                              std::size_t nparams) const noexcept {
    const auto fin = static_cast<std::uint8_t>(final);
    // xterm takes a marker after a parameter for an error and ignores the
    // sequence.
    if (HasMidParamMarker()) return false;
    if (PrivateMarker_ != 0) {
        return PrivateMarker_ == '?' && IntermediateCount_ == 0 &&
               (fin == 'h' || fin == 'l');
    }
    if (IntermediateCount_ != 0) {
        if (IntermediateCount_ != 1) return false;
        return (Intermediates_[0] == '!' && fin == 'p') ||
               (Intermediates_[0] == ' ' && fin == 'q');
    }
    // A colon is SGR's sub-parameter separator; elsewhere terminals differ in
    // what it does to the parameters.
    if (fin != 'm') {
        for (std::size_t i = 0; i < nparams; ++i) {
            if (colon[i]) return false;
        }
    }
    // `CSI Pl ; Pr s` is DECSLRM when left and right margins are on, which the
    // mirror does not model; only the bare form is SCP.
    if (fin == 's' && ParamLen_ != 0) return false;
    return true;
}

void VtParser::DispatchCsi(char32_t final, Grid& grid) noexcept {
    std::uint16_t params[MAX_PARAMS]{};
    bool colon[MAX_PARAMS]{};
    const std::size_t nparams = ParseParams(params, colon, MAX_PARAMS);

    // Helper: first param or default.
    auto p = [&](std::size_t idx, std::uint16_t def) noexcept -> std::uint16_t {
        if (idx < nparams && (params[idx] > 0 || idx < nparams))
            return params[idx] > 0 ? params[idx] : def;
        return def;
    };
    // Helper: first param, 0 if absent (no default substitution).
    auto p0 = [&](std::size_t idx) noexcept -> std::uint16_t {
        return idx < nparams ? params[idx] : 0;
    };

    // The mirror executes a sequence only in a form it handles. One with an
    // intermediate, a private marker, a colon or a stray marker among the
    // parameters that it does not specifically handle goes to the unknown
    // path, not to the handler of its final byte: a terminal ignores `CSI 1 SP
    // C` (GSS), and the handler of C would move the cursor.
    if (!CsiFormHandled(final, colon, nparams)) {
        CountUnknown(grid);
        return;
    }

    // Private-mode CSI: only DECSET and DECRST with '?' get here.
    if (PrivateMarker_ != 0) {
        const bool value = static_cast<std::uint8_t>(final) == 'h';
        // DECSET/DECRST skip params 4 (DECSCLM) and 20 (undefined private):
        // IRM and LNM are ANSI modes set by non-private SM/RM (D6).
        for (std::size_t i = 0; i < nparams; ++i) {
            if (params[i] == 4 || params[i] == 20) continue;
            // Modes that move the cursor or wrap the line in ways the grid
            // does not model (EUnmodelledMode), and DECANM, which leaves for
            // VT52 mode (its escape sequences are not these): the change is
            // a sequence the mirror does not know.
            if (params[i] == 2 || params[i] == 3 || params[i] == 45 ||
                params[i] == 69) {
                CountUnknown(grid);
            }
            grid.SetMode(params[i], value);
        }
        return;
    }

    switch (static_cast<std::uint8_t>(final)) {
        // Cursor movement.
        case 'A':  // CUU – cursor up
            grid.MoveCursorUp(p(0, 1));
            break;
        case 'B':  // CUD – cursor down
            grid.MoveCursorDown(p(0, 1));
            break;
        case 'C':  // CUF – cursor forward (right)
            grid.MoveCursorRight(p(0, 1));
            break;
        case 'D':  // CUB – cursor back (left)
            grid.MoveCursorLeft(p(0, 1));
            break;
        case 'E':  // CNL – cursor next line
            grid.CursorNextLine(p(0, 1));
            break;
        case 'F':  // CPL – cursor preceding line
            grid.CursorPrevLine(p(0, 1));
            break;
        case 'G':  // CHA – cursor horizontal absolute
            grid.MoveCursorToCol(p(0, 1));
            break;
        case 'H':  // CUP – cursor position
        case 'f':  // HVP – horizontal vertical position (same as CUP)
            grid.MoveCursorTo(p(0, 1), p(1, 1));
            break;
        case 'I': {  // CHT – cursor horizontal forward tab (n stops)
            const std::uint16_t n = p(0, 1);
            for (std::uint16_t t = 0; t < n; ++t) grid.HorizontalTab();
            break;
        }
        case 'Z':  // CBT – cursor backward tab
            grid.HorizontalTabBack(p(0, 1));
            break;
        case 'a':  // HPR – horizontal position relative
            grid.MoveCursorRight(p(0, 1));
            break;
        case 'e':  // VPR – vertical position relative
            grid.MoveCursorDown(p(0, 1));
            break;
        case 'd':  // VPA – vertical line position absolute
            grid.MoveCursorToRow(p(0, 1));
            break;

        // Erase.
        case 'J':  // ED – erase in display
            grid.EraseInDisplay(static_cast<std::uint8_t>(p0(0)));
            break;
        case 'K':  // EL – erase in line
            grid.EraseInLine(static_cast<std::uint8_t>(p0(0)));
            break;
        case 'X':  // ECH – erase character
            grid.EraseChars(p(0, 1));
            break;

        // Insert / delete.
        case 'L':  // IL – insert lines
            grid.InsertLines(p(0, 1));
            break;
        case 'M':  // DL – delete lines
            grid.DeleteLines(p(0, 1));
            break;
        case 'P':  // DCH – delete character
            grid.DeleteChars(p(0, 1));
            break;
        case '@':  // ICH – insert character
            grid.InsertChars(p(0, 1));
            break;

        // Scroll.
        case 'S':  // SU – scroll up
            grid.ScrollUp(p(0, 1));
            break;
        case 'T':  // SD – scroll down
            grid.ScrollDown(p(0, 1));
            break;

        // SGR.
        case 'm':  // SGR – select graphic rendition
            grid.ApplySgr(params, colon, nparams);
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
            if (IntermediateCount_ == 0 && PrivateMarker_ == 0) {
                grid.SetScrollRegion(
                    p(0, 1), p(1, static_cast<std::uint16_t>(grid.Rows())));
            } else {
                ++UnknownSeqCount_;
                grid.NoteUnknownSequence();
            }
            break;

        // Tab stops.
        case 'g':  // TBC – tab clear
            grid.ClearTabStop(static_cast<std::uint8_t>(p0(0)));
            break;

        // Cursor save/restore (ANSI).
        case 's':  // SCP – save cursor position (CsiFormHandled: no params)
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
                grid.NoteUnknownSequence();
            }
            break;

        // Cursor style.
        case 'q':  // DECSCUSR – set cursor style (intermediate ' ')
            if (IntermediateCount_ == 1 && Intermediates_[0] == ' ') {
                grid.SetCursorStyle(static_cast<std::uint8_t>(p0(0)));
            } else {
                ++UnknownSeqCount_;
                grid.NoteUnknownSequence();
            }
            break;

        // Repeat char.
        case 'b':  // REP – repeat preceding character
            if (!grid.RepeatChar(p(0, 1))) {
                ++UnknownSeqCount_;  // nothing to repeat
                grid.NoteUnknownSequence();
            }
            break;

        // Window ops / device attributes – ignore silently (not tracked).
        // They are not answered here, but DA1 is counted: the sender of the
        // bytes (the client's query demultiplexer) needs to know a terminal
        // will reply. Only the plain forms count: DA2 (CSI > c) and DA3
        // (CSI = c) never reach this switch, they carry a private marker.
        case 'c':  // DA1; xterm looks only at the first parameter
            if (IntermediateCount_ == 0 && (nparams == 0 || params[0] == 0) &&
                !HasMidParamMarker()) {
                ++Da1QueryCount_;
            }
            break;
        case 'n':  // DSR; the terminal answers 6 with the cursor's position
            if (IntermediateCount_ == 0 && nparams == 1 && params[0] == 6) {
                ++CprQueryCount_;
            }
            break;

        default:
            ++UnknownSeqCount_;
            grid.NoteUnknownSequence();
            break;
    }
}

}  // namespace zet::grid
