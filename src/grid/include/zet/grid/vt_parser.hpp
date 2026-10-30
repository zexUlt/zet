#pragma once

#include <cstddef>
#include <cstdint>

#include "zet/grid/limits.hpp"

namespace zet::grid {

class Grid;

/// VT parser states (Paul Williams' DEC ANSI state machine, design.md §9).
///
/// 14 states matching the published diagram.  Bypass is not an official
/// state name but captures the DCS/OSC/APC/PM/SOS pass-through behaviour:
/// bytes are forwarded to the caller without accumulation until ST is seen.
enum class EParserState : std::uint8_t {
    Ground,
    Escape,
    EscapeIntermediate,
    CsiEntry,
    CsiParam,
    CsiIntermediate,
    CsiIgnore,
    DcsEntry,
    DcsParam,
    DcsIntermediate,
    DcsPassthrough,
    DcsIgnore,
    OscString,
    SosPmApcString,
    /// Bypass: DCS/OSC/APC/PM/SOS content streaming without accumulation.
    /// Exits to Ground on ST (ESC \), BEL (in OSC only), or CAN/SUB/ESC.
    Bypass,
};

/// A single parsed action (what the parser dispatches to the Grid).
/// The full call-dispatch happens inside VtParser::Feed; this struct
/// is exposed for introspection from tests / fuzz targets.
enum class EAction : std::uint8_t {
    None,
    Print,    ///< printable character
    Execute,  ///< C0/C1 control character
    Hook,     ///< DCS hook (enter DCS pass-through)
    Put,      ///< DCS pass-through byte
    Unhook,   ///< DCS unhook (leave DCS pass-through)
    OscStart,
    OscPut,
    OscEnd,
    CsiDispatch,  ///< final byte of a CSI sequence
    EscDispatch,  ///< final byte of an ESC sequence
    Collect,      ///< intermediate byte
    Param,        ///< numeric/sub-param byte
    Clear,        ///< clear params
    Ignore,
};

/// Event kind passed to the bypass callback (design.md §9).
enum class EBypassEvent : std::uint8_t {
    Start,  ///< DCS/OSC/APC/PM/SOS sequence begins; byte is the introducer.
    Byte,   ///< One payload byte.
    End,    ///< Sequence ended normally (ST, BEL for OSC) or was aborted.
};

/// The VT parser (sans-I/O, feeds bytes into a Grid).
///
/// Implements Paul Williams' DEC ANSI state machine with the following
/// additions:
///   - UTF-8 decoding: multibyte sequences are assembled and dispatched
///     as a single Print action with the decoded codepoint.
///   - Bypass: DCS/OSC/APC/PM/SOS content is forwarded without accumulation.
///     The BypassCallback, if set, receives Start, Byte, and End events.
///   - UnknownSeqCount: monotonically incremented for unrecognised sequences.
///
/// Usage:
///     VtParser parser;
///     parser.Feed(bytes, length, grid);
class VtParser {
public:
    VtParser() noexcept;

    /// Optional callback invoked for bypass events (DCS/OSC/APC/PM/SOS).
    /// Receives Start on sequence entry, Byte for each payload byte, End on
    /// termination (ST/BEL/CAN/SUB/ESC or bypass-limit expiry).
    /// The byte argument for Start and End events is 0.
    using BypassFn = void (*)(EBypassEvent, std::byte, void*);
    void SetBypassCallback(BypassFn fn, void* ctx) noexcept {
        BypassFn_ = fn;
        BypassCtx_ = ctx;
    }

    /// Feed raw bytes from the pty into the parser.  Calls Grid mutators
    /// as sequences are recognised.  May be called with any number of bytes
    /// at a time; state persists across calls.
    void Feed(const std::byte* data, std::size_t length, Grid& grid) noexcept;

    /// Feed a single byte (convenience).
    void FeedByte(std::byte b, Grid& grid) noexcept;

    // -----------------------------------------------------------------------
    // State inspection (tests / fuzz invariants)
    // -----------------------------------------------------------------------

    [[nodiscard]] EParserState State() const noexcept { return State_; }

    /// Number of unrecognised sequences since construction / last Reset.
    [[nodiscard]] std::uint64_t UnknownSeqCount() const noexcept {
        return UnknownSeqCount_;
    }

    /// Byte counter in the current bypass sequence.  Resets at each new
    /// DCS/OSC/APC/PM/SOS.  Reaching BYPASS_LIMIT_BYTES forces Ground.
    [[nodiscard]] std::size_t BypassBytes() const noexcept {
        return BypassBytes_;
    }

    /// Hard-reset the parser to Ground state (e.g. on reconnect or
    /// GROUND_WAIT_TIMEOUT).
    void Reset() noexcept;

private:
    // Process one byte already decoded from UTF-8 (codepoint < 0x80) or
    // the full codepoint for multibyte chars.
    void ProcessCodepoint(char32_t cp, Grid& grid) noexcept;

    // Dispatch the current CSI/ESC/DCS sequence to the Grid.
    void DispatchCsi(char32_t finalByte, Grid& grid) noexcept;
    void DispatchEsc(char32_t finalByte, Grid& grid) noexcept;

    // Helpers.
    void TransitionTo(EParserState next, Grid& grid) noexcept;
    void ClearParams() noexcept;
    void AddParam(std::uint8_t ch) noexcept;
    void CollectIntermediate(std::uint8_t ch) noexcept;
    void EmitBypassByte(std::byte b) noexcept;
    void EmitBypassStart() noexcept;
    void EmitBypassEnd() noexcept;
    void HandleControl(std::uint8_t c, Grid& grid) noexcept;

    /// Parse collected numeric params into an array of uint16 values.
    /// Returns count.  colon[k] is true when param k was preceded by ':',
    /// meaning it is a sub-param of the previous entry (not a standalone SGR).
    /// Callers use this to distinguish "4:3" (underline style 3) from "4;3"
    /// (underline then italic).
    std::size_t ParseParams(std::uint16_t* out, bool* colon,
                            std::size_t maxOut) const noexcept;

    // -----------------------------------------------------------------------
    // UTF-8 decoder state
    // -----------------------------------------------------------------------
    std::uint32_t Utf8Accumulator_{0};
    std::uint8_t Utf8Remaining_{0};
    std::uint8_t Utf8SeqLen_{0};  ///< extra bytes in current sequence (1,2,3)

    // -----------------------------------------------------------------------
    // Parser state
    // -----------------------------------------------------------------------
    EParserState State_{EParserState::Ground};

    // Param/intermediate buffers (bounded per limits.hpp)
    char ParamBuf_[MAX_PARAM_BYTES]{};
    std::size_t ParamLen_{0};

    std::uint8_t Intermediates_[MAX_INTERMEDIATES]{};
    std::size_t IntermediateCount_{0};

    std::uint8_t PrivateMarker_{
        0};  ///< DEC private marker byte (0x3C–0x3F), 0 = none

    // Bypass state
    bool InOsc_{false};  ///< true = OSC (accepts BEL terminator)
    std::size_t BypassBytes_{0};

    // Callbacks for bypass bytes
    BypassFn BypassFn_{nullptr};
    void* BypassCtx_{nullptr};

    // Statistics
    std::uint64_t UnknownSeqCount_{0};
};

}  // namespace zet::grid
